/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Slices of an Arm SMMUv2 that the secure firmware programs itself.
 *
 * An SMMUv2 without the Security Extensions (IDR0.SES = 0, as Qualcomm's
 * MMU-500s) has no secure stream mapping or context banks. OP-TEE then
 * programs the stage 2 of the co-processor firmware streams in the top
 * SMRs and context banks of the non-secure interface, and publishes that
 * slice with the OP-TEE FF-A call OPTEE_FFA_FW_SMMU_INFO. Xen keeps out of
 * the slice and never maps one of its streams for a domain.
 */

#include <xen/errno.h>
#include <xen/init.h>
#include <xen/types.h>

#include <asm/smccc.h>
#include <asm/tee/ffa.h>

#include "smmu-fw.h"

/* OP-TEE blocking call OPTEE_FFA_FW_SMMU_INFO, layout 1 */
#define OPTEE_FFA_FW_SMMU_INFO          9
#define OPTEE_FFA_FW_SMMU_INFO_V1       1
#define TEE_SUCCESS                     0x00000000U
#define TEE_ERROR_ITEM_NOT_FOUND        0xffff0008U

#define SMMU_FW_MAX_INSTANCES           8

#define SMR_ID(v)                       ((v) & 0xffffU)
#define SMR_MASK(v)                     ((v) >> 16)

static int __init fw_smmu_info(unsigned int inst, unsigned int entry,
                               uint32_t out[4])
{
    struct arm_smccc_1_2_regs req = {
        .a3 = OPTEE_FFA_FW_SMMU_INFO,
        .a4 = OPTEE_FFA_FW_SMMU_INFO_V1,
        .a5 = inst,
        .a6 = entry,
    };
    struct arm_smccc_1_2_regs resp;
    int ret;

    ret = ffa_spmc_direct_req(&req, &resp);
    if ( ret )
        return ret;

    switch ( (uint32_t)resp.a3 )
    {
    case TEE_SUCCESS:
        break;
    case TEE_ERROR_ITEM_NOT_FOUND:
        return -ENOENT;
    default:
        return -EOPNOTSUPP;
    }

    out[0] = resp.a4;
    out[1] = resp.a5;
    out[2] = resp.a6;
    out[3] = resp.a7;

    return 0;
}

static int __init fw_smmu_read(unsigned int inst, struct smmu_fw_slice *s)
{
    uint32_t out[4];
    unsigned int i, nr_streams;
    int ret;

    ret = fw_smmu_info(inst, 1, out);
    if ( ret )
        return ret == -ENOENT ? -EINVAL : ret;

    s->vmid_first = out[0] & 0xffffU;
    s->vmid_count = out[0] >> 16;
    nr_streams = out[1];
    if ( !nr_streams || nr_streams > SMMU_FW_MAX_STREAMS )
        return -EINVAL;

    for ( i = 0; i < nr_streams; i++ )
    {
        ret = fw_smmu_info(inst, 2 + i, out);
        if ( ret )
            return ret == -ENOENT ? -EINVAL : ret;
        if ( out[1] < s->cb_first || out[1] - s->cb_first >= s->cb_count )
            return -EINVAL;
        s->streams[i] = out[0];
    }
    s->nr_streams = nr_streams;

    return 0;
}

/*
 * Find the slice the secure firmware declares on the SMMU at @base.
 * Returns 0 and fills @s; -ENOENT if the firmware answers but declares
 * nothing there; -EOPNOTSUPP if there is no FF-A or the firmware does not
 * publish slices at all; or another error for an answer Xen cannot trust
 * (the caller must not run that SMMU as if it had no slice).
 * OP-TEE answers "not found" both past its last instance and for an
 * instance it failed to set up, so every instance is asked.
 */
int __init smmu_fw_slice_lookup(paddr_t base, struct smmu_fw_slice *s)
{
    unsigned int inst;
    bool answered = false;
    uint32_t out[4];
    int ret;

    for ( inst = 0; inst < SMMU_FW_MAX_INSTANCES; inst++ )
    {
        ret = fw_smmu_info(inst, 0, out);
        if ( ret == -ENOENT )
        {
            answered = true;
            continue;
        }
        if ( ret == -EOPNOTSUPP && !answered && !inst )
            return -EOPNOTSUPP;
        if ( ret )
            return ret == -EOPNOTSUPP ? -EIO : ret;

        answered = true;
        if ( (out[0] | ((paddr_t)out[1] << 32)) != base )
            continue;

        s->base = base;
        s->smr_first = out[2] & 0xffffU;
        s->smr_count = out[2] >> 16;
        s->cb_first = out[3] & 0xffffU;
        s->cb_count = out[3] >> 16;

        return fw_smmu_read(inst, s);
    }

    return -ENOENT;
}

static bool smr_overlaps(uint32_t a, uint32_t b, uint32_t sid_mask)
{
    uint32_t ignore = SMR_MASK(a) | SMR_MASK(b);

    return !((SMR_ID(a) ^ SMR_ID(b)) & ~ignore & sid_mask);
}

/*
 * Xen can fence a slice only if it is the top of the SMRs and context
 * banks, and Xen's own VMIDs (context bank index + 1, so at most
 * cb_first) stay below the firmware's 8-bit VMIDs.
 */
int smmu_fw_slice_check(const struct smmu_fw_slice *s, unsigned int nr_smr,
                        unsigned int nr_cb, uint32_t sid_mask)
{
    unsigned int i;

    if ( !s->smr_first || s->smr_first + s->smr_count != nr_smr ||
         !s->cb_first || s->cb_first + s->cb_count != nr_cb ||
         !s->smr_count || !s->cb_count || !s->vmid_count ||
         s->vmid_first <= s->cb_first ||
         s->vmid_first + s->vmid_count > 256 )
        return -EINVAL;

    if ( !s->nr_streams || s->nr_streams > SMMU_FW_MAX_STREAMS ||
         s->nr_streams > s->smr_count )
        return -EINVAL;

    for ( i = 0; i < s->nr_streams; i++ )
        if ( (SMR_ID(s->streams[i]) | SMR_MASK(s->streams[i])) & ~sid_mask )
            return -EINVAL;

    return 0;
}

/* Whether stream @fwid (mask << 16 | ID) can match a firmware stream */
bool smmu_fw_slice_owns(const struct smmu_fw_slice *s, uint32_t fwid,
                        uint32_t sid_mask)
{
    unsigned int i;

    for ( i = 0; i < s->nr_streams; i++ )
        if ( smr_overlaps(fwid, s->streams[i], sid_mask) )
            return true;

    return false;
}
