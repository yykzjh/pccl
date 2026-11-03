/* SPDX-License-Identifier: Apache-2.0 */
#ifndef PCCL_GIN_SNIC_H_
#define PCCL_GIN_SNIC_H_

#include "gin_snic_common.h"
#include <stdio.h>

namespace pccl { namespace gin { namespace snic {

PCCL_DEVICE_INLINE bool enabled(const pcclGinGdakiGPUContext* ctx) {
  return ctx->companion_gdqp == nullptr;
}
PCCL_DEVICE_INLINE pcclGinSnicQp* queue(pcclGinGdakiGPUContext* ctx, int peer) {
  return reinterpret_cast<pcclGinSnicQp*>(ctx->gdqp) + peer;
}
PCCL_DEVICE_INLINE uint32_t be32(uint32_t x) {
  return (x >> 24) | ((x >> 8) & 0xff00) | ((x << 8) & 0xff0000) | (x << 24);
}
PCCL_DEVICE_INLINE void fence() {
  hggc::atomic_thread_fence(hggc::memory_order_seq_cst, hggc::thread_scope_system);
}
PCCL_DEVICE_INLINE void fail(pcclGinSnicQp* q, unsigned status) {
  hggc::atomic_ref<uint32_t, hggc::thread_scope_system>(*q->error).store(status, hggc::memory_order_release);
  printf("PCCL SNIC GDAKI error: qpn=%u producer=%u consumer=%u status=%u\n",
         q->qpn, q->producer, q->consumer, status);
  __builtin_trap();
}
template <typename Work>
PCCL_DEVICE_INLINE void locked(pcclGinSnicQp* q, Work work) {
  hggc::atomic_ref<uint32_t, hggc::thread_scope_system> mutex(q->mutex);
  uint64_t start = clock64();
  uint32_t progress = q->consumer;
  bool done = false;
  while (!done) {
    unsigned expected = 0;
    if (mutex.compare_exchange_strong(expected, 1, hggc::memory_order_acquire, hggc::memory_order_relaxed)) {
      // Keep the whole critical section inside the retry loop. On SIMD warps,
      // a winning lane must release before losing lanes retry the same QP.
      work();
      mutex.store(0, hggc::memory_order_release);
      done = true;
    }
    uint32_t now = q->consumer;
    if (now != progress) { progress = now; start = clock64(); }
    if (!done && clock64() - start > 10000000000ULL) fail(q, 0x10001);
  }
}
PCCL_DEVICE_INLINE void ring(uint64_t* address, uint32_t a, uint32_t b) {
  fence();
  *reinterpret_cast<volatile uint64_t*>(address) = uint64_t(be32(a)) | (uint64_t(be32(b)) << 32);
  fence();
}
PCCL_DEVICE_INLINE uint32_t cqWord(uint32_t* p) {
  return be32(*reinterpret_cast<volatile uint32_t*>(p));
}
// Wire layouts follow HPN SDK 2.1.2 troce_api.h, not mlx5 WQEs. SQ entries are
// 64-byte basic blocks; a CQ entry is 64 bytes. Both rings wrap independently.
PCCL_DEVICE_INLINE void word(pcclGinSnicQp* q, unsigned index, uint32_t value) {
  q->sq[((q->producer * 16) + index) & 8191] = be32(value);
}
// All queue state is protected by locked(). PI counts 64-byte basic blocks,
// while posted/completed count WQEs; atomic WQEs consume two basic blocks.
PCCL_DEVICE_INLINE void submit(pcclGinSnicQp* q) {
  if (q->notified != q->producer) {
    ring(q->sqDb, q->qpn << 8, (q->producer & 65535) << 16);
    q->notified = q->producer;
  }
}
PCCL_DEVICE_INLINE bool reap(pcclGinSnicQp* q) {
  uint32_t first = q->consumer;
  while (q->completed != q->posted) {
    uint32_t* entry = q->cq + (q->consumer & 127) * 16;
    if ((cqWord(entry + 15) & 1) != ((q->consumer >> 7) & 1)) break;
    fence(); // The NIC publishes the owner after the rest of the CQE.
    uint32_t status = cqWord(entry + 14);
    if ((status >> 24) != 0) fail(q, status >> 24);
    if ((status & 65535) != (q->pending[q->consumer & 127] & 65535)) fail(q, 0x10003);
    ++q->consumer;
    ++q->completed;
  }
  if (q->consumer != first) {
    ring(q->cqDb, (3u << 26) | ((q->cqn & 65535) << 8), (q->consumer & 65535) << 16);
    return true;
  }
  return false;
}
PCCL_DEVICE_INLINE void waitLocked(pcclGinSnicQp* q, uint64_t ticket, uint32_t* abortFlag = nullptr) {
  submit(q);
  uint64_t start = clock64();
  while (q->completed < ticket) {
    if (reap(q)) start = clock64();
    if (abortFlag && *reinterpret_cast<volatile uint32_t*>(abortFlag)) return;
    if (clock64() - start > 10000000000ULL) fail(q, 0x10002);
  }
}
PCCL_DEVICE_INLINE void reserve(pcclGinSnicQp* q) {
  if (q->posted - q->completed >= q->depth) {
    // Publish staged WQEs before waiting for credit. At most 128 WQEs and
    // 256 SQ basic blocks can be live, within the CQ and 512-block SQ rings.
    waitLocked(q, q->completed + 1);
  }
}
PCCL_DEVICE_INLINE void issue(pcclGinSnicQp* q, unsigned opcode, uint64_t remote,
                             __be32 remoteKey, uint64_t local, __be32 localKey,
                             unsigned bytes, uint64_t add = 0) {
  reserve(q);
  const unsigned blocks = opcode == 7 ? 2 : 1;
  for (unsigned i = 0; i < blocks * 16; ++i) word(q, i, 0);
  word(q, 0, (q->qpn << 8) | ((blocks - 1) << 27));
  word(q, 1, (2u << 30) | (opcode << 18) | (q->producer & 65535));
  word(q, 2, (1u << 18) | (q->producer & 65535));
  word(q, 4, bytes);
  word(q, 5, remote >> 32); word(q, 6, remote);
  word(q, 7, be32(remoteKey)); word(q, 8, bytes);
  unsigned segment = 12;
  if (opcode == 7) {
    word(q, 9, add >> 32); word(q, 10, add);
    segment = 16;
  }
  word(q, segment, local >> 32); word(q, segment + 1, local);
  word(q, segment + 2, bytes); word(q, segment + 3, be32(localKey));
  q->pending[q->posted & 127] = q->producer;
  q->producer += blocks;
  ++q->posted;
}
PCCL_DEVICE_INLINE void signal(pcclGinSnicQp* q, size_t offset, __be32 key, uint64_t value) {
  reserve(q);
  uint64_t slot = (q->posted & 127) * 64;
  issue(q, 7, offset, key, q->scratchOffset + slot, q->scratchKey, 8, value);
}
PCCL_DEVICE_INLINE void transfer(pcclGinSnicQp* q, unsigned opcode, uint64_t remote,
                                __be32 remoteKey, uint64_t local, __be32 localKey, size_t bytes) {
  while (bytes) {
    unsigned amount = bytes > q->maxBytes ? q->maxBytes : unsigned(bytes);
    issue(q, opcode, remote, remoteKey, local, localKey, amount);
    remote += amount; local += amount; bytes -= amount;
  }
}
PCCL_DEVICE_INLINE void put(pcclGinCtx ctx, pcclGinGdakiGPUContext* gpu, int peer, bool hasWins,
                           pcclGinWindow_t dstWin, size_t dstOff, pcclGinWindow_t srcWin, size_t srcOff,
                           size_t bytes, bool hasSignal, size_t signalOff, __be32 signalKey,
                           pcclGinSignalOp_t signalOp, uint64_t signalArg, bool hasCounter,
                           pcclGinCounter_t counterId, bool aggregate = false) {
  auto* q = queue(gpu, peer);
  locked(q, [&] {
    if (hasWins) {
      auto* dst = reinterpret_cast<pcclGinGdakiMemHandle*>(dstWin);
      auto* src = reinterpret_cast<pcclGinGdakiMemHandle*>(srcWin);
      transfer(q, 4, dstOff, dst->rkeys[peer], srcOff, src->lkey, bytes);
    }
    if (hasSignal) signal(q, signalOff, signalKey, signalOp == pcclGinSignalInc ? 1 : signalArg);
    if (!aggregate || hasCounter || q->depth == 1) submit(q);
    if (hasCounter || q->depth == 1) waitLocked(q, q->posted);
    if (hasCounter) {
      // Publish local completion only after the NIC has consumed the source.
      fence();
      hggc::atomic_ref<uint64_t, hggc::thread_scope_system>(gpu->counters_table.buffer[counterId])
        .fetch_add(1, hggc::memory_order_release);
    }
  });
}
template <typename T>
PCCL_DEVICE_INLINE void putValue(pcclGinGdakiGPUContext* gpu, int peer,
                                pcclGinWindow_t dstWin, size_t dstOff, T value,
                                bool hasSignal, size_t signalOff, __be32 signalKey,
                                pcclGinSignalOp_t signalOp, uint64_t signalArg, bool aggregate = false) {
  static_assert(sizeof(T) <= 16, "SNIC inline values must fit in the registered scratch slot");
  auto* q = queue(gpu, peer);
  auto* dst = reinterpret_cast<pcclGinGdakiMemHandle*>(dstWin);
  locked(q, [&] {
    reserve(q);
    const uint64_t slot = (q->posted & 127) * 64 + 16;
    // A staging slot belongs to its WQE until its CQE is consumed.
    __builtin_memcpy(q->scratch + slot, &value, sizeof(T));
    transfer(q, 4, dstOff, dst->rkeys[peer], q->scratchOffset + slot, q->scratchKey, sizeof(T));
    if (hasSignal) signal(q, signalOff, signalKey, signalOp == pcclGinSignalInc ? 1 : signalArg);
    if (!aggregate || q->depth == 1) submit(q);
    if (q->depth == 1) waitLocked(q, q->posted);
  });
}
PCCL_DEVICE_INLINE void get(pcclGinGdakiGPUContext* gpu, int peer,
                           pcclGinWindow_t remoteWin, size_t remoteOff,
                           pcclGinWindow_t localWin, size_t localOff, size_t bytes) {
  auto* q = queue(gpu, peer);
  auto* remote = reinterpret_cast<pcclGinGdakiMemHandle*>(remoteWin);
  auto* local = reinterpret_cast<pcclGinGdakiMemHandle*>(localWin);
  locked(q, [&] {
    transfer(q, 0, remoteOff, remote->rkeys[peer], localOff, local->lkey, bytes);
    submit(q);
    if (q->depth == 1) waitLocked(q, q->posted);
  });
}
PCCL_DEVICE_INLINE uint64_t flushAsync(pcclGinGdakiGPUContext* gpu, int peer) {
  auto* q = queue(gpu, peer);
  uint64_t ticket = 0;
  locked(q, [&] { submit(q); ticket = q->posted; });
  return ticket;
}
PCCL_DEVICE_INLINE void wait(pcclGinGdakiGPUContext* gpu, int peer, uint64_t ticket, uint32_t* abortFlag) {
  auto* q = queue(gpu, peer);
  locked(q, [&] { waitLocked(q, ticket, abortFlag); });
  fence();
}
PCCL_DEVICE_INLINE void flush(pcclGinGdakiGPUContext* gpu, int peer, uint32_t* abortFlag = nullptr) {
  auto* q = queue(gpu, peer);
  locked(q, [&] { waitLocked(q, q->posted, abortFlag); });
}

}}} // namespace pccl::gin::snic
#endif
