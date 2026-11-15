/*
 * ACPI tables for the virt machine.
 *
 * Layout and contents follow Linux's actbl*.h (ACPICA's definitions) and
 * QEMU's hw/arm/virt-acpi-build.c. Each table is assembled in the blob in
 * place and its checksum fixed up at the end. There is no BIOS linker here:
 * every address is written as a physical address inside the reserved region
 * (ACPI_PHYS_BASE), which is what makes the set usable by both UEFI and a
 * running OS.
 */

#include "acpi.h"

#include <string.h>
#include <stdlib.h>

/* ---- sizes the structures below must come out at ------------------------- */
enum {
    HDR_SIZE = 36,             /* common ACPI table header */
    SDT_HDR_SIZE = 36,
    FADT_SIZE = 276,           /* ACPI 6.3, no Hypervisor Vendor Id (as QEMU) */
    MADT_GICC_SIZE = 80,
    MADT_GICD_SIZE = 24,
    MADT_GICR_SIZE = 24,
    MADT_ITS_SIZE = 20,
    GTDT_SIZE = 88,
    MCFG_SIZE = 60,
    SPCR_SIZE = 80,
    IORT_HDR = 48,
    IORT_ITS_GROUP = 20,
    IORT_RC = 44,
    IORT_ID_MAPPING = 20,
    PPTT_PROCESSOR = 24,
};

/* ---- the AML emitter -----------------------------------------------------
 * Henge's tables describe a handful of devices; this is the subset of QEMU's
 * aml-build.c that they need. Everything writes big-endian, like AML. */

typedef struct {
    uint8_t *buf;
    size_t cap;   /* hard limit */
    size_t len;   /* bytes written */
} Aml;

static void aml_u8(Aml *a, uint8_t v)
{
    if (a->len < a->cap) a->buf[a->len] = v;
    a->len++;
}

static void aml_u16(Aml *a, uint16_t v)
{
    aml_u8(a, (uint8_t)(v >> 8));
    aml_u8(a, (uint8_t)v);
}

static void aml_u32(Aml *a, uint32_t v)
{
    aml_u16(a, (uint16_t)(v >> 16));
    aml_u16(a, (uint16_t)v);
}

static void aml_u64(Aml *a, uint64_t v)
{
    aml_u32(a, (uint32_t)(v >> 32));
    aml_u32(a, (uint32_t)v);
}

static void aml_bytes(Aml *a, const void *p, size_t n)
{
    for (size_t i = 0; i < n; i++) aml_u8(a, ((const uint8_t *)p)[i]);
}

/* AML's package length: the top two bits of the first byte say how many more
   bytes follow, and the remainder of that byte holds the count's high bits. */
static void aml_pkg_length(Aml *a, size_t len)
{
    if (len <= 0x3f) {
        aml_u8(a, (uint8_t)len);
    } else if (len <= 0xfff) {
        aml_u8(a, (uint8_t)(0x40 | ((len >> 8) & 0x0f)));
        aml_u8(a, (uint8_t)len);
    } else if (len <= 0xfffff) {
        aml_u8(a, (uint8_t)(0x80 | ((len >> 16) & 0x0f)));
        aml_u8(a, (uint8_t)(len >> 8));
        aml_u8(a, (uint8_t)len);
    } else {
        aml_u8(a, (uint8_t)(0xc0 | ((len >> 24) & 0x0f)));
        aml_u8(a, (uint8_t)(len >> 16));
        aml_u8(a, (uint8_t)(len >> 8));
        aml_u8(a, (uint8_t)len);
    }
}

/* A NameSeg: four characters, '_' padded. */
static void aml_namestring(Aml *a, const char *name)
{
    size_t n = strlen(name);
    for (int i = 0; i < 4; i++) aml_u8(a, (uint8_t)(i < (int)n ? name[i] : '_'));
}

/* Patch a package length that was written as a placeholder. len_at is where
   the length field starts and the package's body ends at a->len. The body
   counts the length field itself, so the field's own size is part of the
   number it holds: try a size, re-encode with it, and keep the size the
   encoding came back with. */
static void aml_fix_pkg_length(Aml *a, size_t len_at)
{
    /* The placeholder is always two bytes, and they are included in the
       length the field holds, so the count is the node's whole span. */
    size_t total = a->len - len_at;
    a->buf[len_at] = (uint8_t)(0x40 | ((total >> 8) & 0x0f));
    a->buf[len_at + 1] = (uint8_t)total;
}

/* Node begin helpers ------------------------------------------------------ */

static void aml_device_begin(Aml *a, const char *name, size_t *len_at)
{
    aml_u8(a, 0x5b); /* ExtOpPrefix */
    aml_u8(a, 0x82); /* DeviceOp */
    aml_namestring(a, name);
    *len_at = a->len;
    aml_u8(a, 0x40); /* 2-byte length placeholder */
    aml_u8(a, 0);    /* (fixed 2 bytes: no node here exceeds 0xfff) */
}

static void aml_scope_begin(Aml *a, const char *name, size_t *len_at)
{
    aml_u8(a, 0x10); /* ScopeOp */
    aml_namestring(a, name);
    *len_at = a->len;
    aml_u8(a, 0x40);
    aml_u8(a, 0);
}

static void aml_method_begin(Aml *a, const char *name, uint8_t argc, size_t *len_at)
{
    aml_u8(a, 0x14); /* MethodOp */
    aml_namestring(a, name);
    aml_u8(a, argc);
    *len_at = a->len;
    aml_u8(a, 0x40);
    aml_u8(a, 0);
}

static void aml_node_end(Aml *a, size_t len_at)
{
    aml_fix_pkg_length(a, len_at);
}

/* Name(_HID, "PNP0A08") style: a Name object holding a string. */
static void aml_name_string(Aml *a, const char *name, const char *value)
{
    aml_u8(a, 0x08); /* NameOp */
    aml_namestring(a, name);
    aml_u8(a, 0x0d); /* StringOp */
    aml_bytes(a, value, strlen(value) + 1);
}

/* Name(_UID, 0) */
static void aml_name_byte(Aml *a, const char *name, uint8_t value)
{
    aml_u8(a, 0x08);
    aml_namestring(a, name);
    aml_u8(a, 0x0a); /* BytePrefix */
    aml_u8(a, value);
}

/* Name(_CRS, ResourceTemplate() { ... }) */
static void aml_name_resource_template_begin(Aml *a, size_t *len_at)
{
    aml_u8(a, 0x08);           /* NameOp */
    aml_namestring(a, "_CRS");
    aml_u8(a, 0x11);           /* BufferOp */
    *len_at = a->len;
    aml_u8(a, 0x40);           /* 2-byte length placeholder */
    aml_u8(a, 0);
}

/* QWordMemory descriptor: address space 0 (memory), flags 0x29 (consumer,
   positive decode, non-cacheable), no translation offset. */
static void aml_qword_memory(Aml *a, uint64_t base, uint64_t end,
                             uint64_t gran, uint64_t len)
{
    aml_u8(a, 0x8a); /* QWordMemory */
    aml_u8(a, 0x00); /* resource type: memory range */
    aml_u8(a, 0x25); /* general flags: consumer, positive decode, non-cacheable */
    aml_u8(a, 0x00); /* type-specific flags */
    aml_u64(a, gran);
    aml_u64(a, base);
    aml_u64(a, end);
    aml_u64(a, len);
}

static void aml_resource_end(Aml *a)
{
    aml_u8(a, 0x79); /* EndTag */
    aml_u8(a, 0x00);
}

/* _DSM: answer the "PCIe capable?" query with bit 0 set, and the revision
   probe with a small positive number. RID mapping comes from the IORT. */
static void aml_device_dsm(Aml *a, size_t *len_at)
{
    aml_method_begin(a, "_DSM", 4, len_at);
    aml_u8(a, 0xa4); /* ReturnOp */
    aml_u8(a, 0x0a); /* BytePrefix */
    aml_u8(a, 0x01); /* 1: yes, PCIe */
}

/* ---- DSDT ---------------------------------------------------------------
 *   Scope (\_SB) {
 *     Device (PCI0) {
 *       Name (_HID, "PNP0A08")      PCIe host bridge
 *       Name (_CID, "PNP0A03")      also a plain PCI bus
 *       Name (_UID, 0)
 *       Name (_CRS, ResourceTemplate { ECAM window })
 *       Method (_DSM, 4, NotSerialized)
 *     }
 *   }
 * Windows enumerates PCIe from MCFG + _CRS and asks _DSM whether the bus
 * speaks PCIe. */

static uint32_t build_dsdt_aml(Aml *a)
{
    /* The DSDT body byte for byte as iasl compiles it from the ASL below.
       Writing it out by hand instead (lengths patched afterwards) produced
       bytes that looked right and that iasl's parser rejected; taking the
       compiler's own output removes that class of mistake. Regenerate with:

         DefinitionBlock ("", "DSDT", 2, "HENGEM", "VIRT0001", 0x00000001)
         {
             Scope (\_SB) {
                 Device (PCI0) {
                     Name (_HID, "PNP0A08")
                     Name (_CID, "PNP0A03")
                     Name (_UID, 0)
                     Name (_CRS, ResourceTemplate () {
                         QWordMemory (ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                                      NonCacheable, ReadWrite,
                                      0, 0x3F000000, 0x3F0FFFFF, 0, 0x00100000)
                     })
                     Method (_DSM, 4, NotSerialized) { Return (0x01) }
                 }
             }
         }

       (iasl -p DSDT DSDT.dsl, then the table header is replaced by ours; the
       command is captured verbatim so the table stays readable to Windows.) */
    static const uint8_t body[] = {
        0x10, 0x42, 0x07, 0x5f, 0x53, 0x42, 0x5f, 0x5b, /* Scope(\_SB){ Device( */
        0x82, 0x4a, 0x06, 0x50, 0x43, 0x49, 0x30,       /* PCI0) { */
        0x08, 0x5f, 0x48, 0x49, 0x44, 0x0d, 0x50, 0x4e, 0x50, 0x30, 0x41, 0x30, 0x38, 0x00,
        0x08, 0x5f, 0x43, 0x49, 0x44, 0x0d, 0x50, 0x4e, 0x50, 0x30, 0x41, 0x30, 0x33, 0x00,
        0x08, 0x5f, 0x55, 0x49, 0x44, 0x00,
        0x08, 0x5f, 0x43, 0x52, 0x53, 0x11, 0x33, 0x0a, 0x30, /* Name(_CRS, Buffer{ */
        0x8a, 0x2b, 0x00, 0x00, 0x0d, 0x01,                   /* QWordMemory */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,       /* granularity */
        0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x00,       /* min: 0x3F000000 */
        0xff, 0xff, 0x0f, 0x3f, 0x00, 0x00, 0x00, 0x00,       /* max: 0x3F0FFFFF */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,       /* translation */
        0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,       /* length: 1 MB */
        0x79, 0x00,                                           /* EndTag */
        0x14, 0x08, 0x5f, 0x44, 0x53, 0x4d, 0x04, 0xa4, 0x01, /* Method(_DSM){Return(1)} */
    };
    memcpy(a->buf, body, sizeof(body));
    a->len = sizeof(body);
    return (uint32_t)sizeof(body);
}

/* ---- common ACPI table header ------------------------------------------- */

static uint8_t sum8(const uint8_t *p, size_t n)
{
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s = (uint8_t)(s + p[i]);
    return s;
}

/* Put a table's checksum byte where it makes the bytes sum to zero. Called
   last, so nothing written afterwards can disturb it. */
static void fix_checksum(uint8_t *p, uint32_t len)
{
    p[9] = 0;
    p[9] = (uint8_t)(0u - sum8(p, len));
}

/* Fill in the header of the table starting at p; length is its total size.
   The checksum byte ends up making the whole table sum to zero. */
static void sdt_header(uint8_t *p, const char *sig, uint32_t len, uint8_t rev,
                       const HvAcpiParams *pr)
{
    memcpy(p + 0, sig, 4);
    p[4] = (uint8_t)(len >> 24);
    p[5] = (uint8_t)(len >> 16);
    p[6] = (uint8_t)(len >> 8);
    p[7] = (uint8_t)len;
    p[8] = rev;
    p[9] = 0;
    memcpy(p + 10, pr->oem_id ? pr->oem_id : "HENGEM", 6);
    memcpy(p + 16, pr->oem_table_id ? pr->oem_table_id : "VIRT0001", 8);
    p[24] = 0;
    p[25] = 0;
    p[26] = 0;
    p[27] = 1; /* oem_revision */
    memcpy(p + 28, "HENG", 4); /* asl_compiler_id */
    p[32] = 0;
    p[33] = 0;
    p[34] = 0;
    p[35] = 1; /* asl_compiler_revision */
    p[9] = 0;
    fix_checksum(p, len);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)(v >> 16));
    put16(p + 2, (uint16_t)v);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}

/* Generic address structure, 12 bytes. space 0 = memory, 1 = I/O, 2 = PCI cfg. */
static void put_gas(uint8_t *p, uint8_t space, uint8_t bits, uint8_t access, uint64_t addr)
{
    p[0] = space;
    p[1] = bits;
    p[2] = 0;
    p[3] = access;
    put64(p + 4, addr);
}

/* ---- FACP --------------------------------------------------------------- */

static uint32_t build_fadt(uint8_t *p, uint64_t dsdt_pa, const HvAcpiParams *pr)
{
    memset(p, 0, FADT_SIZE);
    sdt_header(p, "FACP", FADT_SIZE, 6, pr);

    uint8_t *b = p + HDR_SIZE;
    put32(b + 4, (uint32_t)dsdt_pa);     /* DSDT */
    put32(b + 80, (1u << 20));           /* FLAGS: HW_REDUCED_ACPI */
    /* ARM_BOOT_ARCH at 109..110: bit 0 PSCI compliant, bits 1-2 the conduit
       (0 none, 1 smc, 2 hvc, 3 both). There is no EL3 here, so smc traps to
       nothing: HVC is the only conduit that works, and it is what the DT's
       /psci says. Saying "both" (QEMU's 0x3) would let a guest try smc first
       and hang. MINOR_VERSION follows the DT's /psci version. */
    put16(b + 109, (uint16_t)(1u | (pr->psci_conduit << 1)));
    b[111] = pr->psci_version ? pr->psci_version : 2; /* MINOR_VERSION: PSCI 0.2 */
    put64(b + 120, dsdt_pa);             /* X_DSDT */
    /* Every PM block stays zero: the machine is HW-reduced. */
    fix_checksum(p, FADT_SIZE);
    return FADT_SIZE;
}

/* ---- APIC (MADT) --------------------------------------------------------- */

static uint32_t build_madt(uint8_t *p, const HvAcpiParams *pr)
{
    int ncpus = pr->ncpus > 0 ? pr->ncpus : 1;
    uint32_t len = (uint32_t)(HDR_SIZE + 8
                              + (uint32_t)ncpus * MADT_GICC_SIZE
                              + MADT_GICD_SIZE + MADT_GICR_SIZE + MADT_ITS_SIZE);
    memset(p, 0, len);
    sdt_header(p, "APIC", len, 5, pr);

    uint8_t *b = p + HDR_SIZE;
    put32(b + 0, 0); /* Local Interrupt Controller Address */
    put32(b + 4, 1); /* Flags: PCIe MSI capable */

    uint8_t *q = b + 8;
    /* 5.2.12.15 GIC Distributor Structure: 24 bytes */
    q[0] = 0x0c;
    q[1] = MADT_GICD_SIZE;
    put16(q + 2, 0);
    put32(q + 4, 0);            /* GIC ID */
    put64(q + 8, pr->gic_dist); /* Physical Base Address */
    put32(q + 16, 0);           /* System Vector Base */
    q[20] = 3;                  /* GIC version */
    q = q + MADT_GICD_SIZE;

    /* 5.2.12.14 GIC Structure: 80 bytes, one per CPU */
    for (int i = 0; i < ncpus; i++) {
        q[0] = 0x0b;
        q[1] = MADT_GICC_SIZE;
        put16(q + 2, 0);
        put32(q + 4, (uint32_t)i);  /* GIC ID */
        put32(q + 8, (uint32_t)i);  /* ACPI Processor UID */
        put32(q + 12, 1);           /* Enabled */
        put32(q + 16, 0);           /* Parking Protocol Version */
        put32(q + 20, 0);           /* Performance Interrupt GSIV */
        put64(q + 24, 0);           /* Parked Address */
        put64(q + 32, 0);           /* Physical Base Address (GICv2 only) */
        put64(q + 40, 0);           /* GICV */
        put64(q + 48, 0);           /* GICH */
        put32(q + 56, 0);           /* VGIC Maintenance interrupt */
        put64(q + 60, pr->gic_redist + (uint64_t)i * 0x20000ull); /* GICR */
        put64(q + 68, (uint64_t)i); /* MPIDR affinity (Aff0 = CPU index) */
        q[76] = 0;                  /* Processor Power Efficiency Class */
        /* q[77..79] reserved */
        q += MADT_GICC_SIZE;
    }

    /* 5.2.12.17 GIC Redistributor Structure: 24 bytes with the range fields */
    q[0] = 0x0e;
    q[1] = MADT_GICR_SIZE;
    put16(q + 2, 0);
    put32(q + 4, 0); /* GIC Redistributor ID */
    /* 12 GICR base, 20 discovery range base (0: the same), 20..24 range length.
       ACPI's own structure is 16 bytes; the range fields extend it to 24. */
    put64(q + 12, pr->gic_redist);
    put32(q + 20, (uint32_t)((uint64_t)ncpus * 0x20000ull));
    q += MADT_GICR_SIZE;

    /* 5.2.12.18 GIC ITS Structure: 20 bytes (6.0 errata A length) */
    q[0] = 0x0f;
    q[1] = MADT_ITS_SIZE;
    put16(q + 2, 0);
    put32(q + 4, pr->gic_its_id); /* GIC ITS ID: IORT's ITS node refers to it */
    put64(q + 8, pr->gic_its);    /* Physical Base Address */
    /* 5.2.12.18, ACPI 6.0+: without this field every LPI a device raises is
       below the ITS's GIC ITS Base and the interrupt is discarded (Windows then
       reports its virtio queue as "no interrupt"). QEMU writes the GIC's LPI
       base here; for us that is not 0, because INTIDs below it belong to SPIs. */
    put32(q + 16, pr->gic_its_gsi_base); /* GIC ITS Base */

    fix_checksum(p, len);
    return len;
}

/* ---- GTDT ---------------------------------------------------------------- */

static uint32_t build_gtdt(uint8_t *p, const HvAcpiParams *pr)
{
    memset(p, 0, GTDT_SIZE);
    sdt_header(p, "GTDT", GTDT_SIZE, 3, pr);
    uint8_t *b = p + HDR_SIZE;
    put64(b + 0, 0xffffffffffffffffull); /* CntControlBase: absent */
    put32(b + 8, 0);                     /* reserved */
    put32(b + 12, 13);                   /* Secure EL1 timer GSIV */
    put32(b + 16, 0);                    /* flags */
    put32(b + 20, 30);                   /* Non-Secure EL1 timer GSIV */
    put32(b + 24, 1u << 2);              /* flags: Always-on */
    put32(b + 28, 27);                   /* Virtual timer GSIV */
    put32(b + 32, 0);
    put32(b + 36, 26);                   /* Non-Secure EL2 timer GSIV */
    put32(b + 40, 0);
    put64(b + 44, 0xffffffffffffffffull); /* CntReadBase: absent */
    /* platform timer count / offset: no watchdog */
    fix_checksum(p, GTDT_SIZE);
    return GTDT_SIZE;
}

/* ---- MCFG ---------------------------------------------------------------- */

static uint32_t build_mcfg(uint8_t *p, const HvAcpiParams *pr)
{
    memset(p, 0, MCFG_SIZE);
    sdt_header(p, "MCFG", MCFG_SIZE, 1, pr);
    uint8_t *b = p + HDR_SIZE + 8; /* skip reserved[8] */
    put64(b + 0, pr->ecam);                    /* base address */
    put16(b + 8, 0);                           /* PCI segment group */
    b[10] = (uint8_t)pr->pci_bus_min;          /* start bus */
    b[11] = (uint8_t)pr->pci_bus_max;          /* end bus */
    fix_checksum(p, MCFG_SIZE);
    return MCFG_SIZE;
}

/* ---- SPCR ---------------------------------------------------------------- */

static uint32_t build_spcr(uint8_t *p, const HvAcpiParams *pr)
{
    memset(p, 0, SPCR_SIZE);
    sdt_header(p, "SPCR", SPCR_SIZE, 4, pr);
    uint8_t *b = p + HDR_SIZE;
    b[0] = 14;                          /* interface type: ARM PL011 */
    put_gas(b + 4, 0, 32, 0, pr->uart); /* serial_port: system memory, 32-bit */
    b[16] = 3;                          /* interrupt type: ARM GIC */
    b[17] = 0;                          /* pc_interrupt */
    put32(b + 18, 33);                  /* interrupt: SPI 1 = UART0 */
    b[22] = 7;                          /* baud rate: 115200 */
    b[23] = 0;                          /* parity: none */
    b[24] = 1;                          /* stop bits: 1 */
    b[25] = 0;                          /* flow control: none */
    b[26] = 0;                          /* terminal type: VT100 */
    b[27] = 0;                          /* language */
    fix_checksum(p, SPCR_SIZE);
    return SPCR_SIZE;
}

/* ---- IORT ----------------------------------------------------------------
 * Node 0: ITS group, holding ITS id 0.
 * Node 1: PCIe root complex, mapping every RID (0..0xffff) to node 0.
 * Windows builds its MSI domain from this; without it virtio-blk has no
 * interrupts even though MSI-X works. */

static uint32_t build_iort(uint8_t *p, const HvAcpiParams *pr)
{
    uint32_t rc_len = IORT_RC + IORT_ID_MAPPING;
    uint32_t len = IORT_HDR + IORT_ITS_GROUP + 4 + rc_len;
    memset(p, 0, len);
    sdt_header(p, "IORT", len, 1, pr); /* revision 1: node layout as in 6.x */

    uint8_t *b = p + HDR_SIZE;
    put32(b + 0, 2);                   /* node count */
    put32(b + 4, IORT_HDR - HDR_SIZE); /* offset to the first node */
    put32(b + 8, 0);                   /* reserved */

    uint8_t *its = p + IORT_HDR;
    /* Type 0, ITS group: 20 bytes, then the array of ITS identifiers. */
    its[0] = 0;                        /* type */
    put16(its + 1, (uint16_t)(IORT_ITS_GROUP + 4));
    its[3] = 1;                        /* revision */
    put32(its + 4, 0);                 /* identifier */
    put32(its + 8, 0);                 /* mapping count */
    put32(its + 12, 0);                /* mapping offset */
    put32(its + 16, 1);                /* number of ITSs */
    put32(its + 20, 0);                /* GIC ITS identifier array[0] */

    /* Root complex */
    uint8_t *rc = its + IORT_ITS_GROUP + 4;
    rc[0] = 2;                         /* type */
    put16(rc + 1, (uint16_t)rc_len);
    rc[3] = 3;                         /* revision */
    put32(rc + 4, 1);                  /* identifier */
    put32(rc + 8, 1);                  /* mapping count */
    put32(rc + 12, IORT_RC);           /* offset to the mapping array */
    /* Memory access properties: CCA (u32) | AH (u8) | reserved (u16) |
       flags (u8) = 8 bytes, then ATS (u32), segment (u32), address limit
       (u8), reserved (3 bytes): 16 bytes in all, after the 20-byte node. */
    put32(rc + 16, 1);                 /* CCA: fully coherent */
    rc[20] = 0;                        /* AH: no allocation hints */
    put16(rc + 21, 0);                 /* reserved */
    rc[23] = 0x3;                      /* memory flags: CPM | DACS */
    put32(rc + 24, 0);                 /* ATS attribute: none */
    put32(rc + 28, 0);                 /* MCFG PCI segment */
    rc[32] = 64;                       /* memory address size limit */
    rc[33] = 0;
    rc[34] = 0;
    rc[35] = 0;

    uint8_t *m = rc + IORT_RC;
    put32(m + 0, 0);                   /* input base */
    put32(m + 4, 0x10000);             /* ID count: every RID */
    put32(m + 8, 0);                   /* output base */
    put32(m + 12, IORT_HDR - HDR_SIZE);/* output reference: ITS group node */
    put32(m + 16, 0);                  /* flags: direct, not through an SMMU */

    fix_checksum(p, len);
    return len;
}

/* ---- PPTT ----------------------------------------------------------------
 * Processor hierarchy: a package node with one processor node per CPU. No
 * caches (the guest has none to describe). Windows ignores PPTT; Linux warns
 * and carries on. It is here so the same tables boot both. */

static uint32_t build_pptt(uint8_t *p, const HvAcpiParams *pr)
{
    int ncpus = pr->ncpus > 0 ? pr->ncpus : 1;
    uint32_t len = (uint32_t)(HDR_SIZE + 4 + PPTT_PROCESSOR + (uint32_t)ncpus * PPTT_PROCESSOR);
    memset(p, 0, len);
    sdt_header(p, "PPTT", len, 2, pr);

    uint8_t *b = p + HDR_SIZE;
    put32(b + 0, 0);              /* offset to the first node */
    uint8_t *q = b + 4;
    /* the physical package */
    q[0] = 0;                     /* type */
    q[1] = 1;                     /* revision */
    put16(q + 2, PPTT_PROCESSOR);
    put32(q + 4, 0);              /* flags: physical package */
    put32(q + 8, 0);              /* parent */
    put32(q + 12, 0);             /* ACPI processor ID */
    put32(q + 16, 0);             /* number of private resources */
    q += PPTT_PROCESSOR;
    for (int i = 0; i < ncpus; i++) {
        q[0] = 0;
        q[1] = 1;
        put16(q + 2, PPTT_PROCESSOR);
        put32(q + 4, 1);          /* flags: processor */
        put32(q + 8, 4);          /* parent: the package node */
        put32(q + 12, (uint32_t)i); /* ACPI processor ID */
        put32(q + 16, 0);
        q += PPTT_PROCESSOR;
    }
    fix_checksum(p, len);
    return len;
}

/* ---- RSDP ----------------------------------------------------------------- */

static void build_rsdp(uint8_t *p, uint64_t xsdt_pa, uint64_t rsdt_pa)
{
    memset(p, 0, 36);
    memcpy(p, "RSD PTR ", 8);
    memcpy(p + 9, "HENGEM", 6);
    p[15] = 2;                        /* revision: ACPI 2.0+, XSDT present */
    put32(p + 16, (uint32_t)rsdt_pa); /* the 32-bit RSDT for ACPI 1.0 readers */
    put32(p + 20, 36);                /* length */
    put64(p + 24, xsdt_pa);
    p[8] = 0;
    p[8] = (uint8_t)(0u - sum8(p, 20));  /* the first 20 bytes */
    p[32] = (uint8_t)(0u - sum8(p, 36)); /* the whole thing */
}

/* ---- XSDT / RSDT ---------------------------------------------------------- */

static uint32_t build_root_table(uint8_t *p, const uint64_t *tables, int n,
                                 const HvAcpiParams *pr, int wide)
{
    uint32_t len = (uint32_t)(SDT_HDR_SIZE + (wide ? 8u : 4u) * (uint32_t)n);
    memset(p, 0, len);
    sdt_header(p, wide ? "XSDT" : "RSDT", len, 1, pr);
    for (int i = 0; i < n; i++) {
        if (wide) put64(p + SDT_HDR_SIZE + i * 8, tables[i]);
        else put32(p + SDT_HDR_SIZE + i * 4, (uint32_t)tables[i]);
    }
    fix_checksum(p, len);
    return len;
}

/* ---- the whole set -------------------------------------------------------- */

int hv_acpi_build(uint8_t *buf, size_t buf_size, const HvAcpiParams *pr, HvAcpi *out)
{
    Aml aml;
    uint64_t tables[8];
    uint32_t off;
    int n = 0;

    if (!buf || !out || !pr || buf_size < ACPI_MAX_SIZE) return -1;
    memset(buf, 0, ACPI_MAX_SIZE);
    memset(out, 0, sizeof(*out));

    /* RSDP first, at ACPI_PHYS_BASE; the two root tables right after it;
       then the description tables in the order the XSDT lists them. */
    out->off_rsdp = 0;
    uint32_t rsdt_at = 36;
    uint32_t xsdt_at = rsdt_at + SDT_HDR_SIZE + 8 * 4;
    off = xsdt_at + SDT_HDR_SIZE + 8 * 8;

    /* The DSDT's AML first, then its header in front of it. */
    aml.buf = buf + off + SDT_HDR_SIZE;
    aml.cap = ACPI_MAX_SIZE - off - SDT_HDR_SIZE;
    aml.len = 0;
    uint32_t dsdt_body = build_dsdt_aml(&aml);
    out->off_dsdt = off;
    sdt_header(buf + off, "DSDT", SDT_HDR_SIZE + dsdt_body, 2, pr);
    /* sdt_header writes the checksum last, and the AML that follows must not
       disturb it: rebuild the checksum over the final bytes. */
    fix_checksum(buf + off, SDT_HDR_SIZE + dsdt_body);
    tables[n++] = ACPI_PHYS_BASE + out->off_dsdt;
    off += SDT_HDR_SIZE + dsdt_body;

    out->off_fadt = off;
    off += build_fadt(buf + off, ACPI_PHYS_BASE + out->off_dsdt, pr);
    tables[n++] = ACPI_PHYS_BASE + out->off_fadt;

    out->off_madt = off;
    off += build_madt(buf + off, pr);
    tables[n++] = ACPI_PHYS_BASE + out->off_madt;

    out->off_gtdt = off;
    off += build_gtdt(buf + off, pr);
    tables[n++] = ACPI_PHYS_BASE + out->off_gtdt;

    out->off_mcfg = off;
    off += build_mcfg(buf + off, pr);
    tables[n++] = ACPI_PHYS_BASE + out->off_mcfg;

    out->off_spcr = off;
    off += build_spcr(buf + off, pr);
    tables[n++] = ACPI_PHYS_BASE + out->off_spcr;

    out->off_iort = off;
    off += build_iort(buf + off, pr);
    tables[n++] = ACPI_PHYS_BASE + out->off_iort;

    out->off_pptt = off;
    off += build_pptt(buf + off, pr);
    tables[n++] = ACPI_PHYS_BASE + out->off_pptt;

    out->off_rsdt = rsdt_at;
    out->off_xsdt = xsdt_at;
    build_root_table(buf + rsdt_at, tables, n, pr, 0);
    build_root_table(buf + xsdt_at, tables, n, pr, 1);
    build_rsdp(buf, ACPI_PHYS_BASE + xsdt_at, ACPI_PHYS_BASE + rsdt_at);

    out->total = off;
    out->ncpus = pr->ncpus;
    return 0;
}
