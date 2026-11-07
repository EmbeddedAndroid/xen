/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xen/arch/arm/platforms/qcom.c
 *
 * Qualcomm SoC specific code
 *
 * Copyright (c) 2024 EPAM Systems.
 */

#include <xen/bitmap.h>
#include <xen/device_tree.h>
#include <xen/sched.h>
#include <asm/irq.h>
#include <asm/platform.h>
#include <asm/setup.h>
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

/*
 * The Power Domain Controller ("qcom,pdc") sits between wakeup capable
 * interrupt lines and the GIC: a device interrupt described as PDC pin N
 * reaches the GIC as the SPI that "qcom,pdc-ranges" maps N to. The PDC
 * has no interrupt of its own, so the generic code never routes those
 * SPIs to the hardware domain, which drives the PDC. Route them, with the
 * trigger the PDC presents to the GIC: it turns falling edges into rising
 * ones, both edges into rising and low levels into high ones (as Linux
 * irq-qcom-pdc.c does).
 */
static unsigned int qcom_pdc_gic_type(uint32_t flags)
{
    switch ( flags & DT_IRQ_TYPE_SENSE_MASK )
    {
    case DT_IRQ_TYPE_EDGE_RISING:
    case DT_IRQ_TYPE_EDGE_FALLING:
    case DT_IRQ_TYPE_EDGE_BOTH:
        return DT_IRQ_TYPE_EDGE_RISING;
    default:
        return DT_IRQ_TYPE_LEVEL_HIGH;
    }
}

static int qcom_pdc_pin_to_spi(const struct dt_device_node *pdc,
                               uint32_t pin)
{
    const __be32 *cells;
    u32 len;
    unsigned int i;

    cells = dt_get_property(pdc, "qcom,pdc-ranges", &len);
    if ( !cells || len % (3 * sizeof(*cells)) )
        return -EINVAL;

    for ( i = 0; i < len / sizeof(*cells); i += 3 )
    {
        uint32_t base = be32_to_cpu(cells[i]);
        uint32_t spi = be32_to_cpu(cells[i + 1]);
        uint32_t count = be32_to_cpu(cells[i + 2]);

        if ( pin >= base && pin - base < count )
            return NR_LOCAL_IRQS + spi + (pin - base);
    }

    return -ENOENT;
}

static int __init qcom_route_pdc_irqs(struct domain *d)
{
    const struct dt_device_node *pdc;
    struct dt_device_node *np;
    DECLARE_BITMAP(routed, NR_IRQS) = { 0 };

    pdc = dt_find_compatible_node(NULL, NULL, "qcom,pdc");
    if ( !pdc )
        return 0;

    dt_for_each_device_node ( dt_host, np )
    {
        unsigned int i, nirq;

        if ( !dt_device_is_available(np) ||
             dt_device_used_by(np) == DOMID_XEN )
            continue;

        nirq = dt_number_of_irq(np);
        for ( i = 0; i < nirq; i++ )
        {
            struct dt_raw_irq rirq;
            unsigned int type;
            int irq, rc;

            if ( dt_device_get_raw_irq(np, i, &rirq) ||
                 rirq.controller != pdc || rirq.size != 2 )
                continue;

            irq = qcom_pdc_pin_to_spi(pdc, rirq.specifier[0]);
            if ( irq < 0 || irq >= NR_IRQS )
            {
                printk(XENLOG_WARNING "%s: PDC pin %u has no SPI\n",
                       dt_node_full_name(np), rirq.specifier[0]);
                continue;
            }

            type = qcom_pdc_gic_type(rirq.specifier[1]);
            if ( test_and_set_bit(irq, routed) )
            {
                if ( irq_to_desc(irq)->arch.type != type )
                    printk(XENLOG_WARNING
                           "%s: PDC SPI %d already routed with another trigger\n",
                           dt_node_full_name(np), irq);
                continue;
            }

            /* A failure costs this interrupt only, not the domain. */
            rc = irq_set_spi_type(irq, type);
            if ( !rc )
                rc = map_irq_to_domain(d, irq, true, dt_node_name(np));
            if ( rc )
                printk(XENLOG_WARNING "%s: unable to route PDC SPI %d to %pd: %d\n",
                       dt_node_full_name(np), irq, d, rc);
        }
    }

    return 0;
}

PLATFORM_START(qcom, "Qualcomm")
    .compatible = qcom_dt_compat,
    .specific_mapping = qcom_route_pdc_irqs,
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
