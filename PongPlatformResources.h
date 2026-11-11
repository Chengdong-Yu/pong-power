/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PONG_PLATFORM_RESOURCES_H
#define PONG_PLATFORM_RESOURCES_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void pong_rpmh_init(void);
bool pong_scm_call_lock(void);
void pong_scm_call_unlock(void);
/* Local serialization must precede the shared TCSR hardware lock: both
 * APPS threads write the same hardware owner id and cannot exclude each
 * other with that id alone. The caller still acquires the hardware mutex. */
bool pong_smem_local_lock(void);
void pong_smem_local_unlock(void);
bool pong_rpmh_vote(uint32_t address, uint32_t value);
/* QUP0 SE5 is one real I2C engine shared by eUSB2 repeater and Glyph. */
bool pong_qup5_lock(void);
void pong_qup5_unlock(void);
bool pong_smmu_lease_lock(void);
void pong_smmu_lease_unlock(void);
/* Boot-lifetime shared IPCC subscribers. The callback runs in IRQ context:
 * acknowledge/wake only; no allocation, blocking or GLINK/QRTR parsing.
 * Its context must remain alive for the boot. One owner per(client,signal).
 */
typedef void (*PongIpccNotify)(void *context);
int pong_ipcc_subscribe(uint32_t client,uint32_t signal,PongIpccNotify notify,void *context);
bool pong_ipcc_send(uint32_t client,uint32_t signal);
#ifdef __cplusplus
}
#endif
#endif
