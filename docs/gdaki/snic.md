# Native SNIC GDAKI

This experimental backend targets SNIC1.0 / HPN SDK 2.1.2 on PPU. It uses the
public TROCE GPU queue interface for GPU-owned send queues, completion queues,
and MMIO doorbells. Ordinary verbs QPs provide hardware responders; there is
no CPU posting, polling, or data forwarding in the data path.

Build the library and rebuild device consumers against the same installed
headers. Existing precompiled MLX5 GDAKI kernels cannot consume SNIC contexts.
The backend is disabled by default; enabling it asserts that the application
uses matching device headers. Use a fresh DeepEP JIT cache after rebuilding.
Do not replace HPN's verbs/provider libraries with distribution RDMA packages.

```bash
bash build.sh --ppu_archs ppu_15 --hggcrt_version 13 --disable_tests --no_ext_kernel --cpu_num 64
export PCCL_GIN_TYPE=3
export PCCL_GIN_SNIC_GDAKI_ENABLE=1
export PCCL_GIN_SNIC_GDAKI_DEPTH=32
# Also provide the deployment's HCA list, GID index, and management interface.
```

`PCCL_GIN_SNIC_GDAKI_DEPTH` bounds outstanding WQEs per peer/context (1–128).
Depth 1 provides a blocking reference mode. Each queue has 128 CQ entries and
512 send-queue basic blocks; an atomic consumes two basic blocks. Producer and
completion indices therefore advance in different units. Each outstanding WQE
owns a separate registered staging slot until its CQE is consumed. Writes and
following signals share one RC queue and are submitted together where possible.
Aggregated requests defer their doorbell until a non-aggregated operation,
flush, or credit pressure publishes the batch.

`flushAsync` snapshots the queue's posted-WQE sequence. `wait` waits for that
prefix, and credit reclamation validates CQ ownership, completion status, and
WQE identity before reuse. Local completion counters are published after the
associated source data has been consumed. Counter-bearing operations currently
drain the queue; they are correct but do not gain the same overlap as puts
without a local counter. Flush/wait honor an abort flag when provided.

The backend requires homogeneous TROCE peers, GPU peer-memory registration,
strict ordering, and the HPN GPU queue API. GPU queues are initiators only; the
responder uses the ordinary verbs allocation path. Queue geometry is fixed
independently of the application's queue-depth hint. The native context ABI is
private and must match its device headers. This branch does not implement the
provider-descriptor ABI proposed for PCCL main.

A CPU harness exercises the actual device queue implementation with emulated
NIC completions. It covers bounded credits, staging lifetime, mixed write/atomic
SQ wrap, CQ ownership wrap, 16-bit wire-index wrap, completion tickets, aborted
waits, segmented Get, 16-byte inline values, local counters, and CQ errors:

```bash
c++ -std=c++17 -O2 -Isrc/include tests/src/snic_gdaki_queue_test.cc -o /tmp/snic-gdaki-queue-test
/tmp/snic-gdaki-queue-test
```

This harness does not validate DMA visibility, GPU scheduling, provider behavior,
or hardware throughput. Those require matching host/device builds and tests on
SNIC hardware. Performance evidence must identify the exact source revision,
queue depth, application shape, warmup, timing mode, and whether runs overlapped.

## Source-build measurements (2026-11-03)

Three sequential paired runs used two nodes, each with 8 PPU accelerators and
4 x 400 Gb/s SNICs. DeepEP 2.2.1+v0.1.0.ppu2.2.0 used 1024 tokens/rank,
hidden size 7168, 256 experts, top-k 8, 36 SMs and 8 QPs. Both modes used the
same rebuilt PCCL 2.2.2 source: depth 1 is a blocking reference, depth 32 is the
pipelined implementation. Each mode ran BF16/FP8 with CUDA Graphs, 3 warmups and
10 timed iterations, discarding the first timed sample. All 192 rank/dtype
correctness checks across the six runs passed.

Node payload throughput, averaged over three runs and both nodes (GB/s):

| Type / operation | Depth 1 | Depth 32 | Change |
|---|---:|---:|---:|
| bf16 / cached_dispatch | 13.76 | 17.49 | +27.1% |
| bf16 / combine | 20.98 | 29.97 | +42.9% |
| fp8 / cached_dispatch | 8.67 | 12.08 | +39.3% |
| fp8 / combine | 20.96 | 29.85 | +42.4% |

All four NICs carried traffic. Depth 128 did not provide a material gain over
32 in the exploratory runs. These are end-to-end DeepEP measurements, not
standalone NIC link bandwidth. They remain far below the node's 200 GB/s
physical one-way ceiling; this branch does not claim peak-bandwidth acceptance.
The upstream tree lacks this native SNIC backend, so depth 1 is not a measurement
of unmodified upstream PCCL. Mellanox hardware regression and dedicated async
Get hardware stress were not performed for this revision.
