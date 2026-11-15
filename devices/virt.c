#include "virt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "acpi.h"
#include "disk.h"
#include "dtb.h"
#include "its.h"
#include "pcie.h"
#include "timer.h"
#include "virtio_blk.h"
#include "virtio_net.h"

/* What this machine is, and what it is not (yet).

   The platform is QEMU's virt because that is the only arm64 shape for which a
   ready-made UEFI exists: Windows ARM64 boots nothing but UEFI with ACPI, and
   ArmVirtQemu is built for this exact memory map and interrupt wiring.

   Departures from QEMU, all deliberate:

   1. No etc/table-loader and no fw_cfg DMA. QEMU puts ACPI tables in fw_cfg and
      has the firmware relocate them, because on x86 QEMU cannot know where they
      will fit. Henge builds its own tables (devices/acpi.c) at a fixed physical
      address and publishes them through ordinary fw_cfg entries, so the
      firmware has nothing to relocate.

   2. Modern virtio only, INTx never. Every device here interrupts through
      MSI-X -> ITS -> GIC, which is what Windows' virtio drivers require.

   3. RAM above 4 GiB. The ACPI tables sit at 0x4800_0000, and VIRT_ACPI_SIZE is
      reserved around them, so the firmware's memory stops at 0x4810_0000 and is
      continued at 0x1_0000_0000 (the 36-bit PA space QEMU's virt offers a
      Windows guest). RAM that ran straight through the ACPI region would be
      either overwritten by the tables or described twice. */ 

/* A redistributor's base address to its CPU: the machine places them one after
   another, 2 x 64 KB each, from VIRT_GIC_REDIST. */
static int virt_rd_cpu(uint32_t rd_base) {
    if (rd_base < VIRT_GIC_REDIST) return 0;
    uint32_t idx = (rd_base - (uint32_t)VIRT_GIC_REDIST) >> 17;
    return idx < HV_MAX_CPUS ? (int)idx : 0;
}

/* RAM split around the ACPI region: lo_size ends at VIRT_ACPI_BASE, hi_size
   starts at VIRT_RAM_HIGH. Returns -1 if the low part cannot hold the DT. */
static int virt_split_ram(uint64_t ram_size, uint64_t *lo, uint64_t *hi) {
    if (ram_size <= VIRT_DTB_SIZE) return -1;
    uint64_t below = VIRT_ACPI_BASE - VIRT_RAM_BASE; /* room under the tables */
    if (ram_size <= below) {
        *lo = ram_size;
        *hi = 0;
        return 0;
    }
    *lo = below;
    *hi = ram_size - below;
    return 0;
}

/* The PSCI conduit the DT passes to the guest, said again in the FADT. Reading
   it back from the tree keeps the two from drifting apart. */
static void virt_psci_params(uint8_t *dtb, HvAcpiParams *ap) {
    ap->psci_conduit = 2; /* hvc */
    ap->psci_version = 2; /* PSCI 0.2 */
    if (!dtb) return;
    uint8_t *p = memmem(dtb, VIRT_DTB_SIZE, "method", 6);
    if (!p) return;
    size_t left = VIRT_DTB_SIZE - (size_t)(p - dtb);
    if (left > 32 && memmem(p, 32, "smc", 3)) ap->psci_conduit = 3;
}

int hv_virt_create(HvVirt *b, HvMachine *m, HvCpu *cpus, int ncpus, uint64_t ram_size,
                   const char *code_path, const char *vars_path) {
    /* Read the disk request before clearing: the caller sets it on the struct. */
    const char *disk_path = b->disk_path;
    int disk_ro = b->disk_ro;
    memset(b, 0, sizeof *b);
    b->disk_path = disk_path;
    b->disk_ro = disk_ro;
    b->m = m;
    b->cpu = cpus;
    b->ncpus = ncpus < 1 ? 1 : ncpus;
    b->ram_size = ram_size;

    /* --- guest RAM ---
       Reserved, not committed, as the rest of the engine assumes. The first
       megabyte of the low part holds the DT, and the ACPI window is carved out
       of the middle; RAM continues above 4 GiB (see the note at the top). */
    uint64_t lo_size = 0, hi_size = 0;
    if (virt_split_ram(ram_size, &lo_size, &hi_size) != 0) return -1;
    b->ram = mmap(NULL, (size_t)lo_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (b->ram == MAP_FAILED) return -1;
    if (hv_machine_add_ram(m, VIRT_RAM_BASE, lo_size, b->ram, 0) != 0) goto fail;
    if (hi_size) {
        b->ram_hi_size = hi_size;
        b->ram_hi = mmap(NULL, (size_t)hi_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (b->ram_hi == MAP_FAILED) goto fail;
        if (hv_machine_add_ram(m, VIRT_RAM_HIGH, hi_size, b->ram_hi, 0) != 0) goto fail;
    }

    /* The tree the firmware will read. Written before anything can execute, and
       never touched again: it describes the map, not the state. */
    b->dtb_len = hv_dtb_build(b->ram, VIRT_DTB_SIZE, VIRT_GUEST_RAM, lo_size - VIRT_DTB_SIZE, (uint32_t)b->ncpus);
    if (!b->dtb_len) {
        fprintf(stderr, "hengevm: the device tree did not fit in %llu bytes\n", (unsigned long long)VIRT_DTB_SIZE);
        goto fail;
    }
    if (m->log)
        fprintf(stderr, "hengevm: device tree %llu bytes at %llx; RAM %llu MiB at %llx + %llu MiB at %llx\n",
                (unsigned long long)b->dtb_len, (unsigned long long)VIRT_DTB_PA,
                (unsigned long long)((lo_size - VIRT_DTB_SIZE) >> 20), (unsigned long long)VIRT_GUEST_RAM,
                (unsigned long long)(hi_size >> 20), (unsigned long long)VIRT_RAM_HIGH);

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

    /* The ITS: without it a PCIe device has no MSI at all, and Windows'
       virtio drivers will not take an interrupt line instead. */
    b->its = hv_its_create(m, b->gic, VIRT_GIC_ITS, VIRT_GIC_ITS_SIZE, virt_rd_cpu);
    if (!b->its) goto fail;

    b->pci = hv_pcie_create(m, b->gic, VIRT_PCIE_ECAM, VIRT_PCIE_ECAM_SIZE, VIRT_PCIE_MMIO,
                            VIRT_PCIE_MMIO_SIZE, VIRT_PCIE_INTX);
    if (!b->pci) goto fail;
    hv_pcie_set_its(b->pci, b->its);

    /* --- the ACPI set ---
       Built into reserved host memory and published as fw_cfg files, which is
       how ArmVirtQemu finds them (it reads "etc/acpi/rsdp" by name). The
       addresses inside are absolute, so the same bytes would also work if
       something copied them to ACPI_PHYS_BASE; nothing has to. */
    HvAcpiParams ap;
    memset(&ap, 0, sizeof ap);
    ap.oem_id = "HENGEM";
    ap.oem_table_id = VIRT_ACPI_OEM_TABLE_ID;
    ap.gic_dist = VIRT_GIC_DIST;
    ap.gic_redist = VIRT_GIC_REDIST;
    ap.gic_its = VIRT_GIC_ITS;
    ap.gic_its_id = VIRT_GIC_ITS_ID;
    ap.gic_its_gsi_base = VIRT_GIC_LPI_BASE;
    ap.uart = VIRT_UART0;
    ap.ecam = VIRT_PCIE_ECAM;
    ap.ecam_size = VIRT_PCIE_ECAM_SIZE;
    ap.pci_bus_min = 0;
    ap.pci_bus_max = VIRT_PCI_BUS_MAX;
    ap.counter_hz = VIRT_COUNTER_HZ;
    ap.ncpus = b->ncpus;
    virt_psci_params(b->ram, &ap);

    static const struct {
        int which;
        const char *name;
    } names[] = {
        {0, "etc/acpi/rsdp"}, {1, "etc/acpi/rsdt"},   {2, "etc/acpi/xsdt"},     {3, "etc/acpi/facp"},
        {4, "etc/acpi/dsdt"}, {5, "etc/acpi/apic"},   {6, "etc/acpi/gtdt"},     {7, "etc/acpi/mcfg"},
        {8, "etc/acpi/spcr"}, {9, "etc/acpi/iort"},   {10, "etc/acpi/pptt"},
    };
    uint32_t offs[11] = {0};
    uint32_t lens[11] = {0};
    if (hv_acpi_build(b->acpi_blob, sizeof b->acpi_blob, &ap, &b->acpi) != 0) {
        fprintf(stderr, "hengevm: the ACPI tables did not fit in %u bytes\n", (unsigned)sizeof b->acpi_blob);
        goto fail;
    }
    offs[0] = b->acpi.off_rsdp, lens[0] = 36;
    offs[1] = b->acpi.off_rsdt, lens[1] = 36 + 8 * 4;
    offs[2] = b->acpi.off_xsdt, lens[2] = 36 + 8 * 8;
    offs[3] = b->acpi.off_fadt, lens[3] = 276;
    offs[4] = b->acpi.off_dsdt, lens[4] = b->acpi.off_fadt - b->acpi.off_dsdt;
    offs[5] = b->acpi.off_madt, lens[5] = b->acpi.off_gtdt - b->acpi.off_madt;
    offs[6] = b->acpi.off_gtdt, lens[6] = b->acpi.off_mcfg - b->acpi.off_gtdt;
    offs[7] = b->acpi.off_mcfg, lens[7] = b->acpi.off_spcr - b->acpi.off_mcfg;
    offs[8] = b->acpi.off_spcr, lens[8] = b->acpi.off_iort - b->acpi.off_spcr;
    offs[9] = b->acpi.off_iort, lens[9] = b->acpi.off_pptt - b->acpi.off_iort;
    offs[10] = b->acpi.off_pptt, lens[10] = b->acpi.total - b->acpi.off_pptt;
    b->acpi_bytes = b->acpi.total;
    for (int i = 0; i < 11; i++) {
        if (hv_fw_cfg_add_file(b->fw, names[i].name, b->acpi_blob + offs[i], lens[i]) != 0) {
            fprintf(stderr, "hengevm: fw_cfg refused %s\n", names[i].name);
            goto fail;
        }
    }

    /* --- devices ---
       One disk and one NIC: what an install needs to see. Both modern virtio
       and both MSI-X only; hv_pcie_attach gives each a devfn on bus 0. */
    if (b->disk_path) {
        if (hv_disk_open(&b->disk, b->disk_path, 0, b->disk_ro) != 0) {
            fprintf(stderr, "hengevm: cannot open the disk image %s\n", b->disk_path);
            goto fail;
        }
        b->blk = hv_virtio_blk_create(m, b->pci, &b->disk, 0);
        if (!b->blk) goto fail;
    }
    {
        static const uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        b->net = hv_virtio_net_create(m, b->pci, mac);
        if (!b->net) goto fail;
    }

    m->tick_opaque = b;
    return 0;
fail:
    hv_virt_destroy(b);
    return -1;
}

static uint64_t tick(HvMachine *m, HvCpu *cpu) {
    HvVirt *b = m->tick_opaque;
    /* The guest queues ITS commands and rings a doorbell by writing GITS_CWRITER,
       which runs them there and then; this catches a queue left with work in it
       (the guest may also just write into the ring). */
    if (cpu->index == 0) hv_its_poll(b->its);
    /* Completions the block workers finished belong to the CPU thread. */
    if (cpu->index == 0) hv_virtio_blk_poll(b->blk);
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
        c->x[0] = VIRT_DTB_PA;   /* the DT, as the arm64 boot protocol passes it */
        c->off = i != 0;         /* secondaries wait for PSCI CPU_ON */
    }
}

int hv_virt_save_vars(HvVirt *b, const char *path) {
    if (!b->flash) return -1;
    return hv_pflash_save_vars(b->flash, path);
}

void hv_virt_destroy(HvVirt *b) {
    if (!b) return;
    /* CPU threads are gone by now (the caller stops running first). */
    if (b->blk) {
        hv_virtio_blk_destroy(b->blk);
        b->blk = NULL;
    }
    if (b->disk.fd > 0) hv_disk_close(&b->disk);
    free(b->net);
    b->net = NULL;
    if (b->its) {
        pthread_mutex_destroy(&b->its->mu);
        free(b->its->dev);
        free(b->its);
    }
    free(b->pci);
    free(b->gic);
    free(b->uart);
    free(b->rtc);
    free(b->gpio);
    hv_fw_cfg_destroy(b->fw);
    hv_pflash_destroy(b->flash);
    if (b->ram && b->ram != MAP_FAILED) munmap(b->ram, (size_t)(VIRT_ACPI_BASE - VIRT_RAM_BASE));
    if (b->ram_hi && b->ram_hi != MAP_FAILED) munmap(b->ram_hi, (size_t)b->ram_hi_size);
    memset(b, 0, sizeof *b);
}
