/* First-Fit: pick the first block that fits, scanning from low address. */
#include "alloc_common.h"

static block_t *find_free_block(size_t needed) {
    for (block_t *b = free_head; b; b = b->next)
        if (blk_sz(b) >= needed) return b;
    return NULL;
}
