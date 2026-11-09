/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Register rules for a hardware domain that drives stage 1 of an Arm
 * SMMUv2 nested on Xen's stage 2 (dom0-iommu=nested).
 */
#ifndef __ARM_SMMU_NESTED_H__
#define __ARM_SMMU_NESTED_H__

#include <xen/types.h>

/* Global register space, page 0 (GR0) */
#define SMMU_NESTED_GR0_sCR0        0x000
#define SMMU_NESTED_GR0_sACR        0x010
#define SMMU_NESTED_GR0_ID0         0x020
#define SMMU_NESTED_GR0_ID1         0x024
#define SMMU_NESTED_GR0_sGFSR       0x048
#define SMMU_NESTED_GR0_STLBIALL    0x060
#define SMMU_NESTED_GR0_TLBIALLNSNH 0x068
#define SMMU_NESTED_GR0_sTLBGSYNC   0x070
#define SMMU_NESTED_GR0_sTLBGSTATUS 0x074
#define SMMU_NESTED_GR0_SMR(n)      (0x800 + ((n) << 2))
#define SMMU_NESTED_GR0_S2CR(n)     (0xc00 + ((n) << 2))

/* Global register space, page 1 (GR1) */
#define SMMU_NESTED_GR1_CBAR(n)     (0x000 + ((n) << 2))
#define SMMU_NESTED_GR1_CBFRSYNRA(n) (0x400 + ((n) << 2))
#define SMMU_NESTED_GR1_CBA2R(n)    (0x800 + ((n) << 2))

#define SMMU_NESTED_SMR_VALID       (1U << 31)
#define SMMU_NESTED_S2CR_FAULT      (2U << 16)

/* What to do with a hardware domain access to the global space */
enum smmu_nested_op {
    SMMU_NESTED_IGNORE,     /* reads as zero, writes ignored */
    SMMU_NESTED_HW,         /* goes to the register as is */
    SMMU_NESTED_SCR0,       /* the hwdom's own copy of sCR0 */
    SMMU_NESTED_SACR,       /* the hwdom's own copy of sACR */
    SMMU_NESTED_ID0,        /* ID0/ID1 describe the hwdom's slice only */
    SMMU_NESTED_ID1,
    SMMU_NESTED_SMR,        /* idx = SMR index */
    SMMU_NESTED_S2CR,       /* idx = S2CR index */
    SMMU_NESTED_CBAR,       /* idx = context bank */
    SMMU_NESTED_CBFRSYNRA,
    SMMU_NESTED_CBA2R,
};

/*
 * Split the SMRs and context banks Xen may use: the hardware domain gets
 * the bottom of each, Xen keeps the rest (its stage-2 context banks and
 * the streams of other domains). Returns false when either is too small.
 */
bool smmu_nested_split(unsigned int nr_smr, unsigned int nr_cb,
                       unsigned int *hwdom_smr, unsigned int *hwdom_cb);

/*
 * Classify an access at @off of the global space with page size
 * 1 << @pgshift; @idx gets the register index.
 */
enum smmu_nested_op smmu_nested_classify(unsigned long off,
                                         unsigned int pgshift, bool write,
                                         unsigned int nr_smr,
                                         unsigned int nr_cb,
                                         unsigned int *idx);

uint32_t smmu_nested_id0(uint32_t hw, unsigned int nr_smr);
uint32_t smmu_nested_id1(uint32_t hw, unsigned int nr_cb);

/* Whether SMR values @a and @b (VALID ignored) can match the same stream */
bool smmu_nested_smr_overlaps(uint32_t a, uint32_t b, uint32_t sid_mask);

/* Hardware values for what the hardware domain writes */
uint32_t smmu_nested_s2cr(uint32_t val, unsigned int nr_cb,
                          unsigned int s2_cb);
uint32_t smmu_nested_cbar(unsigned int s2_cb);
uint32_t smmu_nested_cba2r(uint32_t val);

#endif /* __ARM_SMMU_NESTED_H__ */
