# Xen on the Qualcomm SA8797P (Nord) Ride, open boot stack

Notes, configurations and test scripts for running this branch on the
SA8797P Ride with the open firmware stack: Xen at EL2, Linux dom0, xl
guests, and OP-TEE reached from dom0 and guests over FF-A through Xen's
FF-A mediator. Not meant for upstream: the patches below this commit are.

## Sources

| component | tree | notes |
|---|---|---|
| TF-A | EmbeddedAndroid/arm-trusted-firmware `nord/xen-ffa` | `PLAT=nord SPD=spmd SPMD_SPM_AT_SEL2=0` |
| OP-TEE | EmbeddedAndroid/optee_os `nord/xen-ffa` | S-EL1 SPMC with NS virtualization |
| EDK2 | edk2 9714474bf2e7 + edk2-platforms `NordOpenBoardPkg` | BL33 in the FIP |
| Xen | this branch | arm64 defconfig + `xen-nord.config` |
| dom0 / domU Linux | EmbeddedAndroid/kernel-topics `nord/xen-dom0` | arm64 defconfig + its FF-A fragment |
| Xen toolstack | this branch, `tools/` | cross built, see below |

## Firmware

TF-A (with `SPD=spmd` the FIP also carries the SPMC manifest as
TOS_FW_CONFIG):

    make PLAT=nord SPD=spmd SPMD_SPM_AT_SEL2=0 BL32=<tee-raw.bin> \
         BL33=<NORDOPENBOARDPKG.fd> bl2 fip

BL2 boots from the `tz` partition and must be signed the way the board's
XBL expects; that step is board specific and not covered here.

OP-TEE:

    make PLATFORM=qcom-nord CFG_CORE_SEL1_SPMC=y \
         CFG_NS_VIRTUALIZATION=y CFG_VIRT_GUEST_COUNT=4

`CFG_NS_VIRTUALIZATION=y` is what makes OP-TEE subscribe to the VM
created/destroyed messages: each FF-A domain then gets its own OP-TEE
partition and Xen can reclaim a destroyed guest's shared memory. Without it
the FF-A path works, but a destroyed guest with live shares stays dying
(`FFA_MEM_RECLAIM` returns DENIED).

## Xen

    make -C xen XEN_TARGET_ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
    cat docs/misc/arm/sa8797p/xen-nord.config >> xen/.config
    make -C xen XEN_TARGET_ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
    make -C xen XEN_TARGET_ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

`xen-nord.config`: `HAS_GENI_UART` (console), `GICV3_ESPI` (the apps SMMU
context interrupts and the APSS watchdog are extended SPIs), `HAS_ITS`,
`FFA` (the FF-A mediator), `DEBUG`, `IOREQ_SERVER` (forwards a guest's
virtio-mmio accesses to a device model such as QEMU in dom0; selectable
on Arm only with `EXPERT`), `BOOT_TIME_CPUPOOLS` and `SCHED_NULL` (a
dom0less domain's `domain-cpupool` names a `xen,cpupool` node; without
`BOOT_TIME_CPUPOOLS` Xen ignores both and the domain runs in Pool-0).
Do not enable GENI early printk
(`EARLY_UART_CHOICE_GENI`): it is not part of this branch and the
experimental versions hang at entry on this board.

Toolstack (xl, libxl, xenstored, xenconsoled), cross built in a Debian
trixie arm64-multiarch environment:

    ./configure --host=aarch64-linux-gnu --prefix=/usr --sysconfdir=/etc \
        --disable-docs --disable-stubdom --disable-golang --disable-ocamltools \
        --disable-pygrub --disable-qemu-traditional --disable-ipxe \
        --disable-rombios --disable-seabios --disable-ovmf \
        --with-system-qemu=/bin/false --disable-xsmpolicy --disable-monitors \
        --without-systemd --disable-pvshim
    make build-tools XEN_TARGET_ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- debug=n
    make install-tools DESTDIR=<root> XEN_TARGET_ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- debug=n

(the Python bindings are dropped from `tools/Makefile`: they do not cross
build against the host interpreter and are not needed on the board). The
dom0 rootfs used here is a small Buildroot image, so the toolstack was
installed with its Debian libraries and loader in a private directory
(`patchelf --set-interpreter`, `--set-rpath`); a distribution rootfs does
not need that.

## Boot

EDK2 loads `EFI/BOOT/bootaa64.efi` (xen.efi) from the ESP (UFS LUN 0,
partition `efi`, FAT16 with 4 KiB sectors: EDK2 ignores FAT32). Xen reads
`bootaa64.cfg` next to it:

```
[global]
default=f

[f]
options=console=dtuart dtuart=serial0 conswitch=x loglvl=all guest_loglvl=all dom0_mem=12G bootscrub=0 conring_size=4M
kernel=Image root=/dev/ram0 rw console=hvc0 earlycon=xenboot clk_ignore_unused pd_ignore_unused
ramdisk=initrd
dtb=nord.dtb
```

- `clk_ignore_unused pd_ignore_unused`: otherwise dom0's unused clock and
  power domain cleanup turns off the console serial engine that Xen uses,
  and the next Xen UART access stalls the SoC.
- No `irqchip.gicv3_pseudo_nmi=1` on the dom0 command line: with it dom0
  spins early in boot under Xen (root cause open).
- `iommu=no` in `options=` runs Xen without its SMMU driver; dom0 is
  direct mapped either way.
- `conring_size=4M` keeps the Xen boot log in `xl dmesg` after a verbose
  dom0 boot.

## Guests

`domu/mkinitramfs.sh` builds the guest initramfs (BusyBox, `domu/init`,
`domu/poweroff`). The guest kernel is the dom0 Image. The `.cfg` files
expect the Image at `/esp/EFI/BOOT/Image` and the initramfs at
`/boot/domu.cpio.gz`:

| config | what it is |
|---|---|
| `d1.cfg` | 1 vCPU, boots and powers itself off |
| `s1.cfg` | 2 vCPUs, stays up until `xl shutdown` / `xl destroy` |
| `par.cfg` | template (`@N@`) for parallel guests |
| `f1.cfg` | `tee = "ffa"`: the guest sees FF-A and reaches OP-TEE |
| `f0.cfg` | no `tee`: Xen answers its FF-A calls with NOT_SUPPORTED |

The guest init prints `NXDOMU-UP`, `NXDOMU-FFA <name> ffa=<0|1>
optee=<0|1> tee0=<0|1>` and, on the way down, `NXDOMU-DOWN`.

## dom0less guests with virtio devices

dom0 keeps every Nord device and serves disk, network and console to
dom0less guests as virtio-mmio devices emulated by QEMU (machine
`xenpvh`), with the guests granting their DMA buffers to dom0
(`xen,grant-dma`). The guests are described in `/chosen` of the DTB that
`bootaa64.cfg` names; the EFI stub loads each module from the ESP:

```
chosen {
	xen,uefi-cfg-load;

	g1 {
		compatible = "xen,domain";
		#address-cells = <2>;
		#size-cells = <2>;
		memory = <0 524288>;
		cpus = <2>;
		vpl011;
		xen,enhanced = "no-xenstore";
		start-paused;
		virtio-mmio = <3>;
		virtio-grant-dma = <0>;

		module@1 {
			compatible = "multiboot,kernel", "multiboot,module";
			xen,uefi-binary = "g-Image";
			bootargs = "console=ttyAMA0";
		};
		module@2 {
			compatible = "multiboot,ramdisk", "multiboot,module";
			xen,uefi-binary = "g-initrd";
		};
	};
};
```

Xen writes three virtio-mmio nodes (0x200 bytes each from 0x02000000,
SPIs 33 to 35) and a `xen,grant-dma` node pointing them at dom0 (domid 0).
The same nodes can instead come from a passthrough partial DT
(`multiboot,device-tree` module) as libxl writes them for xl guests; its
phandles must then stay below 65000 (Xen allocates its own above the
partial DT's highest one and refuses an overlap) and `nr_spis` must
cover the last virtio SPI.

In dom0, after xenstored and xen-init-dom0:

    qemu-system-aarch64 -xen-domid 1 -xen-attach -M xenpvh -m 512 \
        -smp 2,maxcpus=2 -nodefaults -display none -no-shutdown \
        -global virtio-mmio.force-legacy=false \
        -drive if=none,id=d0,file=g1.img,format=raw,cache=writeback \
        -device virtio-blk-device,drive=d0,iommu_platform=on \
        -netdev tap,id=n0,ifname=tap1,script=no,downscript=no \
        -device virtio-net-device,netdev=n0,iommu_platform=on \
        -chardev file,id=c0,path=g1-con.log \
        -device virtio-serial-device,iommu_platform=on \
        -device virtconsole,chardev=c0 &
    xl unpause 1

- `start-paused` keeps the guest from probing its virtio devices before
  QEMU has registered its ioreq server; without it the first access
  aborts the guest (or, with `trap-unmapped-accesses = <0>`, Linux reads
  0xffffffff, logs "Wrong magic value" and never probes the device).
- Grant DMA in Linux needs modern virtio (`force-legacy=false`, QEMU's
  virtio-mmio default is legacy) and `VIRTIO_F_ACCESS_PLATFORM`
  (`iommu_platform=on`); the guest kernel needs `CONFIG_XEN_VIRTIO`.
- QEMU for dom0: `--target-list=aarch64-softmmu --enable-xen`, with TCG
  left enabled: without a TCG machine nothing selects the virtio devices.
- The same guest can be recreated after `xl destroy` with `xl create -p`
  and `device_tree = "g1-pt.dtb"`, `vuart = "sbsa_uart"`, `nr_spis = 12`,
  then QEMU and `xl unpause` as above.

## Tests

The scripts in `tests/` run in dom0 and print one
`NORDTC <set>.<case> pass|fail` line per case on the console:

- `xenchk <phase>`: hypervisor type and version (the Xen banner carries
  the build fingerprint passed as `nxenfp=` on the dom0 command line),
  dom0 control domain, 18 vCPUs, ESP mounted, dom0 kernel log clean.
- `xenffa <phase> dom0`: Xen selected its FF-A mediator and found the
  SPMC, dom0's FF-A driver found the firmware, the OP-TEE driver bound
  over FF-A, `/dev/tee0` and `/dev/teepriv0` exist.
- `xenffa <phase> guests`: starts xenstored and xenconsoled, then `f1`
  must reach OP-TEE and `f0` must not.
- `xenxl <phase>`: toolstack start, `xl info`/`list`/`dmesg`, a guest
  that powers itself off, four guests in parallel, `xl shutdown` of a
  guest that stays up, and `xl destroy` returning all guest memory.

On top of these, OP-TEE's own `xtest` runs in dom0 unchanged.

## Board facts that matter to Xen

- Console: `qcom,geni-debug-uart` at 0x884000; firmware leaves it at
  115200 8N1 (the driver does not program the clock).
- GIC-700 with 988 SPIs and 1024 extended SPIs, 18 redistributors.
- CPUs: 18 Qualcomm Oryon cores, all identical.
- SMMUs: three apps MMU-500 (96 context banks and 96 context interrupts
  each, all extended SPIs), two GPU MMU-500 without translation support.
- PDC (`qcom,pdc`) carries the SPMI PMIC arbiter and the USB PHY wakeup
  interrupts; the Qualcomm platform code routes them to dom0.
- About 256MB of RAM below 4GB is left to Xen, so dom0's bank 0 comes from
  high memory and dom0 has no memory below 4GB (no swiotlb).
- `memory@80000000` has a zero size in the DT (EDK2 passes the memory map
  through EFI).
- Linux `qcom_scm` needs the platform's SCM "is call available" answer, or
  it falls back to the legacy convention, whose SMC 1 is Xen's PSCI 0.1
  CPU_OFF.
