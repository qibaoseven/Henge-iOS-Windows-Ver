/*
 * test_acpi: the ACPI set the virt machine hands to a guest.
 *
 * Checks the properties that actually break an OS when they are wrong: every
 * table's length field and checksum, the addresses in the root tables, the
 * PSCI conduit in the FADT, the interrupt-controller nodes in the MADT and the
 * IORT's root-complex-to-ITS mapping, then dumps each table so an external
 * tool (iasl) can have a look too.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acpi.h"

static int failures;
static volatile int *checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        (*checks)++;                                                         \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static uint64_t be16(const uint8_t *p) { return ((uint64_t)p[0] << 8) | p[1]; }
static uint64_t be32(const uint8_t *p)
{
    return ((uint64_t)p[0] << 24) | ((uint64_t)p[1] << 16) | ((uint64_t)p[2] << 8) | p[3];
}
static uint64_t be64(const uint8_t *p) { return (be32(p) << 32) | be32(p + 4); }

static unsigned sum(const uint8_t *p, size_t n)
{
    unsigned s = 0;
    for (size_t i = 0; i < n; i++) s += p[i];
    return s & 0xff;
}

/* Writes one table out so iasl can disassemble it. */
static void dump(const char *dir, const char *name, const uint8_t *p, uint32_t len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.dat", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(p, 1, len, f);
    fclose(f);
}

static void check_table(const uint8_t *blob, uint32_t off, const char *sig, uint32_t want_len)
{
    const uint8_t *p = blob + off;
    uint32_t len = (uint32_t)be32(p + 4);
    if (want_len) CHECK(len == want_len);
    CHECK(len >= 36 && off + len <= ACPI_MAX_SIZE);
    CHECK(memcmp(p, sig, 4) == 0);
    CHECK(sum(p, len) == 0);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/tmp";
    volatile int n = 0;
    checks = &n;

    HvAcpiParams pr;
    memset(&pr, 0, sizeof(pr));
    pr.oem_id = "HENGEM";
    pr.oem_table_id = "VIRT0001";
    pr.gic_dist = 0x08000000ull;
    pr.gic_redist = 0x080A0000ull;
    pr.gic_its = 0x08080000ull;
    pr.uart = 0x09000000ull;
    pr.ecam = 0x3F000000ull;
    pr.ecam_size = 0x01000000ull;
    pr.pci_bus_min = 0;
    pr.pci_bus_max = 15;
    pr.counter_hz = 62500000u;
    pr.gic_its_id = 0;
    pr.gic_its_gsi_base = 8192;
    pr.psci_conduit = 2;
    pr.psci_version = 2;
    pr.ncpus = 2;

    uint8_t *blob = malloc(ACPI_MAX_SIZE);
    HvAcpi a, c;
    if (!blob) return 2;

    CHECK(hv_acpi_build(blob, ACPI_MAX_SIZE, &pr, &a) == 0);
    CHECK(a.total > 0 && a.total < ACPI_MAX_SIZE);

    /* The blob must fit the reserved window and not overlap anything else. */
    CHECK(a.off_rsdp == 0);

    /* ---- RSDP ---- */
    const uint8_t *rsdp = blob + a.off_rsdp;
    CHECK(memcmp(rsdp, "RSD PTR ", 8) == 0);
    CHECK(rsdp[15] == 2);
    CHECK(sum(rsdp, 20) == 0);
    CHECK(sum(rsdp, 36) == 0);
    CHECK(be64(rsdp + 24) == ACPI_PHYS_BASE + a.off_xsdt);
    CHECK(be32(rsdp + 16) == ACPI_PHYS_BASE + a.off_rsdt);

    /* ---- XSDT / RSDT ---- */
    const uint8_t *xsdt = blob + a.off_xsdt;
    CHECK(memcmp(xsdt, "XSDT", 4) == 0);
    uint32_t xsdt_entries = ((uint32_t)be32(xsdt + 4) - 36) / 8;
    CHECK(xsdt_entries == 8);
    CHECK(sum(xsdt, (size_t)be32(xsdt + 4)) == 0);
    /* Every pointer in the XSDT lands on a table that starts inside the blob. */
    for (uint32_t i = 0; i < xsdt_entries; i++) {
        uint64_t pa = be64(xsdt + 36 + i * 8);
        CHECK(pa >= ACPI_PHYS_BASE && pa < ACPI_PHYS_BASE + a.total);
    }
    const uint8_t *rsdt = blob + a.off_rsdt;
    CHECK(memcmp(rsdt, "RSDT", 4) == 0);
    CHECK(sum(rsdt, (size_t)be32(rsdt + 4)) == 0);
    CHECK(((uint32_t)be32(rsdt + 4) - 36) / 4 == 8);

    /* ---- each table ---- */
    check_table(blob, a.off_dsdt, "DSDT", 0);
    check_table(blob, a.off_fadt, "FACP", 276);
    check_table(blob, a.off_madt, "APIC", 0);
    check_table(blob, a.off_gtdt, "GTDT", 88);
    check_table(blob, a.off_mcfg, "MCFG", 60);
    check_table(blob, a.off_spcr, "SPCR", 80);
    check_table(blob, a.off_iort, "IORT", 0);
    check_table(blob, a.off_pptt, "PPTT", 0);

    /* ---- FACP: PSCI over HVC, HW-reduced, DSDT where it says ---- */
    const uint8_t *fadt = blob + a.off_fadt;
    CHECK(be32(fadt + 36 + 4) == (uint32_t)(ACPI_PHYS_BASE + a.off_dsdt));
    CHECK(be64(fadt + 36 + 120) == ACPI_PHYS_BASE + a.off_dsdt);
    CHECK(be32(fadt + 36 + 80) & (1u << 20));       /* HW_REDUCED_ACPI */
    CHECK(be16(fadt + 36 + 109) == 0x5);            /* PSCI + HVC, and not smc */
    CHECK(fadt[36 + 111] == 2);                    /* MINOR_VERSION: PSCI 0.2 */

    /* ---- APIC: one GICC per CPU, GICD, GICR, ITS ---- */
    const uint8_t *madt = blob + a.off_madt;
    uint32_t madt_len = (uint32_t)be32(madt + 4);
    CHECK(be32(madt + 40) == 1);                    /* PCIe MSI capable */
    int gicc = 0, gicd = 0, gicr = 0, its = 0;
    const uint8_t *its_node = NULL;
    for (uint32_t o = 44; o < madt_len;) { /* 36 header + 4 + 4 flags */
        uint8_t type = madt[o];
        uint8_t len = madt[o + 1];
        if (len == 0) break;
        if (type == 0x0b) {
            gicc++;
            /* MPIDR and its GICR frame */
            CHECK(be64(madt + o + 68) == (uint64_t)(gicc - 1));
            CHECK(be64(madt + o + 60) == pr.gic_redist + (uint64_t)(gicc - 1) * 0x20000ull);
        }
        if (type == 0x0c) { gicd++; CHECK(be64(madt + o + 8) == pr.gic_dist); }
        if (type == 0x0e) {
            gicr++;
            CHECK(len == 24);
            CHECK(be32(madt + o + 4) == 0);
            CHECK(be64(madt + o + 12) == pr.gic_redist); /* GICR base */
            /* What a real Linux/Windows reader checks: the frame listed for each
               CPU lies inside the range the GICR structure describes. */
            CHECK(be64(madt + o + 12) <= pr.gic_redist &&
                  be64(madt + o + 12) + be32(madt + o + 20) >= pr.gic_redist + (uint64_t)pr.ncpus * 0x20000ull);
            CHECK(be32(madt + o + 20) == (uint32_t)pr.ncpus * 0x20000u); /* range length */
        }
        if (type == 0x0f) {
            its++;
            its_node = madt + o;
            CHECK(len == 20);
            CHECK(be32(madt + o + 4) == pr.gic_its_id);
            CHECK(be32(madt + o + 16) == pr.gic_its_gsi_base);
        }
        o += len;
    }
    CHECK(gicc == pr.ncpus);
    CHECK(gicd == 1);
    CHECK(gicr == 1);
    CHECK(its == 1);
    if (its_node) CHECK(be64(its_node + 8) == pr.gic_its);

    /* ---- GTDT: 30 = non-secure EL1, always-on; the counter frequency ---- */
    const uint8_t *gtdt = blob + a.off_gtdt;
    CHECK(be32(gtdt + 36 + 20) == 30);
    CHECK(be32(gtdt + 36 + 24) & (1u << 2));
    CHECK(be32(gtdt + 36 + 28) == 27);
    CHECK(be64(gtdt + 36 + 0) == 0xffffffffffffffffull);

    /* ---- MCFG: ECAM base and the bus range the ECAM window holds ---- */
    const uint8_t *mcfg = blob + a.off_mcfg;
    CHECK(be64(mcfg + 44) == pr.ecam);
    CHECK(mcfg[54] == 0 && mcfg[55] == 15);

    /* ---- IORT: RC -> ITS group, all RIDs ---- */
    const uint8_t *iort = blob + a.off_iort;
    uint32_t iort_len = (uint32_t)be32(iort + 4);
    CHECK(be32(iort + 36) == 2);                    /* two nodes */
    CHECK(be32(iort + 40) == 12);                   /* first node at +48 */
    const uint8_t *itsg = iort + 48;
    CHECK(itsg[0] == 0);                            /* ITS group */
    CHECK(be16(itsg + 1) == 24);
    CHECK(be32(itsg + 16) == 1);                    /* one ITS */
    CHECK(be32(itsg + 20) == 0);                    /* ITS id 0 */
    const uint8_t *rc = itsg + 24;
    CHECK(rc[0] == 2);                              /* root complex */
    CHECK(be16(rc + 1) == 64);                      /* 44 + one mapping */
    CHECK(be32(rc + 8) == 1);                       /* one mapping */
    const uint8_t *map = rc + 44;
    CHECK(be32(map + 4) == 0x10000);                /* every RID */
    CHECK(be32(map + 12) == 12);                    /* to the ITS group node */
    CHECK(iort_len == 48 + 24 + 64);

    /* ---- SPCR: PL011 at the UART, SPI 1 ---- */
    const uint8_t *spcr = blob + a.off_spcr;
    CHECK(spcr[36] == 14);                          /* ARM PL011 */
    CHECK(be64(spcr + 36 + 8) == pr.uart);
    CHECK(be32(spcr + 36 + 18) == 33);

    /* ---- DSDT: the AML body must at least be there and well-formed enough
       for its nodes to be inside the table ---- */
    const uint8_t *dsdt = blob + a.off_dsdt;
    uint32_t dsdt_len = (uint32_t)be32(dsdt + 4);
    CHECK(dsdt_len > 36);
    CHECK(dsdt[36] == 0x10);                        /* ScopeOp */
    CHECK(dsdt[37] == 0x42 && dsdt[38] == 0x07);    /* its 2-byte length */
    CHECK(memcmp(dsdt + 39, "_SB_", 4) == 0);       /* NameSeg, '_' padded */
    /* Every object PCI0 must have for Windows to bind a PCIe bus to it. */
    for (const char *obj = "_HID"; obj != NULL; obj = NULL) (void)obj;
    {
        static const char *const objs[] = { "_HID", "_CID", "_UID", "_CRS", "_DSM" };
        for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) {
            uint8_t *p = memmem(dsdt + 36, dsdt_len - 36, objs[i], 4);
            CHECK(p != NULL);
        }
        /* and the ECAM window it advertises */
        CHECK(memmem(dsdt + 36, dsdt_len - 36, "\x3f\x00\x00\x00", 4) != NULL);
    }

    /* ---- PPTT ---- */
    const uint8_t *pptt = blob + a.off_pptt;
    CHECK(((uint32_t)be32(pptt + 4) - 40) / 24 == (uint32_t)(1 + pr.ncpus));

    /* Overlap check: no two tables may share bytes. */
    uint32_t offs[8];
    uint32_t lens[8];
    const char *names[8] = { "DSDT", "FACP", "APIC", "GTDT", "MCFG", "SPCR", "IORT", "PPTT" };
    offs[0] = a.off_dsdt; offs[1] = a.off_fadt; offs[2] = a.off_madt; offs[3] = a.off_gtdt;
    offs[4] = a.off_mcfg; offs[5] = a.off_spcr; offs[6] = a.off_iort; offs[7] = a.off_pptt;
    for (int i = 0; i < 8; i++) lens[i] = (uint32_t)be32(blob + offs[i] + 4);
    for (int i = 0; i < 8; i++)
        for (int j = i + 1; j < 8; j++)
            CHECK(offs[i] + lens[i] <= offs[j] || offs[j] + lens[j] <= offs[i]);

    /* A different CPU count must change the APIC (and PPTT) lengths. Keep
       the two-CPU set aside: the second build reuses the same buffer. */
    uint32_t two_madt = (uint32_t)be32(blob + a.off_madt + 4);
    uint32_t two_pptt = (uint32_t)be32(blob + a.off_pptt + 4);
    pr.ncpus = 1;
    HvAcpi b;
    CHECK(hv_acpi_build(blob, ACPI_MAX_SIZE, &pr, &b) == 0);
    CHECK((uint32_t)be32(blob + b.off_madt + 4) + 80 == two_madt);
    CHECK((uint32_t)be32(blob + b.off_pptt + 4) + 24 == two_pptt);
    pr.ncpus = 2;

    /* Too small a buffer is refused, not silently truncated. */
    CHECK(hv_acpi_build(blob, 4096, &pr, &a) != 0);

    /* Rebuild for the dump. */
    (void)hv_acpi_build(blob, ACPI_MAX_SIZE, &pr, &a);

    printf("acpi blob: %u bytes at 0x%llx\n", a.total, (unsigned long long)ACPI_PHYS_BASE);
    printf("  RSDP %+4u  RSDT %+4u  XSDT %+4u\n", a.off_rsdp, a.off_rsdt, a.off_xsdt);
    for (int i = 0; i < 8; i++)
        printf("  %s at %+6u (%u bytes)\n", names[i], offs[i], lens[i]);

    for (int i = 0; i < 8; i++) dump(dir, names[i], blob + offs[i], lens[i]);
    dump(dir, "RSDT", blob + a.off_rsdt, (uint32_t)be32(blob + a.off_rsdt + 4));
    dump(dir, "XSDT", blob + a.off_xsdt, (uint32_t)be32(blob + a.off_xsdt + 4));
    /* iasl wants the DSDT without a header to disassemble it as ASL; give it
       the body too. */
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/DSDT.body.aml", dir);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(blob + a.off_dsdt + 36, 1, lens[0] - 36, f); fclose(f); }
    }

    printf("%d checks, %d failures\n", n, failures);
    free(blob);
    return failures != 0;
}
