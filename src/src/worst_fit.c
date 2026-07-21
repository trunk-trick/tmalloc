/* Worst-Fit: pick the largest block, hoping the remainder stays useful. */
#include "alloc_common.h"

static block_t *find_free_block(size_t needed) {
    block_t *worst = NULL;
    for (block_t *b = free_head; b; b = b->next) {
        size_t sz = blk_sz(b);
        if (sz >= needed && (!worst || sz > blk_sz(worst)))
            worst = b;
    }
    return worst;
}
