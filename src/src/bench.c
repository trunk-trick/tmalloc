/*
 * bench.c — Allocator benchmark.
 *
 * Reads operation list from stdin, executes with timing, prints JSON stats.
 * Designed to work with any malloc via LD_PRELOAD.
 *
 * Input format (one per line):
 *   A <size>       alloc <size> bytes
 *   F <tag>        free   ptr[<tag>]
 *
 * Output: single-line JSON to stdout.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>
#include <dlfcn.h>

#define MAX_OPS    200000
#define MAX_PTRS   100000

typedef struct { char op; size_t size; int tag; } op_t;

static op_t      ops[MAX_OPS];
static int       op_cnt;
static void     *ptrs[MAX_PTRS];
static char      live[MAX_PTRS];
static size_t    total_ever_allocated;

static uint64_t  a_ns[MAX_OPS];
static int       a_cnt;
static uint64_t  f_ns[MAX_OPS];
static int       f_cnt;

/* ---- nanosecond clock ---- */
static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000UL + ts.tv_nsec;
}

/* ---- read /proc/self/status ---- */
static size_t read_status(const char *key) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char buf[256];
    size_t val = 0;
    while (fgets(buf, sizeof(buf), f)) {
        if (strncmp(buf, key, strlen(key)) == 0) {
            sscanf(buf + strlen(key), ": %zu", &val);
            break;
        }
    }
    fclose(f);
    return val;
}

/* ---- qsort comparator for uint64_t ---- */
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* ---- stats helpers ---- */
static double avg(const uint64_t *arr, int n) {
    if (n == 0) return 0;
    uint64_t sum = 0;
    for (int i = 0; i < n; i++) sum += arr[i];
    return (double)sum / n;
}

static double pct(const uint64_t *arr, int n, double p) {
    if (n == 0) return 0;
    int idx = (int)(p / 100.0 * (n - 1));
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return (double)arr[idx];
}

/* ---- binary search for max contiguous allocation ---- */
static size_t probe_max_block(size_t hi_limit) {
    /* binary search for the largest single malloc that succeeds, up to hi_limit.
     * Use a moderate limit (e.g. 128KB) to measure BRK/small-block heap
     * fragmentation rather than mmap fallback. */
    size_t lo = 16, hi = hi_limit, best = 0;
    while (lo <= hi) {
        size_t mid = lo + (hi - lo) / 2;
        void *p = malloc(mid);
        if (p) { best = mid; free(p); lo = mid + 1; }
        else    { hi = mid - 1; }
    }
    return best;
}

/* ================================================================ */

int main(int argc, char **argv) {
    /* 1. read workload */
    char line[64];
    total_ever_allocated = 0;
    while (fgets(line, sizeof(line), stdin)) {
        if (line[0] == 'A') {
            ops[op_cnt].op   = 'A';
            ops[op_cnt].size = strtoul(line + 2, NULL, 10);
            total_ever_allocated += ops[op_cnt].size;
        } else if (line[0] == 'F') {
            ops[op_cnt].op  = 'F';
            ops[op_cnt].tag = (int)strtol(line + 2, NULL, 10);
        }
        op_cnt++;
        if (op_cnt >= MAX_OPS) break;
    }

    /* 2. warmup pass */
    int pc = 0;
    memset(live, 0, sizeof(live));
    for (int i = 0; i < op_cnt; i++) {
        if (ops[i].op == 'A') {
            ptrs[pc] = malloc(ops[i].size);
            live[pc] = 1;
            pc++;
        } else {
            int t = ops[i].tag;
            if (t < MAX_PTRS && live[t]) { free(ptrs[t]); live[t] = 0; }
        }
    }
    for (int j = 0; j < pc; j++) if (live[j]) { free(ptrs[j]); live[j] = 0; }

    /* 3. timed pass */
    a_cnt = f_cnt = 0;
    pc    = 0;
    memset(live, 0, sizeof(live));

    uint64_t t_start = now_ns();
    for (int i = 0; i < op_cnt; i++) {
        if (ops[i].op == 'A') {
            uint64_t t0 = now_ns();
            ptrs[pc] = malloc(ops[i].size);
            uint64_t t1 = now_ns();
            a_ns[a_cnt++] = t1 - t0;
            live[pc] = 1;
            pc++;
        } else {
            int t = ops[i].tag;
            if (t < MAX_PTRS && live[t]) {
                uint64_t t0 = now_ns();
                free(ptrs[t]);
                uint64_t t1 = now_ns();
                f_ns[f_cnt++] = t1 - t0;
                live[t] = 0;
            }
        }
    }
    uint64_t t_end = now_ns();
    double total_us = (t_end - t_start) / 1000.0;

    /* 4. peak RSS (HWM) */
    size_t peak_rss = read_status("VmHWM");
    size_t vm_peak  = read_status("VmPeak");

    /* 5. fragmentation under load — BEFORE freeing remaining allocs.
     *    For hand-written allocators, use the internal __alloc_frag_stats
     *    which walks the free list directly (accurate).
     *    For real allocators, probe the small-block heap with binary search
     *    capped at 64KB (below glibc's 128KB mmap threshold). */
    size_t total_free = 0;
    size_t max_contig = 0;

    void (*frag_fn)(size_t *, size_t *) = dlsym(RTLD_DEFAULT, "__alloc_frag_stats");
    if (frag_fn) {
        frag_fn(&total_free, &max_contig);
    } else {
        max_contig = probe_max_block(64UL * 1024);
        total_free = 0;  /* unknown for real allocators */
    }

    /* 6. free remaining live allocations */
    for (int j = 0; j < pc; j++) if (live[j]) { free(ptrs[j]); live[j] = 0; }

    /* 7. sort timing arrays for percentiles */
    qsort(a_ns, a_cnt, sizeof(uint64_t), cmp_u64);
    qsort(f_ns, f_cnt, sizeof(uint64_t), cmp_u64);

    /* 8. compute fragmentation ratio from internal free-list walk.
     *    Only available for hand-written allocators that export __alloc_frag_stats. */
    int have_frag = (total_free > 0 && max_contig > 0);
    double frag_ratio = 0.0;
    if (have_frag) {
        double d = 1.0 - (double)max_contig / (double)total_free;
        if (d < 0.0) d = 0.0;
        if (d > 1.0) d = 1.0;
        frag_ratio = d;
    }

    /* 9. output JSON */
    printf("{");
    printf("\"total_ops\":%d,", op_cnt);
    printf("\"total_time_us\":%.2f,", total_us);
    printf("\"avg_alloc_ns\":%.1f,", avg(a_ns, a_cnt));
    printf("\"p50_alloc_ns\":%.1f,", pct(a_ns, a_cnt, 50));
    printf("\"p99_alloc_ns\":%.1f,", pct(a_ns, a_cnt, 99));
    printf("\"avg_free_ns\":%.1f,", avg(f_ns, f_cnt));
    printf("\"p50_free_ns\":%.1f,", pct(f_ns, f_cnt, 50));
    printf("\"p99_free_ns\":%.1f,", pct(f_ns, f_cnt, 99));
    printf("\"peak_rss_kb\":%zu,", peak_rss);
    printf("\"vm_peak_kb\":%zu,", vm_peak);
    printf("\"max_contig_bytes\":%zu,", max_contig);
    printf("\"total_free_bytes\":%zu,", total_free);
    if (have_frag)
        printf("\"frag_ratio\":%.4f,", frag_ratio);
    else
        printf("\"frag_ratio\":null,");
    printf("\"ops_per_sec\":%.0f", op_cnt / (total_us / 1000000.0));
    printf("}\n");

    return 0;
}
