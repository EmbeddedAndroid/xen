/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xen/arch/arm/platforms/qcom.c
 *
 * Qualcomm SoC specific code
 *
 * Copyright (c) 2024 EPAM Systems.
 */

#include <xen/sched.h>
#include <asm/platform.h>
#include <asm/smccc.h>

/* Secure Channel Manager (SCM) "INFO" service, as used by Linux qcom_scm */
#define QCOM_SCM_FNID(svc, cmd)     ((((svc) & 0xff) << 8) | ((cmd) & 0xff))
#define QCOM_SCM_SVC_INFO               0x06
#define QCOM_SCM_INFO_IS_CALL_AVAIL     0x01

#define QCOM_SCM_IS_CALL_AVAIL_FNID \
    QCOM_SCM_FNID(QCOM_SCM_SVC_INFO, QCOM_SCM_INFO_IS_CALL_AVAIL)

/* Without the call type bit (fast or yielding). */
#define QCOM_SCM_IS_CALL_AVAIL(conv) \
    ARM_SMCCC_CALL_VAL(0, conv, ARM_SMCCC_OWNER_SIP, \
                       QCOM_SCM_IS_CALL_AVAIL_FNID)

/* The queried function ID carries neither call type nor convention. */
#define QCOM_SCM_IS_CALL_AVAIL_QUERY \
    ((ARM_SMCCC_OWNER_SIP << ARM_SMCCC_OWNER_SHIFT) | \
     QCOM_SCM_IS_CALL_AVAIL_FNID)

static const char * const qcom_dt_compat[] __initconst =
{
    "qcom,nord",
    "qcom,sa8155p",
    NULL
};

/*
 * Linux qcom_scm probes the SCM calling convention with an SMC64, then an
 * SMC32, "is call available" query on itself. If both fail it assumes the
 * pre-SMCCC "legacy" convention and issues SMCs with function ID 1, which
 * Xen handles as the PSCI 0.1 CPU_OFF of the calling vCPU.
 *
 * Answer the query for the hardware domain: the INFO call itself is
 * available, every other SCM call is not, as Xen forwards none of them.
 */
static bool qcom_smc(struct cpu_user_regs *regs)
{
    uint32_t fid = get_user_reg(regs, 0);

    if ( !is_hardware_domain(current->domain) )
        return false;

    /* Linux issues the query as fast and as yielding call. */
    fid &= ~(ARM_SMCCC_FAST_CALL << ARM_SMCCC_TYPE_SHIFT);
    if ( fid != QCOM_SCM_IS_CALL_AVAIL(ARM_SMCCC_CONV_64) &&
         fid != QCOM_SCM_IS_CALL_AVAIL(ARM_SMCCC_CONV_32) )
        return false;

    /* x1 carries the argument layout, x2 the queried function ID. */
    set_user_reg(regs, 0, ARM_SMCCC_SUCCESS);
    set_user_reg(regs, 1, (uint32_t)get_user_reg(regs, 2) ==
                          QCOM_SCM_IS_CALL_AVAIL_QUERY);

    return true;
}

PLATFORM_START(qcom, "Qualcomm")
    .compatible = qcom_dt_compat,
    .smc = qcom_smc,
PLATFORM_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
