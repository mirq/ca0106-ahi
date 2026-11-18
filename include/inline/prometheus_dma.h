/*
 * Minimal prometheus.library (v2+) inline stubs: DMA buffer API only.
 * On this system Prometheus.card serves these from a reserved tail of the
 * Radeon VRAM (DMASIZE tooltype).  Buffers are 4096-byte page aligned and
 * must be freed with the original pointer and the exact requested size.
 */
#ifndef _INLINE_PROMETHEUS_DMA_H
#define _INLINE_PROMETHEUS_DMA_H

#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif

#ifndef PROMETHEUS_BASE_NAME
#define PROMETHEUS_BASE_NAME PrometheusBase
#endif

#define Prm_AllocDMABuffer(size) \
	LP1(0x60, APTR, Prm_AllocDMABuffer, ULONG, size, d0, , PROMETHEUS_BASE_NAME)

#define Prm_FreeDMABuffer(block, size) \
	LP2NR(0x66, Prm_FreeDMABuffer, APTR, block, a0, ULONG, size, d0, , PROMETHEUS_BASE_NAME)

#define Prm_GetPhysicalAddress(addr) \
	LP1(0x6c, APTR, Prm_GetPhysicalAddress, APTR, addr, d0, , PROMETHEUS_BASE_NAME)

#endif
