/*
 * ACPI tables for the virt machine, the way QEMU's build_virt builds them.
 *
 * Field layouts come from Linux's actbl.h/actbl1.h/actbl2.h/actbl3.h (Intel's
 * ACPICA definitions) and from hw/arm/virt-acpi-build.c for the virt-specific
 * contents. Every table gets a real ACPI header: signature, length, revision,
 * a checksum that makes the byte sum zero, OEM ids and the creator id.
 *
 * What a Windows-ARM64 guest needs to see:
 *   RSDP   "RSD PTR " at 0x4800_0000, pointing at the XSDT (and a low RSDT,
 *          because ACPI 1.0 readers only look at the 32-bit field)
 *   FACP   PSCI 0.2+ via HVC (no EL3 here), HW_REDUCED_ACPI, DSDT
 *   DSDT   _SB.PCI0 with _CRS / _HID / _UID / _DSM, plus the ARM known-folder
 *   APIC   GICC per CPU, GICD, GICR, ITS (Windows finds its MSI domain here)
 *   GTDT   Non-secure EL1 timer, 62.5 MHz counter
 *   MCFG   ECAM base, so PCIe config space is discoverable without DT
 *   IORT   PCIe root complex -> ITS ID mapping (where MSI interrupts come from)
 *   SPCR   PL011
 * The one Linux needs and Windows does not is PPTT (cache topology); it is
 * generated too, so the same image boots both.
 */

#ifndef HENGEVM_ACPI_H
#define HENGEVM_ACPI_H

#include <stdint.h>
#include <stddef.h>

/* The tables land here; VIRT_ACPI_BASE in virt.h must match. */
#define ACPI_PHYS_BASE   0x48000000ull
#define ACPI_MAX_SIZE    0x00100000u /* 1 MB reserved for them */

/* Where each table starts inside the blob (offsets from ACPI_PHYS_BASE). */
typedef struct HvAcpi {
    uint32_t off_rsdp;
    uint32_t off_rsdt;
    uint32_t off_xsdt;
    uint32_t off_fadt;
    uint32_t off_dsdt;
    uint32_t off_madt;
    uint32_t off_gtdt;
    uint32_t off_mcfg;
    uint32_t off_spcr;
    uint32_t off_iort;
    uint32_t off_pptt;
    uint32_t total; /* the blob's length */
    int ncpus;
} HvAcpi;

/* Writes the whole set into buf (which must be ACPI_MAX_SIZE bytes and is
   zeroed by the caller). Returns 0, or -1 if buf is too small.
   Values that belong to the machine come in as arguments so this file stays
   free of device headers: pass nhv_virt_acpi_defaults() or the real ones. */
typedef struct HvAcpiParams {
    const char *oem_id;      /* 6 bytes, e.g. "HENGEM" */
    const char *oem_table_id;/* 8 bytes, e.g. "VIRT0001" */
    uint64_t gic_dist;
    uint64_t gic_redist;
    uint64_t gic_its;
    uint32_t gic_its_id;     /* the ITS's own ID: IORT names it, and must name a real one */
    uint32_t gic_its_gsi_base; /* MADT's GIC ITS "GIC ITS Base": this is where Windows puts its LPIs */
    uint64_t uart;
    uint64_t ecam;
    uint64_t ecam_size;
    uint32_t pci_bus_min;
    uint32_t pci_bus_max;
    uint32_t counter_hz;
    int ncpus;
    /* PSCI conduit. FADT 5.2.9 wants this to follow the DT's /psci, which is hvc
       here because there is no EL3 to trap smc. Both are filled in. */
    uint8_t psci_conduit;   /* 2: hvc, 3: smc */
    uint8_t psci_version;   /* minor; major is always 0 in this field */
} HvAcpiParams;

int hv_acpi_build(uint8_t *buf, size_t buf_size, const HvAcpiParams *p, HvAcpi *out);

/* The sizes above, for callers that want to sanity-check or log them. */
static inline uint64_t hv_acpi_phys(uint32_t off)
{
    return ACPI_PHYS_BASE + off;
}

#endif /* HENGEVM_ACPI_H */
