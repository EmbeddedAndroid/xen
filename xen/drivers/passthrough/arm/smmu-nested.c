/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Register rules for a hardware domain that drives stage 1 of an Arm
 * SMMUv2 nested on Xen's stage 2 (dom0-iommu=nested).
 *
 * The hardware domain sees an SMMU with stage 1 only, the bottom SMRs and
 * context banks, and its own copies of the registers that configure
 * them. Every context bank it gets is a stage-1 bank nested on Xen's
 * stage-2 bank for the hardware domain, a bypass stream translates
 * through that stage-2 bank, and anything else is a fault.
 */

#include "smmu-nested.h"

#define ID0_S2TS            (1U << 29)
#define ID0_NTS             (1U << 28)
#define ID0_EXIDS           (1U << 8)
#define ID0_NUMSMRG_MASK    0xffU
#define ID1_NUMS2CB_MASK    (0xffU << 16)
#define ID1_NUMCB_MASK      0xffU

#define S2CR_CBNDX_MASK     0xffU
#define S2CR_TYPE_SHIFT     16
#define S2CR_TYPE_MASK      (3U << S2CR_TYPE_SHIFT)
#define S2CR_TYPE_TRANS     0U
#define S2CR_TYPE_BYPASS    1U
#define S2CR_TYPE_FAULT     2U
#define S2CR_EXIDVALID      (1U << 10)
#define S2CR_PRIVCFG_MASK   (3U << 24)
#define S2CR_INSTCFG_MASK   (3U << 26)

#define CBAR_TYPE_NESTED    (3U << 16)
#define CBAR_CBNDX_SHIFT    8
#define CBA2R_VA64          (1U << 0)

bool smmu_nested_split(unsigned int nr_smr, unsigned int nr_cb,
                       unsigned int *hwdom_smr, unsigned int *hwdom_cb)
{
    unsigned int xen_smr = nr_smr / 8 > 2 ? nr_smr / 8 : 2;
    unsigned int xen_cb = nr_cb / 8 > 2 ? nr_cb / 8 : 2;

    if ( nr_smr <= xen_smr || nr_cb <= xen_cb )
        return false;

    *hwdom_smr = nr_smr - xen_smr;
    *hwdom_cb = nr_cb - xen_cb;

    return true;
}

enum smmu_nested_op smmu_nested_classify(unsigned long off,
                                         unsigned int pgshift, bool write,
                                         unsigned int nr_smr,
                                         unsigned int nr_cb,
                                         unsigned int *idx)
{
    unsigned long page = off >> pgshift;
    unsigned long reg = off & ((1UL << pgshift) - 1);

    if ( reg & 3 )
        return SMMU_NESTED_IGNORE;

    if ( page == 0 )
    {
        if ( reg >= SMMU_NESTED_GR0_SMR(0) && reg < SMMU_NESTED_GR0_SMR(nr_smr) )
        {
            *idx = (reg - SMMU_NESTED_GR0_SMR(0)) >> 2;
            return SMMU_NESTED_SMR;
        }
        if ( reg >= SMMU_NESTED_GR0_S2CR(0) &&
             reg < SMMU_NESTED_GR0_S2CR(nr_smr) )
        {
            *idx = (reg - SMMU_NESTED_GR0_S2CR(0)) >> 2;
            return SMMU_NESTED_S2CR;
        }

        switch ( reg )
        {
        case SMMU_NESTED_GR0_sCR0:
            return SMMU_NESTED_SCR0;
        case SMMU_NESTED_GR0_sACR:
            return SMMU_NESTED_SACR;
        case SMMU_NESTED_GR0_ID0:
            return write ? SMMU_NESTED_IGNORE : SMMU_NESTED_ID0;
        case SMMU_NESTED_GR0_ID1:
            return write ? SMMU_NESTED_IGNORE : SMMU_NESTED_ID1;
        /* Invalidating every non-secure, non-hyp entry only costs Xen. */
        case SMMU_NESTED_GR0_TLBIALLNSNH:
        case SMMU_NESTED_GR0_sTLBGSYNC:
        case SMMU_NESTED_GR0_sTLBGSTATUS:
            return SMMU_NESTED_HW;
        }

        /*
         * Reads of the rest of the ID registers and of the global fault
         * syndrome; the global fault status is write-one-to-clear.
         */
        if ( !write )
            return reg < SMMU_NESTED_GR0_STLBIALL || reg >= 0xfd0
                   ? SMMU_NESTED_HW : SMMU_NESTED_IGNORE;
        return reg == SMMU_NESTED_GR0_sGFSR ? SMMU_NESTED_HW
                                             : SMMU_NESTED_IGNORE;
    }

    if ( page == 1 )
    {
        enum smmu_nested_op op;

        if ( reg < SMMU_NESTED_GR1_CBFRSYNRA(0) )
            op = SMMU_NESTED_CBAR;
        else if ( reg < SMMU_NESTED_GR1_CBA2R(0) )
            op = SMMU_NESTED_CBFRSYNRA;
        else if ( reg < SMMU_NESTED_GR1_CBA2R(256) )
            op = SMMU_NESTED_CBA2R;
        else
            return SMMU_NESTED_IGNORE;

        *idx = (reg & 0x3ff) >> 2;
        return *idx < nr_cb ? op : SMMU_NESTED_IGNORE;
    }

    return SMMU_NESTED_IGNORE;
}

bool smmu_nested_smr_overlaps(uint32_t a, uint32_t b, uint32_t sid_mask)
{
    uint32_t ignore = ((a | b) >> 16) & 0x7fffU;

    return !((a ^ b) & 0xffffU & ~ignore & sid_mask);
}

uint32_t smmu_nested_id0(uint32_t hw, unsigned int nr_smr)
{
    hw &= ~(ID0_S2TS | ID0_NTS | ID0_EXIDS | ID0_NUMSMRG_MASK);

    return hw | (nr_smr & ID0_NUMSMRG_MASK);
}

uint32_t smmu_nested_id1(uint32_t hw, unsigned int nr_cb)
{
    hw &= ~(ID1_NUMS2CB_MASK | ID1_NUMCB_MASK);

    return hw | (nr_cb & ID1_NUMCB_MASK);
}

uint32_t smmu_nested_s2cr(uint32_t val, unsigned int nr_cb,
                          unsigned int s2_cb)
{
    uint32_t keep = val & (S2CR_PRIVCFG_MASK | S2CR_INSTCFG_MASK);

    switch ( (val & S2CR_TYPE_MASK) >> S2CR_TYPE_SHIFT )
    {
    case S2CR_TYPE_TRANS:
        if ( (val & S2CR_CBNDX_MASK) < nr_cb )
            return val & ~S2CR_EXIDVALID;
        break;
    case S2CR_TYPE_BYPASS:
        return keep | (S2CR_TYPE_TRANS << S2CR_TYPE_SHIFT) |
               (s2_cb & S2CR_CBNDX_MASK);
    }

    return SMMU_NESTED_S2CR_FAULT;
}

uint32_t smmu_nested_cbar(unsigned int s2_cb)
{
    return CBAR_TYPE_NESTED | ((s2_cb & 0xffU) << CBAR_CBNDX_SHIFT);
}

uint32_t smmu_nested_cba2r(uint32_t val)
{
    return val & CBA2R_VA64;
}
