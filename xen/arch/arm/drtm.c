/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Arm DRTM (DEN0113) dynamic launch with Xen as the Dynamically Launched
 * Measured Environment (DLME).
 *
 * The dynamic launch measured Xen, its device tree and the boot modules
 * into PCRs 17 and 18, and gave the DLME TPM locality 2. As tboot does on
 * x86, Xen records dom0's kernel, initrd and command line in PCR 19 at
 * locality 2, then gives the locality up with DRTM_CLOSE_LOCALITY before
 * any domain runs. Other domains are not recorded in PCR 19.
 *
 * The TPM belongs to the secure world. Its D-RTM endpoint, an OP-TEE FF-A
 * logical partition whose ABI is core/arch/arm/include/tpm2_drtm_lsp.h in
 * OP-TEE, takes 32 bytes of event data per FF-A direct request from
 * endpoint 0 and runs TPM2_PCR_Event at locality 2 with them: each object
 * is recorded as its SHA-256 digest (all zeroes for a missing initrd),
 * hashed by the TPM in the algorithm of every active PCR bank. The endpoint
 * serves these requests with interrupts masked, so it answers with a
 * direct response or FFA_ERROR, never FFA_INTERRUPT or FFA_YIELD. It is to
 * be used before any other OP-TEE client runs: Xen is done before dom0
 * exists.
 */

#include <xen/acpi.h>
#include <xen/bootinfo.h>
#include <xen/delay.h>
#include <xen/dom0less-build.h>
#include <xen/errno.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/mm.h>
#include <xen/param.h>
#include <xen/sha2.h>
#include <xen/string.h>
#include <xen/unaligned.h>
#include <xen/vmap.h>

#include <asm/domain_build.h>
#include <asm/drtm.h>
#include <asm/smccc.h>
#include <asm/tee/ffa.h>

#define DRTM_CLOSE_LOCALITY             0xC4000115U
#define DRTM_NOT_SUPPORTED              -1
#define DRTM_ALREADY_CLOSED             -11
#define DRTM_DLME_LOCALITY              2

/* From OP-TEE's tpm2_drtm_lsp.h: x3 = command | argument << 8 */
#define TPM2_DRTM_CMD_DLME_EVENT        7
#define TPM2_DRTM_RES_OK                0
#define TPM2_DRTM_RES_BUSY              4

#define DRTM_PCR                        19
#define DRTM_TPM_RETRIES                100

/* FF-A endpoint ID of the secure world's TPM D-RTM endpoint, 0 = none */
static unsigned int __initdata opt_drtm_tpm;
integer_param("drtm-tpm", opt_drtm_tpm);

/*
 * 0 on success, -EACCES when the endpoint refuses the request (locality 2
 * closed), -EBUSY when the TPM stayed busy, -EIO for anything else.
 */
static int __init drtm_tpm_event(const uint8_t digest[SHA2_256_DIGEST_SIZE])
{
    struct arm_smccc_1_2_regs arg = {
        .a0 = FFA_MSG_SEND_DIRECT_REQ_64,
        .a1 = opt_drtm_tpm,
        .a3 = TPM2_DRTM_CMD_DLME_EVENT | (DRTM_PCR << 8),
        .a4 = get_unaligned_le64(digest),
        .a5 = get_unaligned_le64(digest + 8),
        .a6 = get_unaligned_le64(digest + 16),
        .a7 = get_unaligned_le64(digest + 24),
    };
    struct arm_smccc_1_2_regs resp;
    unsigned int i;

    for ( i = 0; i < DRTM_TPM_RETRIES; i++ )
    {
        arm_smccc_1_2_smc(&arg, &resp);
        if ( resp.a0 != FFA_MSG_SEND_DIRECT_RESP_64 ||
             resp.a3 != TPM2_DRTM_RES_BUSY )
            break;
        mdelay(1);
    }

    if ( resp.a0 == FFA_MSG_SEND_DIRECT_RESP_64 &&
         resp.a3 == TPM2_DRTM_RES_OK )
        return 0;
    if ( resp.a0 == FFA_ERROR && (int32_t)resp.a2 == FFA_RET_DENIED )
        return -EACCES;

    printk(XENLOG_ERR "DRTM: TPM endpoint answered %#lx %#lx %#lx\n",
           resp.a0, resp.a2, resp.a3);

    if ( resp.a0 == FFA_MSG_SEND_DIRECT_RESP_64 &&
         resp.a3 == TPM2_DRTM_RES_BUSY )
        return -EBUSY;

    return -EIO;
}

static int __init drtm_record(const char *name,
                              const uint8_t digest[SHA2_256_DIGEST_SIZE])
{
    int rc = drtm_tpm_event(digest);

    printk("DRTM: PCR %u %s %*phN: %d\n", DRTM_PCR, name,
           SHA2_256_DIGEST_SIZE, digest, rc);

    return rc;
}

static int __init drtm_measure_module(const char *name,
                                      const struct boot_module *mod)
{
    uint8_t digest[SHA2_256_DIGEST_SIZE] = {};
    void *va;

    if ( mod )
    {
        va = ioremap_cache(mod->start, mod->size);
        if ( !va )
            return -ENOMEM;
        sha2_256(digest, va, mod->size);
        iounmap(va);
    }

    return drtm_record(name, digest);
}

void __init drtm_measure_dom0(void)
{
    uint8_t digest[SHA2_256_DIGEST_SIZE];
    const struct bootcmdline *cmd;
    const char *cmdline;
    struct arm_smccc_res res;
    int rc;

    if ( !opt_drtm_tpm )
        return;

    if ( opt_drtm_tpm > 0xffff )
        panic("DRTM: drtm-tpm=%#x is not an FF-A endpoint ID\n", opt_drtm_tpm);

    /* hwdom_bootargs() is the rule of a device tree boot */
    if ( !acpi_disabled )
        panic("DRTM: drtm-tpm= needs a device tree boot\n");

    if ( is_dom0less_mode() )
        printk(XENLOG_WARNING
               "DRTM: dom0less boot: no domain is measured into PCR 19\n");
    else
    {
        /* Always three events, so a verifier replays a fixed sequence */
        rc = drtm_measure_module("kernel",
                                 boot_module_find_by_kind(BOOTMOD_KERNEL));
        if ( !rc )
            rc = drtm_measure_module("initrd",
                                     boot_module_find_by_kind(BOOTMOD_RAMDISK));
        if ( !rc )
        {
            cmd = boot_cmdline_find_by_kind(BOOTMOD_KERNEL);
            cmdline = hwdom_bootargs(cmd ? cmd->cmdline : NULL) ?: "";
            sha2_256(digest, cmdline, strlen(cmdline));
            rc = drtm_record("cmdline", digest);
        }

        /*
         * A failed measurement leaves PCR 19 short of dom0, which a
         * verifier sees. Locality 2 must close whatever happened, or dom0
         * could extend PCR 19 itself.
         */
        if ( rc )
            printk(XENLOG_ERR "DRTM: measuring dom0 failed: %d\n", rc);
    }

    res = arm_smccc_smc(DRTM_CLOSE_LOCALITY, DRTM_DLME_LOCALITY);
    if ( (long)res.a0 == DRTM_ALREADY_CLOSED )
        printk(XENLOG_WARNING
               "DRTM: TPM locality %u was not open (no dynamic launch?)\n",
               DRTM_DLME_LOCALITY);
    else if ( (long)res.a0 == DRTM_NOT_SUPPORTED )
        panic("DRTM: firmware does not implement DRTM_CLOSE_LOCALITY\n");
    else if ( (long)res.a0 )
        panic("DRTM: closing TPM locality %u failed: %ld\n",
              DRTM_DLME_LOCALITY, (long)res.a0);

    /* With the locality closed the endpoint must refuse a measurement */
    memset(digest, 0, sizeof(digest));
    rc = drtm_tpm_event(digest);
    if ( rc != -EACCES )
        panic("DRTM: endpoint %#x did not refuse an event after close: %d\n",
              opt_drtm_tpm, rc);

    printk("DRTM: TPM locality %u closed (%d)\n", DRTM_DLME_LOCALITY,
           FFA_RET_DENIED);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
