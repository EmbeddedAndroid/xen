/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ARM_DRTM_H
#define ARM_DRTM_H

#ifdef CONFIG_ARM_DRTM
void drtm_measure_dom0(void);
#else
static inline void drtm_measure_dom0(void) {}
#endif

#endif /* ARM_DRTM_H */
