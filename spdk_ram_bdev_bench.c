/*
 * spdk_ram_bdev_bench.c
 *
 * Full workflow:
 *   1. Init SPDK + hugepages
 *   2. Create 64KB malloc bdev (128 blocks x 512B)
 *   3. Open bdev, get I/O channel
 *   4. DMA path:     write -> read -> verify -> perf loop
 *   5. non-DMA path: write -> read -> verify -> perf loop
 *   6. Print side-by-side comparison
 *   7. Clean shutdown
 *
 * Metrics collected per path:
 *   - Write latency  (ns)
 *   - Read latency   (ns)
 *   - Throughput     (MB/s)
 *   - IOPS
 *   - CPU cycles per I/O  (rdtsc)
 *   - Min / Max / Avg latency over PERF_ITERATIONS runs
 *
 * Build:
 *   cd spdk/examples/bdev/experiment
 *   make
 *
 * Run:
 *   sudo ../../build/examples/spdk_ram_bdev_bench
 */

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"

/* ── Configuration ──────────────────────────────────────────────────────── */

#define BDEV_NAME        "sim_disk"
#define NUM_BLOCKS       128          /* 128 x 512 = 64 KB total */
#define BLOCK_SIZE       512
#define PERF_ITERATIONS  1000         /* I/Os per perf run */
#define WRITE_PATTERN    0xAB         /* byte written to every block */

/* ── malloc bdev internal API ───────────────────────────────────────────── */
/*
 * Exact copy of struct malloc_bdev_opts from bdev_malloc.h.
 * Redeclared here to avoid including the internal header.
 * IMPORTANT: uuid must be struct spdk_uuid (value), NOT a pointer.
 *            Getting this wrong shifts all fields and causes block_size=0.
 */
struct malloc_bdev_opts {
    char                    *name;
    struct spdk_uuid         uuid;               /* value type — NOT pointer */
    uint64_t                 num_blocks;
    uint32_t                 block_size;
    uint32_t                 physical_block_size;
    uint32_t                 optimal_io_boundary;
    uint32_t                 md_size;
    bool                     md_interleave;
    enum spdk_dif_type       dif_type;
    bool                     dif_is_head_of_md;
    enum spdk_dif_pi_format  dif_pi_format;
    int32_t                  numa_id;
};

int create_malloc_disk(struct spdk_bdev **bdev, const struct malloc_bdev_opts *opts);

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* x86 CPU cycle counter — measures CPU work per I/O, not wall time */
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Monotonic wall clock in nanoseconds — measures real elapsed time */
static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ── Per-path stats ──────────────────────────────────────────────────────── */

struct perf_stats {
    const char *label;

    uint64_t write_lat_ns;
    uint64_t read_lat_ns;

    uint64_t loop_write_min_ns;
    uint64_t loop_write_max_ns;
    uint64_t loop_write_total_ns;

    uint64_t loop_read_min_ns;
    uint64_t loop_read_max_ns;
    uint64_t loop_read_total_ns;

    uint64_t write_cycles;
    uint64_t read_cycles;

    bool     verify_ok;
};

/* ── App state ───────────────────────────────────────────────────────────── */

struct app_ctx {
    struct spdk_bdev        *bdev;
    struct spdk_bdev_desc   *desc;
    struct spdk_io_channel  *ch;

    char *dma_write_buf;    /* aligned_alloc — page-aligned, DMA path  */
    char *dma_read_buf;
    char *plain_write_buf;  /* plain malloc  — unaligned, non-DMA path */
    char *plain_read_buf;

    struct perf_stats dma_stats;
    struct perf_stats nodma_stats;

    uint64_t  t_start_ns;
    uint64_t  t_start_tsc;
    int       perf_iter;
    bool      dma_phase;
};

static struct app_ctx g_ctx = {0};

/* ── Forward declarations ────────────────────────────────────────────────── */
static void phase_nodma_start(void);
static void perf_write_submit(void);
static void print_results_and_stop(void);

/* ── Cleanup ─────────────────────────────────────────────────────────────── */

static void cleanup(void)
{
    if (g_ctx.dma_write_buf)   free(g_ctx.dma_write_buf);
    if (g_ctx.dma_read_buf)    free(g_ctx.dma_read_buf);
    if (g_ctx.plain_write_buf) free(g_ctx.plain_write_buf);
    if (g_ctx.plain_read_buf)  free(g_ctx.plain_read_buf);
    if (g_ctx.ch)              spdk_put_io_channel(g_ctx.ch);
    if (g_ctx.desc)            spdk_bdev_close(g_ctx.desc);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PRINT RESULTS
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_results_and_stop(void)
{
    struct perf_stats *d  = &g_ctx.dma_stats;
    struct perf_stats *nd = &g_ctx.nodma_stats;

    uint64_t total_bytes = (uint64_t)BLOCK_SIZE * PERF_ITERATIONS;

    double d_write_avg  = (double)d->loop_write_total_ns  / PERF_ITERATIONS;
    double d_read_avg   = (double)d->loop_read_total_ns   / PERF_ITERATIONS;
    double nd_write_avg = (double)nd->loop_write_total_ns / PERF_ITERATIONS;
    double nd_read_avg  = (double)nd->loop_read_total_ns  / PERF_ITERATIONS;

    double d_write_tput  = (double)total_bytes / ((double)d->loop_write_total_ns  / 1e9) / (1024*1024);
    double d_read_tput   = (double)total_bytes / ((double)d->loop_read_total_ns   / 1e9) / (1024*1024);
    double nd_write_tput = (double)total_bytes / ((double)nd->loop_write_total_ns / 1e9) / (1024*1024);
    double nd_read_tput  = (double)total_bytes / ((double)nd->loop_read_total_ns  / 1e9) / (1024*1024);

    double d_write_iops  = 1e9 / d_write_avg;
    double d_read_iops   = 1e9 / d_read_avg;
    double nd_write_iops = 1e9 / nd_write_avg;
    double nd_read_iops  = 1e9 / nd_read_avg;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║         SPDK RAM bdev benchmark  (%d iterations)             ║\n", PERF_ITERATIONS);
    printf("║         bdev: %s  |  %d blocks x %dB = %dKB                ║\n",
           BDEV_NAME, NUM_BLOCKS, BLOCK_SIZE, (NUM_BLOCKS * BLOCK_SIZE) / 1024);
    printf("╠══════════════════════════╦═══════════════╦════════════════════════╣\n");
    printf("║ Metric                   ║  DMA buffer   ║  non-DMA (malloc) buf  ║\n");
    printf("╠══════════════════════════╬═══════════════╬════════════════════════╣\n");
    printf("║ Verify correctness       ║  %-13s║  %-22s║\n",
           d->verify_ok  ? "PASS" : "FAIL",
           nd->verify_ok ? "PASS" : "FAIL");
    printf("╠══════════════════════════╬═══════════════╬════════════════════════╣\n");
    printf("║ Single write latency     ║  %7lu ns   ║  %7lu ns              ║\n",
           d->write_lat_ns, nd->write_lat_ns);
    printf("║ Single read  latency     ║  %7lu ns   ║  %7lu ns              ║\n",
           d->read_lat_ns,  nd->read_lat_ns);
    printf("╠══════════════════════════╬═══════════════╬════════════════════════╣\n");
    printf("║ Write  avg latency       ║  %7.0f ns   ║  %7.0f ns              ║\n",
           d_write_avg, nd_write_avg);
    printf("║ Write  min latency       ║  %7lu ns   ║  %7lu ns              ║\n",
           d->loop_write_min_ns, nd->loop_write_min_ns);
    printf("║ Write  max latency       ║  %7lu ns   ║  %7lu ns              ║\n",
           d->loop_write_max_ns, nd->loop_write_max_ns);
    printf("╠══════════════════════════╬═══════════════╬════════════════════════╣\n");
    printf("║ Read   avg latency       ║  %7.0f ns   ║  %7.0f ns              ║\n",
           d_read_avg, nd_read_avg);
    printf("║ Read   min latency       ║  %7lu ns   ║  %7lu ns              ║\n",
           d->loop_read_min_ns, nd->loop_read_min_ns);
    printf("║ Read   max latency       ║  %7lu ns   ║  %7lu ns              ║\n",
           d->loop_read_max_ns, nd->loop_read_max_ns);
    printf("╠══════════════════════════╬═══════════════╬════════════════════════╣\n");
    printf("║ Write  throughput        ║  %7.2f MB/s ║  %7.2f MB/s           ║\n",
           d_write_tput, nd_write_tput);
    printf("║ Read   throughput        ║  %7.2f MB/s ║  %7.2f MB/s           ║\n",
           d_read_tput, nd_read_tput);
    printf("╠══════════════════════════╬═══════════════╬════════════════════════╣\n");
    printf("║ Write  IOPS              ║  %7.0f      ║  %7.0f               ║\n",
           d_write_iops, nd_write_iops);
    printf("║ Read   IOPS              ║  %7.0f      ║  %7.0f               ║\n",
           d_read_iops, nd_read_iops);
    printf("╠══════════════════════════╬═══════════════╬════════════════════════╣\n");
    printf("║ Write  CPU cycles        ║  %7lu cyc  ║  %7lu cyc             ║\n",
           d->write_cycles, nd->write_cycles);
    printf("║ Read   CPU cycles        ║  %7lu cyc  ║  %7lu cyc             ║\n",
           d->read_cycles,  nd->read_cycles);
    printf("╚══════════════════════════╩═══════════════╩════════════════════════╝\n");
    printf("\n  Note: on WSL both buffers use plain malloc — no real DMA hardware.\n"
           "  On real NVMe, swap aligned_alloc to spdk_dma_zmalloc for DMA path.\n\n");

    cleanup();
    spdk_app_stop(0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PERF LOOP
 * ═══════════════════════════════════════════════════════════════════════════ */

static void perf_read_cb(struct spdk_bdev_io *io, bool ok, void *arg)
{
    struct perf_stats *s = (struct perf_stats *)arg;
    uint64_t lat         = now_ns() - g_ctx.t_start_ns;

    spdk_bdev_free_io(io);

    if (lat < s->loop_read_min_ns) s->loop_read_min_ns = lat;
    if (lat > s->loop_read_max_ns) s->loop_read_max_ns = lat;
    s->loop_read_total_ns += lat;

    g_ctx.perf_iter++;
    if (g_ctx.perf_iter < PERF_ITERATIONS) {
        perf_write_submit();
    } else {
        if (g_ctx.dma_phase) {
            phase_nodma_start();
        } else {
            print_results_and_stop();
        }
    }
}

static void perf_write_cb(struct spdk_bdev_io *io, bool ok, void *arg)
{
    struct perf_stats *s = (struct perf_stats *)arg;
    uint64_t lat         = now_ns() - g_ctx.t_start_ns;

    spdk_bdev_free_io(io);

    if (lat < s->loop_write_min_ns) s->loop_write_min_ns = lat;
    if (lat > s->loop_write_max_ns) s->loop_write_max_ns = lat;
    s->loop_write_total_ns += lat;

    /* chain into read */
    char *rbuf = g_ctx.dma_phase ? g_ctx.dma_read_buf : g_ctx.plain_read_buf;
    g_ctx.t_start_ns = now_ns();
    spdk_bdev_read(g_ctx.desc, g_ctx.ch,
                   rbuf, 0, BLOCK_SIZE,
                   perf_read_cb, arg);
}

static void perf_write_submit(void)
{
    struct perf_stats *s = g_ctx.dma_phase ? &g_ctx.dma_stats : &g_ctx.nodma_stats;
    char *wbuf           = g_ctx.dma_phase ? g_ctx.dma_write_buf : g_ctx.plain_write_buf;

    g_ctx.t_start_ns = now_ns();
    spdk_bdev_write(g_ctx.desc, g_ctx.ch,
                    wbuf, 0, BLOCK_SIZE,
                    perf_write_cb, s);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SINGLE WRITE → READ → VERIFY  (runs once before perf loop)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void verify_read_cb(struct spdk_bdev_io *io, bool ok, void *arg)
{
    struct perf_stats *s = (struct perf_stats *)arg;
    uint64_t lat         = now_ns() - g_ctx.t_start_ns;

    spdk_bdev_free_io(io);
    s->read_lat_ns = lat;
    s->read_cycles = rdtsc() - g_ctx.t_start_tsc;

    /* verify every byte == WRITE_PATTERN */
    char *rbuf = g_ctx.dma_phase ? g_ctx.dma_read_buf : g_ctx.plain_read_buf;
    s->verify_ok = true;
    for (int i = 0; i < BLOCK_SIZE; i++) {
        if ((unsigned char)rbuf[i] != WRITE_PATTERN) {
            s->verify_ok = false;
            break;
        }
    }
    SPDK_NOTICELOG("[%s] verify: %s  read_lat: %lu ns\n",
                   s->label, s->verify_ok ? "PASS" : "FAIL", lat);

    /* kick off perf loop */
    g_ctx.perf_iter      = 0;
    s->loop_write_min_ns = UINT64_MAX;
    s->loop_read_min_ns  = UINT64_MAX;
    perf_write_submit();
}

static void verify_write_cb(struct spdk_bdev_io *io, bool ok, void *arg)
{
    struct perf_stats *s = (struct perf_stats *)arg;
    uint64_t lat         = now_ns() - g_ctx.t_start_ns;

    spdk_bdev_free_io(io);
    s->write_lat_ns = lat;
    s->write_cycles = rdtsc() - g_ctx.t_start_tsc;

    SPDK_NOTICELOG("[%s] single write done  lat: %lu ns  cycles: %lu\n",
                   s->label, lat, s->write_cycles);

    /* read back for verify */
    char *rbuf = g_ctx.dma_phase ? g_ctx.dma_read_buf : g_ctx.plain_read_buf;
    g_ctx.t_start_ns  = now_ns();
    g_ctx.t_start_tsc = rdtsc();
    spdk_bdev_read(g_ctx.desc, g_ctx.ch,
                   rbuf, 0, BLOCK_SIZE,
                   verify_read_cb, s);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PHASE START FUNCTIONS
 * ═══════════════════════════════════════════════════════════════════════════ */

static void phase_nodma_start(void)
{
    SPDK_NOTICELOG("── non-DMA path (plain malloc buffer) ──\n");
    g_ctx.dma_phase = false;

    memset(g_ctx.plain_write_buf, WRITE_PATTERN, BLOCK_SIZE);
    memset(g_ctx.plain_read_buf,  0x00,          BLOCK_SIZE);

    g_ctx.t_start_ns  = now_ns();
    g_ctx.t_start_tsc = rdtsc();

    spdk_bdev_write(g_ctx.desc, g_ctx.ch,
                    g_ctx.plain_write_buf, 0, BLOCK_SIZE,
                    verify_write_cb, &g_ctx.nodma_stats);
}

static void phase_dma_start(void)
{
    SPDK_NOTICELOG("── DMA path (page-aligned buffer) ──\n");
    g_ctx.dma_phase = true;

    memset(g_ctx.dma_write_buf, WRITE_PATTERN, BLOCK_SIZE);
    memset(g_ctx.dma_read_buf,  0x00,          BLOCK_SIZE);

    g_ctx.t_start_ns  = now_ns();
    g_ctx.t_start_tsc = rdtsc();

    spdk_bdev_write(g_ctx.desc, g_ctx.ch,
                    g_ctx.dma_write_buf, 0, BLOCK_SIZE,
                    verify_write_cb, &g_ctx.dma_stats);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * BDEV EVENT CALLBACK — required by spdk_bdev_open_ext in SPDK v26
 * ═══════════════════════════════════════════════════════════════════════════ */

static void bdev_event_cb(enum spdk_bdev_event_type type,
                          struct spdk_bdev *bdev, void *ctx)
{
    SPDK_NOTICELOG("bdev event: type=%d\n", type);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SPDK APP ENTRY — called on the SPDK reactor thread
 * ═══════════════════════════════════════════════════════════════════════════ */

static void app_start(void *arg)
{
    int rc;

    /* ── 1. Create malloc bdev (64 KB) ── */
    struct malloc_bdev_opts malloc_opts = {};
    malloc_opts.name                = BDEV_NAME;
    malloc_opts.num_blocks          = NUM_BLOCKS;
    malloc_opts.block_size          = BLOCK_SIZE;
    malloc_opts.physical_block_size = BLOCK_SIZE;   /* must match block_size */
    malloc_opts.numa_id             = -1;            /* SPDK_ENV_NUMA_ID_ANY */

    rc = create_malloc_disk(&g_ctx.bdev, &malloc_opts);
    if (rc != 0 || g_ctx.bdev == NULL) {
        SPDK_ERRLOG("create_malloc_disk failed: %d\n", rc);
        spdk_app_stop(rc);
        return;
    }
    SPDK_NOTICELOG("bdev created: %s  blocks=%lu  block_size=%u  total=%luKB\n",
                   spdk_bdev_get_name(g_ctx.bdev),
                   spdk_bdev_get_num_blocks(g_ctx.bdev),
                   spdk_bdev_get_block_size(g_ctx.bdev),
                   spdk_bdev_get_num_blocks(g_ctx.bdev) *
                   spdk_bdev_get_block_size(g_ctx.bdev) / 1024);

    /* ── 2. Open bdev + get I/O channel ── */
    rc = spdk_bdev_open_ext(BDEV_NAME, true, bdev_event_cb, NULL, &g_ctx.desc);
    if (rc) {
        SPDK_ERRLOG("spdk_bdev_open_ext failed: %d\n", rc);
        spdk_app_stop(rc);
        return;
    }

    g_ctx.ch = spdk_bdev_get_io_channel(g_ctx.desc);
    if (!g_ctx.ch) {
        SPDK_ERRLOG("spdk_bdev_get_io_channel failed\n");
        spdk_app_stop(-1);
        return;
    }

    /* ── 3. Allocate buffers ── */
    /*
     * DMA path  — page-aligned malloc (4096 byte alignment)
     * On real NVMe hardware replace with spdk_dma_zmalloc(BLOCK_SIZE, 0x1000, NULL)
     *
     * non-DMA path — plain unaligned malloc
     * On real NVMe this would fail or corrupt — works here because malloc bdev
     * is pure RAM with no actual DMA hardware involved.
     */
    g_ctx.dma_write_buf   = aligned_alloc(4096, BLOCK_SIZE);
    g_ctx.dma_read_buf    = aligned_alloc(4096, BLOCK_SIZE);
    g_ctx.plain_write_buf = malloc(BLOCK_SIZE);
    g_ctx.plain_read_buf  = malloc(BLOCK_SIZE);

    if (!g_ctx.dma_write_buf || !g_ctx.dma_read_buf ||
        !g_ctx.plain_write_buf || !g_ctx.plain_read_buf) {
        SPDK_ERRLOG("buffer allocation failed\n");
        spdk_app_stop(-1);
        return;
    }

    /* ── 4. Label stats structs ── */
    g_ctx.dma_stats.label   = "aligned  ";
    g_ctx.nodma_stats.label = "unaligned";

    /* ── 5. Kick off Phase A (aligned/DMA path) ── */
    SPDK_NOTICELOG("starting benchmark  (%d iterations per path)\n",
                   PERF_ITERATIONS);
    phase_dma_start();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    /*
     * Reduce iobuf pool sizes before SPDK init.
     * Default: small=8192×8KB=64MB, large=1024×132KB=132MB — too large for WSL.
     * Minimum allowed: small=64, large=8.
     * We use 256/32 to give the accel channel enough room (needs 128 minimum).
     */
    struct spdk_iobuf_opts iobuf_opts;
    spdk_iobuf_get_opts(&iobuf_opts, sizeof(iobuf_opts));
    iobuf_opts.small_pool_count = 256;
    iobuf_opts.large_pool_count = 32;
    spdk_iobuf_set_opts(&iobuf_opts);

    /*
     * Reduce bdev_io pool size before SPDK init.
     * Default: 65535 entries — far too large for 64MB hugepage budget.
     * Minimum: bdev_io_cache_size × (threads + 1) = 128 × 2 = 256.
     * We use 512 for safety margin.
     */
    struct spdk_bdev_opts bdev_opts;
    spdk_bdev_get_opts(&bdev_opts, sizeof(bdev_opts));
    bdev_opts.bdev_io_pool_size  = 512;
    bdev_opts.bdev_io_cache_size = 128;
    spdk_bdev_set_opts(&bdev_opts);

    struct spdk_app_opts opts = {};
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name            = "spdk_ram_bdev_bench";
    opts.reactor_mask    = "0x1";
    opts.mem_size        = 64;           /* 64MB — matches hugepage allocation */
    opts.iova_mode       = "va";         /* required for WSL2 — no IOMMU */
    opts.unlink_hugepage = false;        /* keep hugepages mapped after init */

    int rc = spdk_app_start(&opts, app_start, NULL);
    spdk_app_fini();
    return rc;
}