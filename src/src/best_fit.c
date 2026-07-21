/* Best-Fit: pick the smallest block that fits, scanning the whole free list. */
#include "alloc_common.h"

static block_t *find_free_block(size_t needed) {
    block_t *best = NULL;
    for (block_t *b = free_head; b; b = b->next) {
        size_t sz = blk_sz(b);
        if (sz >= needed && (!best || sz < blk_sz(best)))
            best = b;
    }
    return best;
}
