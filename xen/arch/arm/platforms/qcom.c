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
#include <xen/init.h>
#include <xen/mm.h>
#include <xen/sched.h>
#include <xen/spinlock.h>
#include <asm/guest_access.h>
#include <asm/irq.h>
#include <asm/p2m.h>
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

/* Memory protection service: hand memory to other VMs (e.g. a DSP) */
#define QCOM_SCM_SVC_MP                 0x0c
#define QCOM_SCM_MP_ASSIGN              0x16
#define QCOM_SCM_MP_ASSIGN_FNID \
    QCOM_SCM_FNID(QCOM_SCM_SVC_MP, QCOM_SCM_MP_ASSIGN)
#define QCOM_SCM_MP_ASSIGN_SMC64 \
    ARM_SMCCC_CALL_VAL(0, ARM_SMCCC_CONV_64, ARM_SMCCC_OWNER_SIP, \
                       QCOM_SCM_MP_ASSIGN_FNID)
#define QCOM_SCM_MP_ASSIGN_QUERY \
    ((ARM_SMCCC_OWNER_SIP << ARM_SMCCC_OWNER_SHIFT) | QCOM_SCM_MP_ASSIGN_FNID)
/* Seven arguments: RO, VAL, RO, VAL, RO, VAL, VAL */
#define QCOM_SCM_MP_ASSIGN_ARGINFO      0x1117
#define QCOM_SCM_EINVAL_ADDR            ((register_t)-3)

/*
 * Bounds of the parameter lists of an assign call: memory regions of 16
 * bytes, source VMIDs of 4 bytes, destination permissions of 24 bytes.
 */
#define QCOM_SCM_ASSIGN_MAX_MEM         (64 * 16)
#define QCOM_SCM_ASSIGN_MAX_SRC         (32 * 4)
#define QCOM_SCM_ASSIGN_MAX_DST         (32 * 24)

struct qcom_scm_assign_buf {
    uint64_t args[4];                   /* indirect arguments 3..6 */
    uint8_t mem[QCOM_SCM_ASSIGN_MAX_MEM];
    uint8_t src[QCOM_SCM_ASSIGN_MAX_SRC];
    uint8_t dst[QCOM_SCM_ASSIGN_MAX_DST];
};

static struct qcom_scm_assign_buf *qcom_assign_buf;
static DEFINE_SPINLOCK(qcom_assign_lock);

static const char * const qcom_dt_compat[] __initconst =
{
    "qcom,nord",
    "qcom,sa8775p",
    "qcom,sa8155p",
    NULL
};

/*
 * Is all of [addr, addr + size) mapped in the domain, as its own RAM or as
 * a 1:1 cacheable mapping Xen gave it (reserved-memory regions and device
 * memory of the hardware domain)? Xen's memory, other domains' memory and
 * foreign or grant mappings never are.
 */
static bool qcom_domain_owns(struct domain *d, paddr_t addr, paddr_t size)
{
    paddr_t end;
    gfn_t gfn;

    if ( !size || addr + size < addr )
        return false;

    end = addr + size - 1;
    for ( gfn = gaddr_to_gfn(addr); gfn_x(gfn) <= paddr_to_pfn(end);
          gfn = gfn_add(gfn, 1) )
    {
        p2m_type_t t;
        mfn_t mfn = p2m_lookup(d, gfn, &t);

        if ( mfn_eq(mfn, INVALID_MFN) ||
             (t != p2m_ram_rw && t != p2m_mmio_direct_c) )
            return false;
    }

    return true;
}

/*
 * Forward the hardware domain's memory assign call (qcom_scm_assign_mem())
 * to the firmware. The firmware reads three lists from memory, by physical
 * address: the regions to assign, the source VMIDs and the destination
 * permissions, plus the indirect arguments 3..6 of the call. Copy them into
 * a Xen page so that the domain cannot change them after they are checked,
 * check that every region is memory the domain owns, and hand the firmware
 * the copies.
 */
static bool qcom_scm_mem_assign(struct cpu_user_regs *regs)
{
    struct domain *d = current->domain;
    struct qcom_scm_assign_buf *b = qcom_assign_buf;
    paddr_t mem = get_user_reg(regs, 2), mem_sz = get_user_reg(regs, 3);
    paddr_t src = get_user_reg(regs, 4), ind = get_user_reg(regs, 5);
    paddr_t src_sz, dst, dst_sz;
    struct arm_smccc_res res;
    unsigned int i;
    bool ok = false;

    if ( !b || get_user_reg(regs, 1) != QCOM_SCM_MP_ASSIGN_ARGINFO )
        goto refuse;

    spin_lock(&qcom_assign_lock);

    if ( access_guest_memory_by_gpa(d, ind, b->args, sizeof(b->args), false) )
        goto out;

    src_sz = b->args[0];
    dst = b->args[1];
    dst_sz = b->args[2];
    if ( !mem_sz || mem_sz > sizeof(b->mem) || mem_sz % 16 ||
         !src_sz || src_sz > sizeof(b->src) || src_sz % 4 ||
         !dst_sz || dst_sz > sizeof(b->dst) || dst_sz % 24 )
        goto out;

    if ( access_guest_memory_by_gpa(d, mem, b->mem, mem_sz, false) ||
         access_guest_memory_by_gpa(d, src, b->src, src_sz, false) ||
         access_guest_memory_by_gpa(d, dst, b->dst, dst_sz, false) )
        goto out;

    for ( i = 0; i < mem_sz; i += 16 )
    {
        uint64_t r[2];

        memcpy(r, b->mem + i, sizeof(r));
        if ( !qcom_domain_owns(d, r[0], r[1]) )
        {
            gprintk(XENLOG_WARNING,
                    "SCM assign of %#"PRIx64"+%#"PRIx64" refused: not owned\n",
                    r[0], r[1]);
            goto out;
        }
    }

    b->args[1] = virt_to_maddr(b->dst);
    clean_dcache_va_range(b, sizeof(*b));

    res = arm_smccc_1_1_smc(get_user_reg(regs, 0), QCOM_SCM_MP_ASSIGN_ARGINFO,
                            virt_to_maddr(b->mem), mem_sz,
                            virt_to_maddr(b->src), virt_to_maddr(b->args));
    set_user_reg(regs, 0, res.a0);
    set_user_reg(regs, 1, res.a1);
    set_user_reg(regs, 2, res.a2);
    set_user_reg(regs, 3, res.a3);
    ok = true;

 out:
    spin_unlock(&qcom_assign_lock);
    if ( ok )
        return true;
 refuse:
    set_user_reg(regs, 0, QCOM_SCM_EINVAL_ADDR);
    return true;
}

/*
 * Linux qcom_scm probes the SCM calling convention with an SMC64, then an
 * SMC32, "is call available" query on itself. If both fail it assumes the
 * pre-SMCCC "legacy" convention and issues SMCs with function ID 1, which
 * Xen handles as the PSCI 0.1 CPU_OFF of the calling vCPU.
 *
 * Handle the hardware domain's SCM calls: the INFO query, and the memory
 * assign call, which is forwarded (see qcom_scm_mem_assign()); every other
 * SCM call is refused. A query reports INFO itself as available, assign as
 * the firmware reports it, and every other call as not available.
 */
static bool qcom_smc(struct cpu_user_regs *regs)
{
    uint32_t fid = get_user_reg(regs, 0);
    uint32_t query;

    if ( !is_hardware_domain(current->domain) )
        return false;

    /* Linux issues the calls as fast and as yielding calls. */
    fid &= ~(ARM_SMCCC_FAST_CALL << ARM_SMCCC_TYPE_SHIFT);
    if ( fid == QCOM_SCM_MP_ASSIGN_SMC64 )
        return qcom_scm_mem_assign(regs);

    if ( fid != QCOM_SCM_IS_CALL_AVAIL(ARM_SMCCC_CONV_64) &&
         fid != QCOM_SCM_IS_CALL_AVAIL(ARM_SMCCC_CONV_32) )
        return false;

    /* x1 carries the argument layout, x2 the queried function ID. */
    query = get_user_reg(regs, 2);
    if ( query == QCOM_SCM_MP_ASSIGN_QUERY )
    {
        struct arm_smccc_res res;

        res = arm_smccc_1_1_smc(get_user_reg(regs, 0), get_user_reg(regs, 1),
                                query);
        set_user_reg(regs, 0, res.a0);
        set_user_reg(regs, 1, qcom_assign_buf ? res.a1 : 0);
        return true;
    }

    set_user_reg(regs, 0, ARM_SMCCC_SUCCESS);
    set_user_reg(regs, 1, query == QCOM_SCM_IS_CALL_AVAIL_QUERY);

    return true;
}

static int __init qcom_init(void)
{
    BUILD_BUG_ON(sizeof(*qcom_assign_buf) > PAGE_SIZE);

    qcom_assign_buf = alloc_xenheap_page();
    if ( !qcom_assign_buf )
        printk(XENLOG_WARNING "qcom: no page for SCM assign calls\n");

    return 0;
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

/*
 * GPIO wakeup interrupts reach the PDC through the TLMM, which the hardware
 * domain maps to PDC pins at run time from a per-SoC table, so the device
 * tree names neither the pin nor its SPI. Route every other SPI the PDC
 * drives to the hardware domain without a trigger: the hardware domain
 * programs it when it enables the interrupt (irq_type_set_by_domain()).
 * SPIs a node names at the GIC are left to the generic code, and SPIs Xen
 * uses are skipped.
 */
static void __init qcom_route_pdc_spis(struct domain *d,
                                       const struct dt_device_node *pdc,
                                       unsigned long *routed)
{
    const struct dt_device_node *np;
    const __be32 *cells;
    unsigned int i, nr = 0;
    u32 len;

    dt_for_each_device_node ( dt_host, np )
    {
        unsigned int j, nirq = dt_number_of_irq(np);

        for ( j = 0; j < nirq; j++ )
        {
            struct dt_raw_irq rirq;
            struct dt_irq dirq;

            if ( !dt_device_get_raw_irq(np, j, &rirq) &&
                 !dt_irq_translate(&rirq, &dirq) && dirq.irq < NR_IRQS )
                __set_bit(dirq.irq, routed);
        }
    }

    cells = dt_get_property(pdc, "qcom,pdc-ranges", &len);
    if ( !cells || len % (3 * sizeof(*cells)) )
        return;

    for ( i = 0; i < len / sizeof(*cells); i += 3 )
    {
        uint32_t spi = be32_to_cpu(cells[i + 1]);
        uint32_t count = be32_to_cpu(cells[i + 2]);
        uint32_t n;

        for ( n = 0; n < count; n++ )
        {
            unsigned int irq = NR_LOCAL_IRQS + spi + n;
            int rc;

            if ( irq >= NR_IRQS )
                break;
            if ( test_and_set_bit(irq, routed) || irq_to_desc(irq)->action )
                continue;

            rc = map_irq_to_domain(d, irq, true, "pdc");
            if ( rc )
                printk(XENLOG_WARNING "qcom: unable to route PDC SPI %u to %pd: %d\n",
                       irq, d, rc);
            else
                nr++;
        }
    }

    printk(XENLOG_INFO "qcom: routed %u untyped PDC SPIs to %pd\n", nr, d);
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

    qcom_route_pdc_spis(d, pdc, routed);

    return 0;
}

PLATFORM_START(qcom, "Qualcomm")
    .compatible = qcom_dt_compat,
    .init = qcom_init,
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
