/* SPDX-License-Identifier: Apache-2.0 */
#ifndef PCCL_GIN_SNIC_COMMON_H_
#define PCCL_GIN_SNIC_COMMON_H_
#include <stdint.h>
#include <linux/types.h>

// Private SNIC subtype of GIN GDAKI. It requires the matching device headers.
#define PCCL_GIN_SNIC_GDAKI_VERSION 202
struct pcclGinSnicQp {
  uint32_t *sq, *cq;
  uint64_t *sqDb, *cqDb;
  uint32_t qpn, cqn;
  volatile uint32_t producer, consumer;
  uint32_t mutex, maxBytes, depth, notified;
  uint64_t posted, completed;
  uint32_t pending[128];
  unsigned char* scratch;
  uint64_t scratchOffset;
  __be32 scratchKey;
  uint32_t* error;
};

#endif
