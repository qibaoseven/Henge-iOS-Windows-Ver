#ifndef HENGEVM_VIRT_H
#define HENGEVM_VIRT_H

#include <stdint.h>

#include "fw_cfg.h"
#include "gicv3.h"
#include "machine.h"
#include "pflash.h"
#include "pl011.h"
#include "pl031.h"
#include "pl061.h"

/* QEMU's "virt" machine, which ArmVirtQemu (edk2) is built for and which is the
   shortest path to a firmware Windows will take an interest in.

   The map below is hw/arm/virt.c's base_memmap[] verbatim. Nothing here is
   Windows-specific: it is the platform a generic arm64 UEFI expects, and the
   choice was made because Windows has no other shape it boots in. */
#define VIRT_FLASH_BASE   0x00000000ull
#define VIRT_FLASH_SIZE   0x08000000ull /* 128 MB, two 64 MB banks */
#define VIRT_GIC_DIST     0x08000000ull
#define VIRT_GIC_DIST_SIZE 0x00010000ull
#define VIRT_GIC_ITS      0x08080000ull
#define VIRT_GIC_ITS_SIZE 0x00020000ull
#define VIRT_GIC_REDIST   0x080A0000ull
#define VIRT_GIC_REDIST_SIZE 0x00F60000ull /* 123 CPUs' worth, as in QEMU */
#define VIRT_UART0        0x09000000ull
#define VIRT_RTC          0x09010000ull
#define VIRT_FW_CFG       0x09020000ull
#define VIRT_GPIO         0x09030000ull
#define VIRT_PCIE_MMIO    0x10000000ull
#define VIRT_PCIE_MMIO_SIZE 0x2EFF0000ull
#define VIRT_PCIE_PIO     0x3EFF0000ull /* unused: no I/O BARs here yet */
#define VIRT_PCIE_ECAM    0x3F000000ull
#define VIRT_PCIE_ECAM_SIZE 0x01000000ull
#define VIRT_PCIE_INTX    0x30 /* INTID of SPI 16: what QEMU's gpex uses for slot 0 */

/* The DT sits at the bottom of RAM, and RAM starts after it: a DT inside the
   region the firmware is told is free memory gets written over. UEFI finds it
   from x0 at reset, not by searching. */
#define VIRT_RAM_BASE     0x40000000ull
#define VIRT_DTB_PA       0x40000000ull
#define VIRT_DTB_SIZE     0x00100000ull /* 1 MB: the tree itself is ~65 KB */
#define VIRT_GUEST_RAM    0x40100000ull /* where the firmware's memory starts */
/* Room for the ACPI tables when they arrive (M2), well inside the 48-bit PA space. */
#define VIRT_ACPI_BASE    0x48000000ull
#define VIRT_ACPI_SIZE    0x00100000ull

typedef struct HvVirt {
    HvMachine *m;
    HvCpu *cpu;
    int ncpus;
    uint64_t ram_size;
    uint8_t *ram;
    HvGic *gic;
    HvPl011 *uart;
    HvPl031 *rtc;
    HvPl061 *gpio;
    HvPflash *flash;
    HvFwCfg *fw;
    uint64_t dtb_len; /* 0: no tree was written */
} HvVirt;

/* Builds the machine around m and cpus[0..cfg_ncpus). code_path and vars_path
   are the two pflash images (edk2-aarch64-code.fd, edk2-arm-vars.fd). */
int hv_virt_create(HvVirt *b, HvMachine *m, HvCpu *cpus, int ncpus, uint64_t ram_size,
                   const char *code_path, const char *vars_path);
/* CPU reset: EL1, MMU off, interrupts masked, PC at the start of the code flash
   (0x0), secondaries off until PSCI CPU_ON. */
void hv_virt_reset_cpu(HvVirt *b);
void hv_virt_destroy(HvVirt *b);
/* Writes the variables bank back (UEFI's settings survive a reboot). */
int hv_virt_save_vars(HvVirt *b, const char *path);

#endif
