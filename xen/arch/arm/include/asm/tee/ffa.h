/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xen/arch/arm/include/asm/tee/ffa.h
 *
 * Arm Firmware Framework for ARMv8-A(FFA) mediator
 *
 * Copyright (C) 2023  Linaro Limited
 */

#ifndef __ASM_ARM_TEE_FFA_H__
#define __ASM_ARM_TEE_FFA_H__

#include <xen/const.h>

#include <asm/smccc.h>
#include <asm/types.h>

/* FF-A Function ID range (AArch32 function number field) as of FF-A v1.2 */
#define FFA_FNUM_MIN_VALUE              _AC(0x60,U)
#define FFA_FNUM_MAX_VALUE              _AC(0x8E,U)

/* Error codes */
#define FFA_RET_OK                      0
#define FFA_RET_NOT_SUPPORTED           -1
#define FFA_RET_INVALID_PARAMETERS      -2
#define FFA_RET_NO_MEMORY               -3
#define FFA_RET_BUSY                    -4
#define FFA_RET_INTERRUPTED             -5
#define FFA_RET_DENIED                  -6
#define FFA_RET_RETRY                   -7
#define FFA_RET_ABORTED                 -8
#define FFA_RET_NO_DATA                 -9

/* Function IDs also used outside the mediator */
#define FFA_ERROR                       0x84000060U
#define FFA_MSG_SEND_DIRECT_REQ_32      0x8400006FU
#define FFA_MSG_SEND_DIRECT_REQ_64      0xC400006FU
#define FFA_MSG_SEND_DIRECT_RESP_32     0x84000070U
#define FFA_MSG_SEND_DIRECT_RESP_64     0xC4000070U

static inline bool is_ffa_fid(uint32_t fid)
{
    uint32_t fn = fid & ARM_SMCCC_FUNC_MASK;

    return fn >= FFA_FNUM_MIN_VALUE && fn <= FFA_FNUM_MAX_VALUE;
}

#ifdef CONFIG_FFA
#define FFA_NR_FUNCS    12
#else
#define FFA_NR_FUNCS    0
#endif

#endif /*__ASM_ARM_TEE_FFA_H__*/
