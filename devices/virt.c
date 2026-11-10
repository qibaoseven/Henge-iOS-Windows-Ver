#include "virt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "pcie.h"
#include "timer.h"

/* What this machine is, and what it is not (yet).

   The platform is QEMU's virt because that is the only arm64 shape for which a
   ready-made UEFI exists: Windows ARM64 boots nothing but UEFI with ACPI, and
   ArmVirtQemu is built for this exact memory map and interrupt wiring.

   Two departures from QEMU, both deliberate:

   1. No etc/table-loader and no fw_cfg DMA. QEMU puts ACPI tables in fw_cfg and
      has the firmware relocate them, because on x86 QEMU cannot know where they
      will fit. Henge generates its own tables and knows exactly where they go,
      so virt_acpi_setup() will place them at VIRT_ACPI_BASE and publish the
      addresses as ordinary fw_cfg entries. The firmware then has nothing to
      relocate, and fw_cfg keeps only the byte-stream interface.

   2. No ITS yet. QEMU's virt has one by default and GICv3 has no MSI without
      it, so PCIe devices here still interrupt through INTx on an SPI. That is
      enough for UEFI (which polls the UART and needs no device interrupt at
      all) and not enough for Windows' virtio drivers, which is the next thing
      to build. The window is left unmapped rather than faked: a firmware or
      guest that probes for the ITS finds nothing, which is true. */

int hv_virt_create(HvVirt *b, HvMachine *m, HvCpu *cpus, int ncpus, uint64_t ram_size,
                   const char *code_path, const char *vars_path) {
    memset(b, 0, sizeof *b);
    b->m = m;
    b->cpu = cpus;
    b->ncpus = ncpus < 1 ? 1 : ncpus;
    b->ram_size = ram_size;

    /* Guest RAM: reserved, not committed, as the rest of the engine assumes. */
    b->ram = mmap(NULL, (size_t)ram_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (b->ram == MAP_FAILED) return -1;
    if (hv_machine_add_ram(m, VIRT_RAM_BASE, ram_size, b->ram, 0) != 0) goto fail;

    /* The flash window: UEFI's entry point is here, at 0x0. */
    b->flash = hv_pflash_create(m, VIRT_FLASH_BASE, VIRT_FLASH_SIZE / 2, VIRT_FLASH_SIZE / 2,
                                code_path, vars_path);
    if (!b->flash) goto fail;

    b->gic = hv_gicv3_create(m, VIRT_GIC_DIST, VIRT_GIC_REDIST);
    if (!b->gic) goto fail;
    b->uart = hv_pl011_create(m, b->gic, VIRT_UART0, 32 + 1); /* SPI 1, as QEMU wires pl011 */
    b->rtc = hv_pl031_create(m, b->gic, VIRT_RTC, 32 + 2);    /* SPI 2 */
    b->gpio = hv_pl061_create(m, b->gic, VIRT_GPIO, 32 + 7);  /* SPI 7 */

    b->fw = hv_fw_cfg_create(m, VIRT_FW_CFG);
    if (!b->fw) goto fail;

    /* The second step, wired now because it costs one line and proves the map:
       an empty PCIe host bridge on QEMU's ECAM and MMIO windows. Its devices
       (virtio-blk, -net, -gpu) come with the ITS. */
    if (!hv_pcie_create(m, b->gic, VIRT_PCIE_ECAM, VIRT_PCIE_ECAM_SIZE, VIRT_PCIE_MMIO,
                        VIRT_PCIE_MMIO_SIZE, VIRT_PCIE_INTX))
        goto fail;

    m->tick_opaque = b;
    return 0;
fail:
    hv_virt_destroy(b);
    return -1;
}

static uint64_t tick(HvMachine *m, HvCpu *cpu) {
    HvVirt *b = m->tick_opaque;
    return hv_timer_update(b->gic, cpu);
}

void hv_virt_reset_cpu(HvVirt *b) {
    if (b->m->tick != tick) {
        b->m->tick = tick;
        b->m->tick_opaque = b;
    }
    for (int i = 0; i < b->ncpus; i++) {
        HvCpu *c = &b->cpu[i];
        hv_cpu_reset_index(c, b->m, (uint32_t)i);
        c->mpidr = (uint32_t)i; /* Aff0 = index, as QEMU's virt numbers its CPUs */
        c->pc = VIRT_FLASH_BASE; /* UEFI's entry, in the code flash */
        c->off = i != 0;         /* secondaries wait for PSCI CPU_ON */
    }
}

int hv_virt_save_vars(HvVirt *b, const char *path) {
    if (!b->flash) return -1;
    return hv_pflash_save_vars(b->flash, path);
}

void hv_virt_destroy(HvVirt *b) {
    if (!b) return;
    free(b->gic);
    free(b->uart);
    free(b->rtc);
    free(b->gpio);
    hv_fw_cfg_destroy(b->fw);
    hv_pflash_destroy(b->flash);
    if (b->ram && b->ram != MAP_FAILED) munmap(b->ram, (size_t)b->ram_size);
    memset(b, 0, sizeof *b);
}
