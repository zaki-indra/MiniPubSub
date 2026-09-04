#ifndef MINIPUBSUB_VERSION_H
#define MINIPUBSUB_VERSION_H

#include <stdint.h>
#include "minipubsub/types.h"

#define MPS_VERSION_MAJOR 1
#define MPS_VERSION_MINOR 0
#define MPS_VERSION_PATCH 0
#define MPS_ABI_VERSION 1
#define MPS_PROTOCOL_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif
MPS_API uint32_t MPS_CALL mps_abi_version(void);
MPS_API uint32_t MPS_CALL mps_protocol_version(void);
MPS_API const char* MPS_CALL mps_version_string(void);
#ifdef __cplusplus
}
#endif
#endif