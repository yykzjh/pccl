/* SPDX-License-Identifier: Apache-2.0 */
// CPU protocol harness for the actual device queue implementation. It replaces
// GPU intrinsics and NIC DMA, and does not substitute for hardware validation.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#define PCCL_DEVICE_INLINE inline
namespace hggc {
enum thread_scope { thread_scope_system };
using std::memory_order_acquire;
using std::memory_order_release;
using std::memory_order_relaxed;
using std::memory_order_seq_cst;
template <typename T, thread_scope> struct atomic_ref {
  T& value;
  explicit atomic_ref(T& v) : value(v) {}
  bool compare_exchange_strong(T& expected, T desired, std::memory_order, std::memory_order) {
    return __atomic_compare_exchange_n(&value, &expected, desired, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
  }
  void store(T v, std::memory_order) { __atomic_store_n(&value, v, __ATOMIC_RELEASE); }
  T fetch_add(T v, std::memory_order) { return __atomic_fetch_add(&value, v, __ATOMIC_RELEASE); }
};
inline void atomic_thread_fence(std::memory_order, thread_scope) {
  std::atomic_thread_fence(std::memory_order_seq_cst);
}
}
#include "pccl_device/gin/gdaki/gin_snic_common.h"
struct pcclGinGdakiMemHandle { __be32* rkeys; __be32 lkey; };
using pcclGinWindow_t = void*;
using pcclGinCounter_t = unsigned;
struct pcclGinCtx {};
enum pcclGinSignalOp_t { pcclGinSignalInc, pcclGinSignalAdd };
struct pcclGinGdakiGPUContext {
  void *gdqp, *companion_gdqp;
  struct { uint64_t* buffer; } counters_table;
};
static void progress();
static uint64_t clock64() {
  progress();
  return std::chrono::steady_clock::now().time_since_epoch().count();
}
#include "pccl_device/gin/gdaki/gin_snic.h"
namespace snic = pccl::gin::snic;

struct Fixture {
  uint32_t sq[8192]{}, cq[2048], error = 0;
  uint64_t sqDb = 0, cqDb = 0, counter = 0;
  unsigned char scratch[8192]{};
  pcclGinSnicQp q{};
  pcclGinGdakiGPUContext gpu{};
  __be32 key = snic::be32(123);
  pcclGinGdakiMemHandle mh{&key, key};
  uint32_t nicPi = 0, nicCi = 0;
  // The emulated NIC can complete only a prefix, leaving newer requests live.
  uint64_t limit = UINT64_MAX, peak = 0;
  bool paused = false;
  explicit Fixture(unsigned depth) {
    memset(cq, 0xff, sizeof(cq));
    q.sq = sq; q.cq = cq; q.sqDb = &sqDb; q.cqDb = &cqDb;
    q.qpn = 0x123; q.cqn = 0x30019; q.depth = depth; q.maxBytes = 17;
    q.scratch = scratch; q.scratchOffset = reinterpret_cast<uint64_t>(scratch);
    q.scratchKey = key; q.error = &error;
    gpu.gdqp = &q; gpu.counters_table.buffer = &counter;
  }
  uint32_t word(unsigned i) { return snic::be32(sq[(nicPi * 16 + i) & 8191]); }
  uint64_t address(unsigned i) { return (uint64_t(word(i)) << 32) | word(i + 1); }
  void complete() {
    peak = std::max(peak, q.posted - q.completed);
    if (paused) return;
    uint32_t tail = snic::be32(sqDb >> 32) >> 16;
    while ((nicPi & 65535) != tail && nicCi < limit) {
      uint32_t header = word(0), control = word(1), identity = word(2);
      unsigned opcode = (control >> 18) & 15, blocks = 1 + ((header >> 27) & 1);
      assert((header & 0x07ffffff) == q.qpn << 8);
      assert((control >> 30) == 2 && (control & 65535) == (nicPi & 65535));
      assert((identity & (1u << 18)) && (identity & 65535) == (nicPi & 65535));
      assert(blocks == (opcode == 7 ? 2 : 1));
      unsigned segment = opcode == 7 ? 16 : 12, bytes = word(4);
      auto* remote = reinterpret_cast<void*>(address(5));
      auto* local = reinterpret_cast<void*>(address(segment));
      assert(word(7) == 123 && word(segment + 3) == 123);
      assert(word(8) == bytes && word(segment + 2) == bytes);
      if (opcode == 4) memcpy(remote, local, bytes);
      else if (opcode == 0) memcpy(local, remote, bytes);
      else {
        assert(opcode == 7 && bytes == 8);
        uint64_t old;
        memcpy(&old, remote, 8);
        uint64_t next = old + address(9);
        memcpy(remote, &next, 8);
        memcpy(local, &old, 8);
      }
      unsigned slot = (nicCi & 127) * 16;
      cq[slot + 14] = snic::be32(nicPi & 65535);
      cq[slot + 15] = snic::be32((nicCi >> 7) & 1);
      nicPi += blocks; ++nicCi;
    }
  }
};
static Fixture* fixture = nullptr;
static void progress() { if (fixture) fixture->complete(); }

static void valuesAndWrap(unsigned depth) {
  Fixture f(depth); fixture = &f;
  std::vector<uint64_t> output(70000);
  for (uint64_t i = 0; i < output.size(); ++i) {
    snic::putValue(&f.gpu, 0, &f.mh, reinterpret_cast<uint64_t>(&output[i]), i + 19,
                   false, 0, 0, pcclGinSignalAdd, 0, true);
    if (i % 11 == 0) snic::put({}, &f.gpu, 0, false, nullptr, 0, nullptr, 0, 0, true,
                              reinterpret_cast<uint64_t>(&f.counter), f.key, pcclGinSignalAdd, 3, false, 0);
  }
  snic::flush(&f.gpu, 0);
  for (uint64_t i = 0; i < output.size(); ++i) assert(output[i] == i + 19);
  assert(f.counter == 3 * ((output.size() + 10) / 11));
  assert(f.q.posted == f.q.completed && f.error == 0 && f.peak <= depth);
  assert(f.q.producer > 65535 && f.q.consumer > 65535);
  fixture = nullptr;
}
static void stagedValuesAndTickets() {
  Fixture f(32); fixture = &f;
  uint64_t values[33]{};
  f.paused = true;
  for (uint64_t i = 0; i < 32; ++i)
    snic::putValue(&f.gpu, 0, &f.mh, reinterpret_cast<uint64_t>(values + i), i + 1,
                   false, 0, 0, pcclGinSignalAdd, 0, true);
  assert(f.sqDb == 0); // Aggregation stages requests without a doorbell.
  uint64_t ticket = snic::flushAsync(&f.gpu, 0);
  assert(ticket == 32 && f.q.completed == 0 && values[0] == 0);
  f.paused = false;
  snic::wait(&f.gpu, 0, ticket, nullptr);
  for (unsigned i = 0; i < 32; ++i) assert(values[i] == i + 1);
  // Waiting an older ticket must not wait for unrelated, newer submissions.
  f.paused = true;
  snic::putValue(&f.gpu, 0, &f.mh, reinterpret_cast<uint64_t>(values + 32), uint64_t(99),
                 false, 0, 0, pcclGinSignalAdd, 0);
  snic::wait(&f.gpu, 0, ticket, nullptr);
  assert(f.q.completed == 32 && f.q.posted == 33 && values[32] == 0);
  uint32_t abortFlag = 1;
  snic::wait(&f.gpu, 0, 33, &abortFlag);
  assert(f.q.completed == 32 && f.q.mutex == 0);
  f.paused = false;
  snic::flush(&f.gpu, 0);
  assert(values[32] == 99);
  fixture = nullptr;
}
static void transfersAndCounter() {
  Fixture f(32); fixture = &f;
  unsigned char source[1024], remote[1024]{}, local[1024]{};
  for (unsigned i = 0; i < sizeof(source); ++i) source[i] = i * 31;
  snic::put({}, &f.gpu, 0, true, &f.mh, reinterpret_cast<uint64_t>(remote), &f.mh,
             reinterpret_cast<uint64_t>(source), sizeof(source), false, 0, 0, pcclGinSignalAdd, 0, true, 0);
  assert(f.counter == 1 && memcmp(source, remote, sizeof(source)) == 0);
  snic::get(&f.gpu, 0, &f.mh, reinterpret_cast<uint64_t>(remote), &f.mh,
             reinterpret_cast<uint64_t>(local), sizeof(local));
  snic::wait(&f.gpu, 0, snic::flushAsync(&f.gpu, 0), nullptr);
  assert(memcmp(source, local, sizeof(source)) == 0 && f.q.completed == f.q.posted);
  fixture = nullptr;
}
static void completionErrors() {
  auto* error = static_cast<uint32_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                           MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  assert(error != MAP_FAILED);
  for (unsigned status : {5u, 0x10003u}) {
    *error = 0;
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
      signal(SIGILL, [](int) { std::_Exit(86); });
      Fixture f(32); // Manually publish a malformed completion.
      f.q.error = error;
      uint64_t value = 1, remote = 0;
      snic::issue(&f.q, 4, reinterpret_cast<uint64_t>(&remote), f.key,
                   reinterpret_cast<uint64_t>(&value), f.key, 8);
      f.cq[14] = snic::be32(status == 5 ? 5u << 24 : 7u);
      f.cq[15] = 0;
      snic::reap(&f.q);
      std::_Exit(1); // A malformed CQE must never release queue credit.
    }
    int result;
    assert(waitpid(pid, &result, 0) == pid);
    assert(WIFEXITED(result) && WEXITSTATUS(result) == 86 && *error == status);
  }
  munmap(error, 4096);
}
static void value16() {
  Fixture f(32); fixture = &f;
  struct Value { uint64_t lo, hi; };
  Value values[32]{};
  f.paused = true;
  for (uint64_t i = 0; i < 32; ++i)
    snic::putValue(&f.gpu, 0, &f.mh, reinterpret_cast<uint64_t>(values + i), Value{i + 1, ~i},
                   false, 0, 0, pcclGinSignalAdd, 0, true);
  f.paused = false;
  snic::flush(&f.gpu, 0);
  for (uint64_t i = 0; i < 32; ++i) assert(values[i].lo == i + 1 && values[i].hi == ~i);
  fixture = nullptr;
}
int main() {
  for (unsigned depth : {1, 2, 32, 128}) valuesAndWrap(depth);
  stagedValuesAndTickets();
  transfersAndCounter();
  value16();
  completionErrors();
  std::cout << "SNIC queue tests passed: credits, staging, PI/CQ wrap, tickets, abort, Get, counters and CQ errors\n";
}
