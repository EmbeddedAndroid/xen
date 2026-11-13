#ifndef __ASM_DOMAIN_BUILD_H__
#define __ASM_DOMAIN_BUILD_H__

#include <xen/fdt-kernel.h>
#include <xen/sched.h>

/* An interrupt specifier of the GIC binding: three cells, or four */
#define GIC_INTERRUPT_MAX_CELLS 4
typedef __be32 gic_interrupt_t[GIC_INTERRUPT_MAX_CELLS];
int make_psci_node(void *fdt);
void evtchn_allocate(struct domain *d);

/*
 * Helper to write an interrupts with the GIC format
 * This code is assuming the irq is an PPI.
 */
void set_interrupt(gic_interrupt_t interrupt, unsigned int irq,
                   unsigned int cpumask, unsigned int level);

int fdt_property_interrupts(const struct kernel_info *kinfo,
                            gic_interrupt_t *intr, unsigned int num_irq);

int prepare_acpi(struct domain *d, struct kernel_info *kinfo);

int add_ext_regions(unsigned long s_gfn, unsigned long e_gfn, void *data);

#define ACPI_DOM0_FDT_MIN_SIZE 4096

paddr_t hwdom_get_fdt_alloc_size(void);

/*
 * The command line of the hardware domain booted from a device tree, from
 * "xen,dom0-bootargs" in /chosen, the kernel module, or "bootargs" in
 * /chosen, in that order; NULL without one.
 */
const char *hwdom_bootargs(const char *module_cmdline);

#if defined(CONFIG_MPU) && defined(CONFIG_ARM_64)
/* Utility function to determine if an Armv8-R processor supports VMSA. */
bool has_v8r_vmsa_support(void);
#else
static inline bool has_v8r_vmsa_support(void)
{
    return false;
}
#endif /* CONFIG_MPU */

#endif

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
