/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Slices of an Arm SMMUv2 that the secure firmware programs itself.
 */
#ifndef __ARM_SMMU_FW_H__
#define __ARM_SMMU_FW_H__

#include <xen/types.h>

#define SMMU_FW_MAX_STREAMS     16

struct smmu_fw_slice {
    paddr_t base;
    unsigned int smr_first, smr_count;
    unsigned int cb_first, cb_count;
    unsigned int vmid_first, vmid_count;
    unsigned int nr_streams;
    /* SMR values: mask << 16 | stream ID */
    uint32_t streams[SMMU_FW_MAX_STREAMS];
};

int smmu_fw_slice_lookup(paddr_t base, struct smmu_fw_slice *s);
int smmu_fw_slice_check(const struct smmu_fw_slice *s, unsigned int nr_smr,
                        unsigned int nr_cb, uint32_t sid_mask);
bool smmu_fw_slice_owns(const struct smmu_fw_slice *s, uint32_t fwid,
                        uint32_t sid_mask);

#endif /* __ARM_SMMU_FW_H__ */
