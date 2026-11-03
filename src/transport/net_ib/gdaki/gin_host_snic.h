/* SPDX-License-Identifier: Apache-2.0 */
#ifndef PCCL_GIN_HOST_SNIC_H_
#define PCCL_GIN_HOST_SNIC_H_

#include <dlfcn.h>
#include <algorithm>
#include <memory>
#include <vector>
#include "pccl_device/gin/gdaki/gin_snic_common.h"

NCCL_PARAM(GinSnicGdakiDepth, "GIN_SNIC_GDAKI_DEPTH", 32);

namespace {
// Public GPU queue ABI from HPN SDK 2.1.2 trocedv.h. No provider-private QP
// offsets or mlx5 structures are used for SNIC resources.
struct SnicQpContext {
  int allocType;
  void* sq; size_t sqLength;
  void* rq; size_t rqLength;
  void* recvDb;
  void *sqTail, *rqTail;
};
struct SnicCqContext {
  int allocType;
  void* address; size_t length;
  void* tail;
  uint32_t cqn, flags;
};
static_assert(sizeof(SnicQpContext) == 64 && sizeof(SnicCqContext) == 40, "HPN GPU queue ABI mismatch");
struct SnicApi {
  void* library = dlopen("libtroce.so.1", RTLD_NOW | RTLD_LOCAL);
  ibv_qp* (*createQp)(ibv_pd*, ibv_qp_init_attr*, SnicQpContext*) = nullptr;
  int (*destroyQp)(ibv_qp*, SnicQpContext*) = nullptr;
  ibv_cq* (*createCq)(ibv_context*, int, ibv_comp_channel*, int, SnicCqContext*) = nullptr;
  int (*destroyCq)(ibv_cq*, SnicCqContext*) = nullptr;
  SnicApi() {
    if (!library) return;
    createQp = reinterpret_cast<decltype(createQp)>(dlsym(library, "troce_create_qp_with_context"));
    destroyQp = reinterpret_cast<decltype(destroyQp)>(dlsym(library, "troce_destroy_qp_with_context"));
    createCq = reinterpret_cast<decltype(createCq)>(dlsym(library, "troce_create_cq_with_context"));
    destroyCq = reinterpret_cast<decltype(destroyCq)>(dlsym(library, "troce_destroy_cq_with_context"));
  }
  bool ready() const { return createQp && destroyQp && createCq && destroyCq; }
};
static SnicApi& snicApi() { static SnicApi api; return api; }
static bool snicDevice(ibv_context* context) {
  return strncmp(wrap_ibv_get_device_name(context->device), "troce", 5) == 0;
}
struct SnicMemory {
  void *raw = nullptr, *pointer = nullptr;
  ncclResult_t allocate(size_t bytes, int fill = 0) {
    if (!bytes) return ncclSuccess;
    HGGCCHECK(hggcMalloc(&raw, bytes + 4096));
    pointer = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(raw) + 4095) & ~uintptr_t(4095));
    HGGCCHECK(hggcMemset(pointer, fill, bytes));
    return ncclSuccess;
  }
  ~SnicMemory() { if (raw) (void)hggcFree(raw); }
};
struct SnicBuffer {
  SnicMemory data, keys;
  ibv_mr* mr = nullptr;
  ncclResult_t allocate(ncclGinIbCollComm* comm, size_t elements) {
    if (!elements) return ncclSuccess;
    NCCLCHECK(data.allocate(elements * sizeof(uint64_t)));
    NCCLCHECK(gdakiRegMr(&mr, comm->ib.pd, data.pointer, elements * sizeof(uint64_t),
                         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ |
                         IBV_ACCESS_REMOTE_ATOMIC, true));
    std::vector<__be32> rkeys(comm->nranks);
    __be32 key = htobe32(mr->rkey);
    NCCLCHECK(comm->allGather(comm, &key, rkeys.data(), sizeof(key)));
    NCCLCHECK(keys.allocate(rkeys.size() * sizeof(key)));
    HGGCCHECK(hggcMemcpy(keys.pointer, rkeys.data(), rkeys.size() * sizeof(key), hggcMemcpyHostToDevice));
    return ncclSuccess;
  }
  pcclGinGdakiGlobalGPUBufferTable<uint64_t> view(unsigned offset) {
    return {data.pointer ? static_cast<uint64_t*>(data.pointer) + offset : nullptr,
            static_cast<__be32*>(keys.pointer), mr ? htobe32(mr->lkey) : 0, offset};
  }
  ~SnicBuffer() { if (mr) (void)wrap_ibv_dereg_mr(mr); }
};
struct SnicHostQueue {
  SnicMemory sq, cqMemory, recvDb;
  SnicQpContext qpContext{};
  SnicCqContext cqContext{};
  ibv_qp *tx = nullptr, *rx = nullptr;
  ibv_cq *gpuCq = nullptr, *hostCq = nullptr;
  ~SnicHostQueue() {
    if (tx) (void)snicApi().destroyQp(tx, &qpContext);
    if (rx) (void)wrap_ibv_destroy_qp(rx);
    if (gpuCq) (void)snicApi().destroyCq(gpuCq, &cqContext);
    if (hostCq) (void)wrap_ibv_destroy_cq(hostCq);
  }
};
static constexpr uint64_t SNIC_CONTEXT_MAGIC = 0x534e49434744414bULL;
struct SnicHostContext {
  uint64_t magic = SNIC_CONTEXT_MAGIC;
  SnicMemory deviceQueues, deviceContexts, scratch;
  SnicBuffer counters, signals;
  ibv_mr* scratchMr = nullptr;
  uint32_t *hostError = nullptr, *deviceError = nullptr;
  ncclNetDeviceHandle_t handle{};
  std::vector<std::unique_ptr<SnicHostQueue>> queues;
  std::vector<void*> mappedPages;
  ncclResult_t map(void* address, uint64_t** out) {
    uintptr_t base = reinterpret_cast<uintptr_t>(address) & ~uintptr_t(4095);
    void* page = reinterpret_cast<void*>(base);
    if (std::find(mappedPages.begin(), mappedPages.end(), page) == mappedPages.end()) {
      HGGCCHECK(hggcHostRegister(page, 4096,
                               hggcHostRegisterPortable | hggcHostRegisterMapped | hggcHostRegisterIoMemory));
      mappedPages.push_back(page);
    }
    void* device = nullptr;
    HGGCCHECK(hggcHostGetDevicePointer(&device, page, 0));
    *out = reinterpret_cast<uint64_t*>(
        reinterpret_cast<uintptr_t>(device) + reinterpret_cast<uintptr_t>(address) - base);
    return ncclSuccess;
  }
  ~SnicHostContext() {
    for (void* page : mappedPages) (void)hggcHostUnregister(page);
    queues.clear();
    if (scratchMr) (void)wrap_ibv_dereg_mr(scratchMr);
    if (hostError) (void)hggcFreeHost(hostError);
  }
};
static bool snicContext(void* context) {
  if (!context) return false;
  uint64_t magic;
  memcpy(&magic, context, sizeof(magic));
  return magic == SNIC_CONTEXT_MAGIC;
}
struct SnicEndpoint {
  uint32_t txQpn, rxQpn;
  ibv_gid gid;
  enum ibv_mtu mtu;
  uint16_t lid;
};
static ncclResult_t snicConnect(ibv_qp* qp, uint32_t peerQpn, const SnicEndpoint& peer,
                                int gidIndex, const ibv_port_attr& port, int depth, int sl, int tc) {
  ibv_qp_attr attr{};
  attr.qp_state = IBV_QPS_INIT;
  attr.pkey_index = ncclParamIbPkey(); attr.port_num = 1;
  attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
  NCCLCHECK(wrap_ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS));
  attr = {};
  attr.qp_state = IBV_QPS_RTR;
  attr.path_mtu = std::min(port.active_mtu, peer.mtu);
  attr.dest_qp_num = peerQpn; attr.rq_psn = 0;
  attr.max_dest_rd_atomic = depth; attr.min_rnr_timer = 12;
  attr.ah_attr.port_num = 1; attr.ah_attr.sl = sl;
  if (port.link_layer == IBV_LINK_LAYER_ETHERNET) {
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.dgid = peer.gid;
    attr.ah_attr.grh.sgid_index = gidIndex;
    attr.ah_attr.grh.hop_limit = 255;
    attr.ah_attr.grh.traffic_class = tc;
  } else attr.ah_attr.dlid = peer.lid;
  NCCLCHECK(wrap_ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                              IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER));
  attr = {};
  attr.qp_state = IBV_QPS_RTS; attr.sq_psn = 0;
  attr.timeout = ncclParamIbTimeout(); attr.retry_cnt = ncclParamIbRetryCnt(); attr.rnr_retry = 7;
  attr.max_rd_atomic = depth;
  NCCLCHECK(wrap_ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                              IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC));
  return ncclSuccess;
}
static ncclResult_t snicCreateContext(ncclGinIbCollComm* comm, int nSignals, int nCounters, int nContexts,
                                     int queueDepth, int trafficClass, void** outContext,
                                     ncclNetDeviceHandle_t** outDevice) {
  if (!snicApi().ready() || nContexts <= 0 || nSignals < 0 || nCounters < 0) {
    WARN("SNIC native GDAKI requires the HPN GPU queue API and valid context dimensions");
    return ncclInvalidUsage;
  }
  const int pipelineDepth = ncclParamGinSnicGdakiDepth();
  if (pipelineDepth < 1 || pipelineDepth > 128) {
    WARN("PCCL_GIN_SNIC_GDAKI_DEPTH must be between 1 and 128");
    return ncclInvalidUsage;
  }
  std::unique_ptr<SnicHostContext> ctx(new SnicHostContext());
  const int count = nContexts * comm->nranks;
  ibv_device_attr caps{};
  ibv_port_attr port{};
  int gidIndex = 0;
  ibv_gid gid{};
  NCCLCHECK(wrap_ibv_query_device(comm->ib.context, &caps));
  if (caps.atomic_cap == IBV_ATOMIC_NONE || caps.max_qp_rd_atom == 0 || caps.max_qp_init_rd_atom == 0) {
    WARN("SNIC native GDAKI requires RC read and remote atomic support"); return ncclInvalidUsage;
  }
  int depth = std::min(16, std::min(caps.max_qp_rd_atom, caps.max_qp_init_rd_atom));
  NCCLCHECK(wrap_ibv_query_port(comm->ib.context, 1, &port));
  NCCLCHECK(comm->getGidIndex(comm->ib.context, 1, &port, &gidIndex));
  NCCLCHECK(wrap_ibv_query_gid(comm->ib.context, 1, gidIndex, &gid));
  NCCLCHECK(ctx->signals.allocate(comm, size_t(nSignals) * nContexts));
  NCCLCHECK(ctx->counters.allocate(comm, size_t(nCounters) * nContexts));
  NCCLCHECK(ctx->scratch.allocate(size_t(count) * 8192));
  NCCLCHECK(gdakiRegMr(&ctx->scratchMr, comm->ib.pd, ctx->scratch.pointer, size_t(count) * 8192,
                       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE |
                       IBV_ACCESS_REMOTE_ATOMIC, true));
  HGGCCHECK(hggcHostAlloc(reinterpret_cast<void**>(&ctx->hostError), 4096,
                         hggcHostAllocMapped | hggcHostAllocPortable));
  *ctx->hostError = 0;
  HGGCCHECK(hggcHostGetDevicePointer(reinterpret_cast<void**>(&ctx->deviceError), ctx->hostError, 0));
  std::vector<pcclGinSnicQp> deviceQueues(count);
  for (int i = 0; i < count; ++i) {
    ctx->queues.emplace_back(new SnicHostQueue());
    auto& q = *ctx->queues.back();
    NCCLCHECK(q.sq.allocate(32768)); NCCLCHECK(q.cqMemory.allocate(8192, 0xff)); NCCLCHECK(q.recvDb.allocate(4096));
    q.cqContext = {1, q.cqMemory.pointer, 8192, nullptr, 0, 2};
    q.gpuCq = snicApi().createCq(comm->ib.context, 128, nullptr, 0, &q.cqContext);
    if (!q.gpuCq) { WARN("SNIC GPU CQ creation failed: %s", strerror(errno)); return ncclSystemError; }
    ibv_qp_init_attr attr{};
    attr.qp_type = IBV_QPT_RC; attr.send_cq = q.gpuCq; attr.recv_cq = q.gpuCq; attr.sq_sig_all = 1;
    attr.cap.max_send_wr = 128; attr.cap.max_send_sge = 1;
    q.qpContext = {1, q.sq.pointer, 32768, nullptr, 0, q.recvDb.pointer, nullptr, nullptr};
    q.tx = snicApi().createQp(comm->ib.pd, &attr, &q.qpContext);
    if (!q.tx) { WARN("SNIC GPU TX QP creation failed: %s", strerror(errno)); return ncclSystemError; }
    // HPN's GPU QP is used only as the initiator. A normal, hardware responder
    // avoids the SNIC GPU-RX/zero-RQ failure. It never needs CPU post/poll work.
    NCCLCHECK(wrap_ibv_create_cq(&q.hostCq, comm->ib.context, 128, nullptr, nullptr, 0));
    attr = {};
    attr.qp_type = IBV_QPT_RC; attr.send_cq = q.hostCq; attr.recv_cq = q.hostCq;
    attr.cap.max_send_wr = 128; attr.cap.max_recv_wr = 128;
    attr.cap.max_send_sge = 1; attr.cap.max_recv_sge = 1;
    NCCLCHECK(wrap_ibv_create_qp(&q.rx, comm->ib.pd, &attr));
    auto& gpu = deviceQueues[i];
    gpu.sq = static_cast<uint32_t*>(q.sq.pointer); gpu.cq = static_cast<uint32_t*>(q.cqMemory.pointer);
    NCCLCHECK(ctx->map(q.qpContext.sqTail, &gpu.sqDb));
    NCCLCHECK(ctx->map(q.cqContext.tail, &gpu.cqDb));
    gpu.qpn = q.tx->qp_num; gpu.cqn = q.cqContext.cqn;
    gpu.maxBytes = std::min(uint32_t(1u << 30), port.max_msg_sz);
    if (!gpu.maxBytes) return ncclInvalidUsage;
    gpu.scratch = static_cast<unsigned char*>(ctx->scratch.pointer) + size_t(i) * 8192;
    gpu.scratchOffset = size_t(i) * 8192; gpu.scratchKey = htobe32(ctx->scratchMr->lkey);
    gpu.depth = pipelineDepth;
    gpu.error = ctx->deviceError;
  }
  const int sl = ncclParamIbSl() != -1 ? ncclParamIbSl()
      : trafficClass == NCCL_NET_TRAFFIC_CLASS_UNDEF ? 0 : trafficClass;
  const int tc = ncclParamIbTc() != -1 ? ncclParamIbTc()
      : trafficClass == NCCL_NET_TRAFFIC_CLASS_UNDEF ? 0 : trafficClass;
  std::vector<SnicEndpoint> local(comm->nranks), remote(comm->nranks);
  for (int c = 0; c < nContexts; ++c) {
    for (int p = 0; p < comm->nranks; ++p) {
      auto& q = *ctx->queues[c * comm->nranks + p];
      local[p] = {q.tx->qp_num, q.rx->qp_num, gid, port.active_mtu, port.lid};
    }
    NCCLCHECK(comm->allToAll(comm, local.data(), remote.data(), sizeof(SnicEndpoint)));
    for (int p = 0; p < comm->nranks; ++p) {
      auto& q = *ctx->queues[c * comm->nranks + p];
      NCCLCHECK(snicConnect(q.tx, remote[p].rxQpn, remote[p], gidIndex, port, depth, sl, tc));
      NCCLCHECK(snicConnect(q.rx, remote[p].txQpn, remote[p], gidIndex, port, depth, sl, tc));
    }
  }
  NCCLCHECK(ctx->deviceQueues.allocate(deviceQueues.size() * sizeof(pcclGinSnicQp)));
  HGGCCHECK(hggcMemcpy(ctx->deviceQueues.pointer, deviceQueues.data(),
                       deviceQueues.size() * sizeof(pcclGinSnicQp), hggcMemcpyHostToDevice));
  std::vector<pcclGinGdakiGPUContext> contexts(nContexts);
  for (int c = 0; c < nContexts; ++c) {
    contexts[c].gdqp = reinterpret_cast<doca_gpu_dev_verbs_qp*>(
        static_cast<pcclGinSnicQp*>(ctx->deviceQueues.pointer) + c * comm->nranks);
    contexts[c].companion_gdqp = nullptr;
    contexts[c].signals_table = ctx->signals.view(c * nSignals);
    contexts[c].counters_table = ctx->counters.view(c * nCounters);
    contexts[c].sink_buffer_lkey = htobe32(ctx->scratchMr->lkey);
  }
  NCCLCHECK(ctx->deviceContexts.allocate(contexts.size() * sizeof(pcclGinGdakiGPUContext)));
  HGGCCHECK(hggcMemcpy(ctx->deviceContexts.pointer, contexts.data(),
                       contexts.size() * sizeof(pcclGinGdakiGPUContext), hggcMemcpyHostToDevice));
  ctx->handle.netDeviceType = NCCL_NET_DEVICE_GIN_GDAKI;
  ctx->handle.netDeviceVersion = PCCL_GIN_SNIC_GDAKI_VERSION;
  ctx->handle.handle = ctx->deviceContexts.pointer;
  ctx->handle.size = contexts.size() * sizeof(pcclGinGdakiGPUContext);
  ctx->handle.needsProxyProgress = 0;
  INFO(NCCL_NET, "SNIC native GDAKI: rank=%d ranks=%d contexts=%d queueDepthHint=%d pipelineDepth=%d; "
                 "GPU TX/CQ + hardware RX, no CPU data progress",
       comm->rank, comm->nranks, nContexts, queueDepth, pipelineDepth);
  *outDevice = &ctx->handle;
  *outContext = ctx.release();
  return ncclSuccess;
}
} // namespace
#endif
