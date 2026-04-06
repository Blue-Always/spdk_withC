# spdk_withC
SPDK experimentation application which bypasses JSON-RPC daemon.

C implementation of an SPDK storage experiment, replacing the conventional Python RPC workflow with direct SPDK C API calls. Measures I/O performance of a RAM-backed block device comparing page-aligned (DMA-path) vs unaligned (non-DMA-path) memory buffers.

---

## What is this:

SPDK (Storage Performance Development Kit) is typically used via Python RPC scripts that talk to a running `spdk_tgt` daemon over a JSON socket. This project bypasses that entirely; your C program **is** the SPDK application. No daemon, no socket, no Python.

```
Python way:   spdk_tgt (daemon) ←→ rpc.py (JSON socket) ←→ your script
C way:        your process = SPDK runtime + bdev + reactor + I/O all in one
```

---

## Workflow

The program runs this sequence in a single process:

```
spdk_app_start()
    └── create_malloc_disk()        # 128 blocks × 512B = 64KB RAM bdev
         └── spdk_bdev_open_ext()   # open bdev handle
              └── spdk_bdev_get_io_channel()   # get I/O submission queue
                   │
                   ├── Phase A: aligned buffer (DMA path)
                   │     single write → read → verify correctness
                   │     → 1000× write+read loop → collect stats
                   │
                   └── Phase B: unaligned buffer (non-DMA path)
                         same workflow
                         → print side-by-side comparison table
                         → spdk_app_stop()
```

### Key SPDK functions used

| Function | Purpose |
|---|---|
| `spdk_app_opts_init` + `spdk_app_start` | Init hugepages, DPDK EAL, reactor threads |
| `create_malloc_disk` | Create RAM-backed block device |
| `spdk_bdev_open_ext` | Open bdev handle (like a file descriptor) |
| `spdk_bdev_get_io_channel` | Get per-thread I/O submission queue |
| `aligned_alloc(4096)` | Page-aligned buffer (DMA path simulation) |
| `malloc` | Unaligned buffer (non-DMA path) |
| `spdk_bdev_write` / `spdk_bdev_read` | Async I/O, callback driven |
| `spdk_bdev_free_io` | Must be called in every callback |
| `spdk_app_stop` + `spdk_app_fini` | Clean shutdown + hugepage release |

### Async I/O model

SPDK has no blocking calls. Every I/O returns immediately and the reactor thread polls for completions. The program is a chain of callbacks:

```
phase_dma_start()
  └─ spdk_bdev_write() ──→ verify_write_cb()
                             └─ spdk_bdev_read() ──→ verify_read_cb()
                                                       └─ perf_write_submit() [×1000]
                                                            └─ perf_write_cb() → perf_read_cb()
                                                                 └─ phase_nodma_start()
                                                                      └─ [same chain]
                                                                      └─ print_results_and_stop()
```

---

## Metrics measured

| Metric | How |
|---|---|
| Write / Read latency (ns) | `clock_gettime(CLOCK_MONOTONIC)` delta |
| Min / Max / Avg latency | Tracked over 1000 iterations |
| Throughput (MB/s) | total_bytes / total_time |
| IOPS | 1e9 / avg_latency_ns |
| CPU cycles per I/O | `rdtsc` instruction delta |

Two time functions are used intentionally:
- `now_ns()` : wall clock time, used for latency and throughput (what the user experiences)
- `rdtsc()` : raw CPU cycle counter, used for CPU efficiency (how hard the CPU worked)

---

## Benchmark results (WSL2, SPDK v26.05-pre)

**System:** ASUS laptop, 8GB RAM, Intel Iris Xe, WSL2 Ubuntu 24.04
**Config:** 128 blocks × 512B = 64KB bdev, 1000 iterations, single reactor core

### First I/O (cold)

| Metric | Aligned (DMA path) | Unaligned (non-DMA) |
|---|---|---|
| Write latency | 12,450 ns | 950 ns |
| Read latency | 5,688 ns | 752 ns |
| Write CPU cycles | 28,600 | 2,100 |
| Read CPU cycles | 12,549 | 1,660 |
| Verify | PASS | PASS |

The aligned path is ~13× slower on first I/O due to a **page fault**-`aligned_alloc`, which reserves virtual address space but does not touch physical pages until first access. The unaligned `malloc` buffer was already faulted in by the OS.

### Steady state (1000 iterations)

| Metric | Aligned (DMA path) | Unaligned (non-DMA) |
|---|---|---|
| Write avg latency | 196 ns | 201 ns |
| Write min latency | 153 ns | 157 ns |
| Write max latency | 17,750 ns | 12,859 ns |
| Read avg latency | 199 ns | 199 ns |
| Read min latency | 158 ns | 169 ns |
| Read max latency | 15,694 ns | 1,823 ns |
| Write throughput | 2,492 MB/s | 2,424 MB/s |
| Read throughput | 2,451 MB/s | 2,457 MB/s |
| Write IOPS | 5,105,636 | 4,964,528 |
| Read IOPS | 5,020,433 | 5,033,473 |

Once warmed up both paths are essentially identical (~2.5 GB/s, ~5M IOPS). This is expected on WSL2 (see [WSL2 note](#wsl2-specific-notes) below.)

---

## Setup

### Prerequisites

- Linux (bare metal) or WSL2 (Ubuntu 20.04+)
- 8GB+ RAM recommended
- SPDK v26.x cloned and built

### 1. Clone and build SPDK

```bash
mkdir spdk_exp && cd spdk_exp
git clone https://github.com/spdk/spdk --recursive
cd spdk
sudo scripts/pkgdep.sh --all
./configure
make -j$(nproc)
```

### 2. Place the experiment files

```bash
mkdir examples/bdev/experiment
cp spdk_ram_bdev_bench.c examples/bdev/experiment/
cp Makefile examples/bdev/experiment/
```

### 3. Allocate hugepages

```bash
sudo sysctl -w vm.nr_hugepages=64
cat /proc/meminfo | grep HugePages_Total   # should show 64
```

To make this permanent across WSL reboots, add to `/etc/wsl.conf`:
```ini
[boot]
command="sysctl -w vm.nr_hugepages=64"
```

### 4. Build

```bash
cd examples/bdev/experiment
make
```

### 5. Run

```bash
cd /path/to/spdk
sudo build/examples/spdk_ram_bdev_bench
```

### 6. Free hugepages when done

```bash
sudo sysctl -w vm.nr_hugepages=0
```

---

## WSL2-specific notes

### Two SPDK source patches are needed on WSL2 (optional)

SPDK assumes a Linux environment with IOMMU support and hugepage DMA. WSL2 has neither. Two patches are required to run on WSL2:

**Patch 1 — `lib/env_dpdk/init.c`**

SPDK checks for IOMMU support and forces `--iova-mode=pa` when it's absent. WSL2 has no IOMMU so this always fires, causing DMA allocation to fail. The patch disables this check:

```c
// original:
if (!no_huge && !x86_cpu_support_iommu()) {
    args = push_arg(args, &argcount, _sprintf_alloc("--iova-mode=pa"));

// patched:
if (0) /* WSL2: skip PA force, use VA */ {
    args = push_arg(args, &argcount, _sprintf_alloc("--iova-mode=pa"));
```

**Patch 2 — `module/bdev/malloc/bdev_malloc.c`**

The malloc bdev allocates its backing buffer using `spdk_zmalloc` with `SPDK_MALLOC_DMA` flag, requiring 2MB-aligned hugepage memory. On WSL2 this allocation fails. The patch replaces it with plain `calloc`:

```c
// original:
mdisk->malloc_buf = spdk_zmalloc(opts->num_blocks * block_size,
                                  2 * 1024 * 1024, NULL,
                                  opts->numa_id, SPDK_MALLOC_DMA);
// patched:
mdisk->malloc_buf = calloc(1, opts->num_blocks * block_size);
```

**Do these patches affect the experiment results?**

No, and this is important to understand:

- Patch 1 is pure infrastructure. Your C file already sets `opts.iova_mode = "va"` explicitly.
- Patch 2 only affects the bdev's internal backing buffer (the simulated disk storage itself), not your I/O buffers. The DMA vs non-DMA comparison measures your `aligned_alloc` buffer vs your `malloc` buffer going through SPDK's I/O path, the bdev's internal buffer is irrelevant to this measurement.

**Results look similar on WSL2**

On WSL2 with a malloc bdev, both the aligned and unaligned I/O paths ultimately hit the same RAM through the same code path — there is no real DMA hardware. The meaningful difference between DMA and non-DMA I/O only appears on real NVMe hardware where:
- DMA path: hugepage-pinned buffer → NVMe controller reads directly from physical address → zero CPU copy
- non-DMA path: normal heap buffer → must be bounce-copied to a DMA-safe region → extra CPU work → may fail entirely on strict controllers

### WSL2 memory configuration

SPDK's default memory pools are too large for WSL2. The program reduces them before init:

```c
// iobuf pools: default 8192×8KB + 1024×132KB = ~200MB → reduced to 256/32
iobuf_opts.small_pool_count = 256;
iobuf_opts.large_pool_count = 32;

// bdev_io pool: default 65535 entries → reduced to 512 (minimum viable)
bdev_opts.bdev_io_pool_size  = 512;
bdev_opts.bdev_io_cache_size = 128;

// hugepage budget: 64MB (32 × 2MB pages)
opts.mem_size        = 64;
opts.unlink_hugepage = false;   // keep hugepages mapped after init
opts.iova_mode       = "va";    // WSL2 has no IOMMU
```

### WSL2 memory limit

Create `C:\Users\<you>\.wslconfig` to prevent WSL2 from consuming all system RAM:

```ini
[wsl2]
memory=2GB
swap=512MB
processors=2
```

---

## Porting to real NVMe hardware

To run this on real NVMe (bare metal Linux):

1. **Revert both SPDK patches**  not needed on real hardware with IOMMU
2. **Run `setup.sh`** before your program:
   ```bash
   sudo scripts/setup.sh
   ```
   This unbinds NVMe from the kernel driver and binds it to VFIO/UIO for SPDK's direct access.
3. **Replace `create_malloc_disk`** with `bdev_nvme_attach_ctrlr` to use real NVMe
4. **Replace `aligned_alloc`** with `spdk_dma_zmalloc` for the DMA buffer path:
   ```c
   // WSL2 (current):
   g_ctx.dma_write_buf = aligned_alloc(4096, BLOCK_SIZE);

   // Real NVMe (replace with):
   g_ctx.dma_write_buf = spdk_dma_zmalloc(BLOCK_SIZE, 0x1000, NULL);
   ```
5. **Replace `free`** with `spdk_dma_free` in `cleanup()` for DMA buffers
6. **Run `setup.sh reset`** after your program exits

---

## Files in this repo

| File | Description |
|---|---|
| `spdk_ram_bdev_bench.c` | Main C source: full benchmark program |
| `Makefile` | Build file: place in `spdk/examples/bdev/experiment/` |
| `README.md` | This file |

## Further reading

- [SPDK documentation](https://spdk.io/doc/)
- [SPDK bdev API](https://spdk.io/doc/bdev.html)
- [DPDK hugepage memory](https://doc.dpdk.org/guides/linux_gsg/sys_reqs.html)
- [SPDK GitHub](https://github.com/spdk/spdk)

---

## Environment

| Item | Version |
|---|---|
| SPDK | v26.05-pre git sha1 db900a7bd |
| DPDK | 25.11.0 |
| OS | Ubuntu 24.04 on WSL2 |
| WSL | 2.6.3.0 |
| Kernel | 6.6.87.2-1 |
| GCC | 13.2.0 |
