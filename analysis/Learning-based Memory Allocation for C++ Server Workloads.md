这是一个基于学习的适用于C++ 服务器的内存分配器

设计原因: C/C++ 服务器, 使用 2MB的 巨页可以有效的减少 TLB Miss,现代高性能服务在生产环境中通常会开启巨页（Huge Pages）来加速。但是这样也会导致 fragmentation 的问题
Llama 的解法：Llama 既想要巨页带来的 TLB 高性能（2MB 连续物理内存），又想解决巨页引起的碎片问题。

background : 传统的 C/C++ 内存分配器（如 TCMalloc、jemalloc）通常是根据对象大小（Size Classes）来组织堆内存的。然而，这会导致严重的内存碎片化问题（特别是当配合操作系统的大内存页 —— 巨页使用时）。于是这个使用的是life class(LC) ,来管理,将对象分类为按数量级区分的生命周期类别：≤ 10毫秒、100毫秒、1秒、10秒:

innovative point: 
1. 基于循环神经网络（LSTM）的生命周期预测器：把程序的堆栈轨迹（Stack Traces）中的符号看作自然语言中的“词汇”。利用语言模型训练 LSTM，使其能够根据分配时的调用栈，跨版本、跨配置地准确预测对象属于哪个生命周期类别（LC）（例如：$\le 10$ 毫秒、100 毫秒、1 秒、10 秒等数量级）。

2. 只使用操作系统的大页（Huge Pages），并将其细分为 8 KB 的块（Blocks） 和 128 B 的线（Lines）。

3. 容错与动态调整机制
    由于预测不可能百分之百准确，Llama 设计了一套机制来处理预测错误。通过设定“截止时间（Deadline）”，当预测过长或过短时，动态提升或降低巨页的生命周期级别，从而在回收内存的同时控制碎片化。

分层堆组织结构：采用 巨页 (Huge Page) $\rightarrow$ 块 (Block) $\rightarrow$ 线 (Line) 的分层结构

Invariants:
    一个块只是填充一种预测生命周期的类别.
    块的生命周期只会容忍或者过预测其内部的块

误区恢复与动态调整:
    如果到了deadline 还有存活的对象,那么说明预测时间太短了,会整体调高巨页的生命周期
    相反的: 如果到了deadline 所有的 对象都已经被释放了,那么说明了:Deadline 太长了,需要变短

Llama 特点是只用巨页,不会把巨页拆分为更小的单位研究,细分为 8KB 的块也不过是一个管理方式,并没有真正的split.
while you may have one question here : 好像这个 细分为 8KB 的块和一般的split 为 4KB 页的处理好像没有什么区别啊??
然而不是这样的:

传统 Split（撕裂巨页）：

    操作系统把 2MB 巨页拆成 512 个 4KB 页。
    代价： 失去了巨页最大的优势 —— TLB 硬件缓存命中率。因为原本只需要 1 个 TLB 表项就能映射 2MB 内存，拆分后变成了需要 512 个 TLB 表项，造成严重的 CPU 性能下降（TLB Miss 增加）。

然而巨页本身也有缺点(这也是为什么传统的allocator 不使用巨页):

如果分配了一个2MB的巨页:
里面可能混杂装了一个活 10 分钟的对象和一百个活 1 毫秒的对象。
1 毫秒后，那一百个对象死了，但那个活 1 分钟的对象还在。
因为只有 1 个对象还活着，整个 2MB 的巨页就无法归还给操作系统，造成 99% 的空间浪费（这就是巨页膨胀/内存碎片）。

传统分配器为了解决这个问题，往往只能强制把这个巨页 Split 成小页，把没用的 4KB 回收。

但是llama 不同: 通过神经网络预测, 让生命周期相同的到一个页里面,实现了2MB的快速回收.(同时也有动态调整实现预测容错)

同时, 论文中给的figure 2 指出: 对象生命周期的分布呈现出长尾分布. 长生命周期的分布是稀有的,也就是混在短生命周期之中,这会带来之前提到的碎片率问题.
    "The vast majority of objects are short-lived, but rare long-lived objects impact fragmentation."
![alt text](<Screenshot from 2026-07-23 10-59-42.png>)

Workloads with varying memory footprint are more susceptible to this problem because small numbers of long-lived objects on a huge page prevent reusing it for large allocations

#### Lifetime Prediction Challenges

Overheads : time cost is a problem.

| | |
| :--- | :--- |
| TCMalloc Fast Path (new/delete) | 8.3 ns |
| TCMalloc Slow Path (central list) | 81.7 ns |
| Capture full stack trace | 396 ns ± 364 ns |
| Look up stack hash (Section 7) | 22.5 ns |

Table 1

这个表格说明了 预测的 overhead 是巨大的.Llama 的解决方法是: "We solve these problems by using stack heightand object size for per-site prediction and cache lookups."

覆盖率与准确率不足
    在我们例子中的工作负载下，64% 的不同分配上下文只被看到过一次，并且 17% 的永久存活内存分配（即那些永远不会被释放的对象）来自于那些只遇到过一次的上下文。
    如果系统没能识别出它们，把它们和短命对象混在一起，就会引发灾难性的内存碎片.
    而传统的PGO 优化无法用于预测, 他们的难度是不一样的.

    传统 PGO 的原理：先让程序跑一遍，把所有的执行路径和数据搜集齐（Profile），下一次编译时针对这些数据进行极限优化。

problems :
在线实时收集：遇到大量“只出现一次”的罕见上下文，覆盖率不够，容易漏掉关键的长寿对象。
传统离线 PGO：由于生命周期数据需要配对（分配+释放）且高度依赖动态上下文，数据收集极为困难，无法简单套用旧方法。

instablity:
    解决方案是: 
    "We solve coverage and instability problems by enhancing PGO to work without observing all contexts"
    Llama 不去死板地匹配整个栈长得一模一样，而是把栈里的方法名拆成“词汇（tokens）”，喂给 LSTM 神经网络。即使换了编译器、升级了库、或者遇到了新版本中从未见过的调用组合，只要语义特征相似，AI 就能举一反三，准确预判其生命周期。
    当然，如果一个二进制文件被足够多次的重复使用,我们的学习可以做到全覆盖,这个时候,这种方法就退化到了PGO,

Overview of Lifetime(ML-based) Prediction allocator 
    如何有效解决: 开销与覆盖问题: 我们通过对多次执行进行抽样来解决开销和覆盖率的挑战,我们在一段抽样期间连接到指定的应用程序，并收集这段时间内极小一部分内存分配的生命周期,这个是当然为了避免全程收集的开销太大了(396ns), 所以采取抽样.
    抽样收集到的零散调用栈（转成 Token）喂给 ML 模型。即使有些新版本的新上下文没被抽样到，AI 也能靠泛化能力“猜”出它们的生命周期。

    detailed :
    1. Sampling-based Data Collection: 抽样是: 定期连接到服务器
    **分配钩子(Allocation Hook)**
    当触发抽样时，分配钩子会记录对象的完整调用栈、分配时间戳、对象大小、对齐方式、栈指针以及 CPU ID，并把它们存入一个以“对象内存地址指针”为索引的哈希表中。
    **释放钩子(Deallocation Hook)**
    当对象被释放（Free）时，释放钩子会去查这个哈希表。如果找到了，就记录下释放时的栈轨迹、时间戳、线程/CPU

    配对与统计(Deduplication & Lifetime Distribution):
    “分配”和“释放”成功配对后，数据会被存入另一个用于去重的哈希表。为了节省空间并方便后续分析，系统不会存每一条明细，而是直接计算并维护生命周期的统计分布数据（最大值、最小值、计数、总和、平方和，从而能随时算出均值和方差

    持久化：
    抽样周期结束时，这些统计结果会被序列化存储为协议缓冲区（Protocol Buffer, protobuf），供后续机器学习模型训练使用。

    2. Lifetime Prediction Model(the real decision maker)

    Our goal is to predict object lifetimes based on our collection of past lifetime samples to generalize to previously unseen stack traces.

    "we weight each stack trace according to the number of times it was observed and sample multiple copies for frequently occurring traces"
    频率加权(Weighting): 高频发生的调用栈赋予更高的权重（通过多次采样复制），让模型重点关注那些高频、重要的分配场景，而不是被少量冷门调用栈干扰。

    **Machine Learning Model**: 

    分词与嵌入（Tokenization & Embedding）：

    把调用栈里的每一帧（Stack Frame，如类名、方法名）当作一句话，用特殊字符（如 ,、::、@）切成词（Tokens）。

    建立一个词表（比如 5000 个最常见的 Token），不认识的归为 UNK。

    采用类似自然语言处理中 word2vec 的 Embedding（嵌入矩阵），把相似的语义词汇（例如 ParseFromArray 和 InternalParse）映射到相近的向量空间中。这正是模型具备“泛化能力”、能识别没见过的新上下文的关键所在。

    这里使用的是LSTM 神经网络, LSTM 就是 long short-term neutral network, 优势在于:
    LSTM常用于自然语言处理(NLP)的"预测下一个词", 在这个场景中,Stack Trace(调用栈),本来就是一种连续数据 , func1 -> func 2 -> func 3 -> ... , 
    **捕捉上下文（Context）：**LSTM 通过对序列中的每一个元素进行递归计算，并在每一步传递和更新状态，最终基于最后一步输出预测结果

    从上到下, 自然的捕捉到了程序的嵌套结构. 这样我们就可以知道生命周期的信息了.简单说就是: 我们需要完整的信息才能预测生命周期
    (原文使用了 一个 Protocol Buffer 的例子来说明这一点)

    模型如何学习: 要遍历这些框架. step by step to travel through the stack frame -- to decide whether a particular token is important or not.
    这个真是LSTM 独有的优点:

    LSTM 内部有精密的“门控机制（Gates）”。在沿着调用栈流动时
    它会把重要信息（如：这是一个全局初始化调用的上下文）一直传递带走（Carrying through information）。
    会把无用噪声（如：一些无关紧要的通用包装函数）直接忽略或忘记,最终根据整体上下文，决定哪些符号才是判断生命周期的关键“开关

    什么是 Embedding（嵌入）？
    计算机看不懂 ParseFromArray 这种文本字符串。因此，模型会维护一个嵌入矩阵 $A$，把每个字符串符号转换成一个高维数向量（Embedding Vector）。这个矩阵 $A$ 会随着模型训练一起学习
    
    We chose the LSTM architecture since it is one of the simplest sequence models. 也就是说: 只是因为简单实现和轻量,放入硬件的处理速度更快.

    具体参数配置: 
    组件 / 参数	配置值	作用与含义
    网络层数	单层 LSTM (Single-layer)	保持结构简单，降低计算耗时
    隐藏状态大小 (Hidden State)	64（尝试过 16）	模型内部记忆容量大小
    嵌入维度 (Embedding Size)	32	把符号 Token 压缩成 32 维的向量
    输出层与损失函数	Softmax + 交叉熵 (Cross-Entropy)	转化为 8 个生命周期类别的概率分布
    优化器与学习率	Adam，学习率 0.001	标准高效的梯度下降优化算法
    梯度裁剪 (Gradient Clipping)	5.0	限制梯度的最大幅度，防止训练时梯度爆炸

    对于内存分配器来说，直接调用完整的 TensorFlow 运行时栈来获取生命周期预测，其开销是高到无法接受的。因此在训练完成后，使用 TensorFlow 的 XLA 编译器将训练好的模型直接转换（编译）为 C++ 代码，并将其编译静态链接到分配器中。该模型直接运行在触发内存分配的线程内部

    多线程处理:
    如果几百个线程同时申请内存，就会带来两个严重问题：

    状态冲突（Data Race / Thread Safety）：
    LSTM 神经网络在推理计算时，需要用到一些中间临时变量和矩阵状态（即句中的 “internal buffers”，如隐状态 hidden state、特征暂存区等）。如果所有线程共享同一套缓冲区，大家同时读写，数据就会彻底乱套（数据竞争）。

    锁竞争开销（Lock Contention）：
    如果为了线程安全给模型加一把全局大锁（Mutex），同一时间只允许一个线程去问 AI，那么 64 核甚至 128 核的服务器性能就会瞬间崩塌——所有线程都在排队等这把锁。

    解决方法:
    实例化多个缓冲区(Instantiate internal buffers multiple times): 模型的核心权重参数（Weights/Embedding Matrix）是只读的，可以被所有线程共享。
    但模型推理时用到的临时计算缓冲区（Internal Buffers），作者复制（实例化）出了多份。
    效果：不同线程在调用模型计算时，可以使用各自独立的缓冲区（或者使用线程本地存储 Thread Local Storage, TLS），相互之间互不干扰，完全不需要互相等待。
    是典型的空间换时间.

    当然,作者也考虑到了: 限制问题: 如果线程数量太多了, 资源不够使用,那么就加入并发控制(concurrency control).


Lifetime Aware Allocator Design
    这个就是比较创新的地方，和其他的分配器不一样的是,其他的 C/C++ 分配器都是采用了 以 size 为核心的管理方式,而这里: we directly manage huge pages and segment object allocation into predicted lifetime classes." 就是以生命周期为核心的管理方式了.

    优先在于: 我们按照生命周期管理,那么这个内存就可以在一定的时间内被及时的回收,降低了碎片率.
    在一个专区里的对象几乎会在同一时间一起死亡！一旦整个内存页全空了，就能立刻完整回收给操作系统，从根本上消灭碎片。

    那么为什么还要细分为 Huge page -> block -> line 的管理方式呢?? 我们当然不可以在几个line 过期的时候把这几个line 返回给系统,之所以要这个结构: 是因为: 我们需要管理和监视: 这个page 是否已经全部过期了, 以及对于要分配的内存,我们要知道是否还有空闲位置.

    detailed:
    1. 利用 TLB 优化与大页（Huge Pages）：
    为了减少 CPU 的 TLB（页表缓存）未命中开销，Llama 直接向操作系统按 2MB 大页（Huge Page） 申请物理内存，并进一步将大页切分为 8 KB 的块（Block） 来跟踪存活状态。
    > 前置知识:
    虚拟地址（Virtual Address）：只是操作系统给程序画的一张“账本/地图”，只是占据了一个数字范围（比如 0x7FFF00000000 到 0x7FFF40000000）。只占账本，不消耗任何物理内存条（RAM）。
    物理内存（Physical RAM）：当程序真的往这个地址写数据时，操作系统才会通过页表（Page Table）将这块虚拟地址映射到真正的物理内存条上。
    **64 bit 的机器的虚拟地址是近乎无限的.**
    为什么我可以自己决定分配的虚拟地址呢?
    你也许感到奇怪, 平时直接 new 一下就可了,似乎也没有参数用来写虚拟地址啊,难道重载了?
    问题是new 底层调用的还是allocator, 也就是说:  
    ```C
    pool = mmap(NULL, POOL_SIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ```
    像是这里的 `NULL` 其实就是用来传递虚拟地址的.(NULL 表示系统"随意")
    "as fragmentation of virtual memory is not a concern" 也不用担心虚拟内存的分配问题.
    2. 利用 64 位大虚拟地址空间划分 16 GB 区域（LC Regions）,直接在虚拟地址实现不同life-time class 的分离.

