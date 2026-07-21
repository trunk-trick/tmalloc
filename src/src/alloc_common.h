#pragma once
/*
 * Common code shared by first-fit / best-fit / worst-fit allocators.
 *
 * Each strategy file:
 *   1. #include "alloc_common.h"
 *   2. defines  static block_t *find_free_block(size_t needed) { ... }
 *
 * The header provides malloc / free / calloc / realloc.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <unistd.h>

#define POOL_SIZE  (64UL * 1024 * 1024)
#define ALIGN      16
#define FOOT_SZ    sizeof(size_t)

typedef struct block {
    size_t           size;   /* total block size, LSB = free flag */
    struct block    *next;
    struct block    *prev;
} block_t;

#define HDR_SZ sizeof(block_t)
#define MIN_BLK  (HDR_SZ + FOOT_SZ + 16)

static char    *pool;
static block_t *free_head;
static size_t   pool_bytes_used;
static size_t   pool_alloc_count;

/* ---- forward: strategy file must define this ---- */
static block_t *find_free_block(size_t needed);

/* ---- helpers ---- */
static inline size_t  blk_sz(block_t *b)   { return b->size & ~15UL; }
static inline int     blk_free(block_t *b) { return b->size & 1UL; }
static inline void    blk_set(block_t *b, size_t sz, int free) {
    b->size = sz | (free ? 1UL : 0UL);
}
static inline size_t *blk_ft(block_t *b) {
    return (size_t *)((char *)b + blk_sz(b) - FOOT_SZ);
}
static inline block_t *blk_hdr(void *p) { return (block_t *)((char *)p - HDR_SZ); }

static size_t align_up(size_t n) {
    return (n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
}

static block_t *blk_prev(block_t *b) {
    if ((char *)b == pool) return NULL;
    size_t *ft = (size_t *)((char *)b - FOOT_SZ);
    return (block_t *)((char *)b - *ft);
}

static block_t *blk_next(block_t *b) {
    block_t *n = (block_t *)((char *)b + blk_sz(b));
    return ((char *)n < pool + POOL_SIZE) ? n : NULL;
}

/* ---- free-list ops ---- */
static void fl_del(block_t *b) {
    if (b->prev) b->prev->next = b->next;
    else         free_head = b->next;
    if (b->next) b->next->prev = b->prev;
}


// hou about this ?
static void fl_add(block_t *b) {
    block_t *cur = free_head, *prv = NULL;
    while (cur && cur < b) { prv = cur; cur = cur->next; }
    b->prev = prv;
    b->next = cur;
    if (prv) prv->next = b;
    else     free_head = b;
    if (cur) cur->prev = b; 
}

/* ---- coalesce + add to free list ---- */
static void coalesce_and_add(block_t *b) {
    /* b is free but NOT yet in the free list. Merge with neighbours,
     * then insert the final block into the free list. */

    /* merge with next */
    block_t *nx = blk_next(b);
    if (nx && blk_free(nx)) {
        fl_del(nx);
        blk_set(b, blk_sz(b) + blk_sz(nx), 1);
        *blk_ft(b) = blk_sz(b);
    }

    /* merge with prev */
    block_t *pv = blk_prev(b);
    if (pv && blk_free(pv)) {
        fl_del(pv);
        blk_set(pv, blk_sz(pv) + blk_sz(b), 1);
        *blk_ft(pv) = blk_sz(pv);
        b = pv;   /* merged block is now the predecessor */
    }

    fl_add(b);
}

/* ---- split ---- */
static void split(block_t *b, size_t need) {
    size_t rem = blk_sz(b) - need;
    if (rem >= MIN_BLK) {
        block_t *nb = (block_t *)((char *)b + need);
        blk_set(nb, rem, 1);
        *blk_ft(nb) = rem;
        nb->next = b->next;
        nb->prev = b->prev;
        if (b->prev) b->prev->next = nb;
        else         free_head = nb;
        if (b->next) b->next->prev = nb;
        blk_set(b, need, 0);
    } else {
        blk_set(b, blk_sz(b), 0);
        fl_del(b);
    }
    *blk_ft(b) = blk_sz(b);
}

/* ---- init ---- */
__attribute__((constructor))
static void init_pool(void) {
    pool = mmap(NULL, POOL_SIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    block_t *b = (block_t *)pool;
    blk_set(b, POOL_SIZE, 1);
    *blk_ft(b) = POOL_SIZE;
    b->next = b->prev = NULL;
    free_head = b;
}

/* ---- mmap fallback for huge allocs ---- */
static void *fallback_alloc(size_t size) {
    size_t total = HDR_SZ + align_up(size) + FOOT_SZ;
    total = align_up(total);
    char *p = mmap(NULL, total, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    block_t *b = (block_t *)p;
    blk_set(b, total, 0);
    *blk_ft(b) = total;
    return (char *)b + HDR_SZ;
}

static int in_pool(void *p) {
    return (char *)p >= pool && (char *)p < pool + POOL_SIZE;
}

/* ================================================================
 *  Standard API
 * ================================================================ */

void *malloc(size_t size) {
    if (size == 0) return NULL;
    if (!pool) init_pool();

    size_t need = HDR_SZ + align_up(size) + FOOT_SZ;
    need = align_up(need);

    if (need > POOL_SIZE / 4) return fallback_alloc(size);

    block_t *b = find_free_block(need);
    if (!b) return fallback_alloc(size);

    split(b, need);
    pool_bytes_used  += size;
    pool_alloc_count += 1;
    return (char *)b + HDR_SZ;
}

void free(void *ptr) {
    if (!ptr) return;
    if (!pool) init_pool();

    if (!in_pool(ptr)) {
        block_t *b = blk_hdr(ptr);
        munmap(b, blk_sz(b));
        return;
    }

    block_t *b = blk_hdr(ptr);
    blk_set(b, blk_sz(b), 1);
    coalesce_and_add(b);
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    block_t *b = blk_hdr(ptr);
    size_t old = blk_sz(b) - HDR_SZ - FOOT_SZ;
    if (size <= old) return ptr;
    void *np = malloc(size);
    // 拷贝数据到新块,释放原指针
    if (np) { memcpy(np, ptr, old); free(ptr); }
    return np;
}

/* ---- stats for bench ---- */
size_t __alloc_pool_used(void)   { return pool_bytes_used; }
size_t __alloc_alloc_count(void) { return pool_alloc_count; }

// frag_ratio = 1 - max_contig / total_free 也就是计算最大的“连续空闲块”的占比，用一减去就可以了
// 

void __alloc_frag_stats(size_t *total_free, size_t *max_contig) {
    *total_free = 0;
    *max_contig = 0;
    for (block_t *b = free_head; b; b = b->next) {
        size_t ds = blk_sz(b) - HDR_SZ - FOOT_SZ;
        *total_free += ds;
        if (ds > *max_contig) *max_contig = ds;
    }
}
