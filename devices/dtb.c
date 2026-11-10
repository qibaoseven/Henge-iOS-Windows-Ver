#include "dtb.h"

#include <stdio.h>
#include <string.h>

#include "virt.h"

/* The FDT format: a header, a memory reservation map, a structure block of
   tokens and a string table. All big endian, 4-byte aligned.
     FDT_BEGIN_NODE (name, NUL terminated)  FDT_PROP (len, nameoff, value)
     FDT_END_NODE  FDT_END

   Properties are written before the string table exists, so a property's name
   offset is filled in afterwards (see close()). The names are therefore recorded
   on the way, in the order they are first used: that is what the table is. */

#define FDT_MAGIC 0xd00dfeedu
#define FDT_BEGIN_NODE 1u
#define FDT_END_NODE 2u
#define FDT_PROP 3u
#define FDT_END 9u

#define AT_RSVMAP 0x40u
#define AT_STRUCT 0x1000u
#define AT_STRINGS 0x10000u /* the structure block has 60 KB; this tree needs 2 */
#define AT_END (AT_STRINGS + 0x2000u)

#define MAX_NAMES 64

typedef struct {
    uint8_t *buf;
    uint64_t pos;
    const char *name[MAX_NAMES];
    uint32_t nameoff_at[256]; /* where in the structure block each nameoff field is */
    uint8_t pid[256];         /* which name that field must hold */
    int npatches;
    int nnames;
    int overflow;
} Fdt;

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

static void align(Fdt *f) {
    while (f->pos & 3) f->buf[f->pos++] = 0;
}

static void token(Fdt *f, uint32_t t) {
    if (f->pos + 4 > AT_STRINGS) {
        f->overflow = 1;
        return;
    }
    put32(f->buf + f->pos, t);
    f->pos += 4;
}

static void begin(Fdt *f, const char *name) {
    token(f, FDT_BEGIN_NODE);
    size_t n = strlen(name) + 1;
    if (f->pos + n > AT_STRINGS) {
        f->overflow = 1;
        return;
    }
    memcpy(f->buf + f->pos, name, n); /* the root's name is the empty string */
    f->pos += n;
    align(f);
}

static void end_node(Fdt *f) { token(f, FDT_END_NODE); }

/* Where a name sits in the table, adding it the first time it is asked for. */
/* The name's index in the table (registering it on first use), or -1. */
static int name_id(Fdt *f, const char *name) {
    for (int i = 0; i < f->nnames; i++)
        if (!strcmp(f->name[i], name)) return i;
    if (f->nnames == MAX_NAMES) return -1;
    f->name[f->nnames] = name;
    return f->nnames++;
}

static void prop(Fdt *f, const char *name, const void *val, uint32_t len) {
    int id = name_id(f, name);
    if (id < 0) {
        f->overflow = 1;
        return;
    }
    token(f, FDT_PROP);
    if (f->pos + 8 + len > AT_STRINGS) {
        f->overflow = 1;
        return;
    }
    put32(f->buf + f->pos, len);
    if (f->npatches < MAX_NAMES) {
        f->pid[f->npatches] = (uint8_t)id; /* which name this property uses */
        f->nameoff_at[f->npatches] = (uint32_t)f->pos + 4;
        f->npatches++;
    } else {
        f->overflow = 1;
    }
    f->pos += 8;
    if (len) memcpy(f->buf + f->pos, val, len);
    f->pos += len;
    align(f);
}

static void prop_str(Fdt *f, const char *name, const char *v) { prop(f, name, v, (uint32_t)strlen(v) + 1); }

static void prop_u32(Fdt *f, const char *name, uint32_t v) {
    uint8_t b[4];
    put32(b, v);
    prop(f, name, b, 4);
}

static void prop_cells(Fdt *f, const char *name, const uint32_t *v, int n) {
    uint8_t b[80];
    for (int i = 0; i < n; i++) put32(b + i * 4, v[i]);
    prop(f, name, b, (uint32_t)n * 4);
}

/* <base size>, two cells each (#address-cells = 2, #size-cells = 2). */
static void prop_reg(Fdt *f, uint64_t base, uint64_t size) {
    uint32_t c[4] = {(uint32_t)(base >> 32), (uint32_t)base, (uint32_t)(size >> 32), (uint32_t)size};
    prop_cells(f, "reg", c, 4);
}

static uint64_t close_tree(Fdt *f) {
    if (f->overflow) return 0;
    uint64_t struct_end = f->pos;
    token(f, FDT_END);
    if (f->overflow) return 0;
    uint64_t struct_len = f->pos - AT_STRUCT;

    /* The string table, in the order the names were registered. */
    uint64_t str_used = 0;
    for (int i = 0; i < f->nnames; i++) {
        size_t n = strlen(f->name[i]) + 1;
        if (str_used + n > AT_END - AT_STRINGS) return 0;
        memcpy(f->buf + AT_STRINGS + str_used, f->name[i], n);
        str_used += n;
    }
    /* Patch each property's name offset now that the offsets are known. */
    uint32_t pos_of[MAX_NAMES];
    uint32_t off = 0;
    for (int i = 0; i < f->nnames; i++) {
        pos_of[i] = off;
        off += (uint32_t)strlen(f->name[i]) + 1;
    }
    for (int i = 0; i < f->npatches; i++) put32(f->buf + f->nameoff_at[i], pos_of[f->pid[i]]);

    uint64_t total = AT_STRINGS + str_used;
    /* Header. */
    put32(f->buf + 0x00, FDT_MAGIC);
    put32(f->buf + 0x04, (uint32_t)total);
    put32(f->buf + 0x08, AT_STRUCT);
    put32(f->buf + 0x0c, AT_STRINGS);
    put32(f->buf + 0x10, AT_RSVMAP);
    put32(f->buf + 0x14, 17);                    /* version */
    put32(f->buf + 0x18, 16);                    /* last_comp_version */
    put32(f->buf + 0x1c, 0);                     /* boot_cpuid_phys */
    put32(f->buf + 0x20, (uint32_t)str_used);    /* size_dt_strings */
    put32(f->buf + 0x24, (uint32_t)struct_len);  /* size_dt_struct */
    /* The reservation map: one entry of zeros, which ends it. (buf is zeroed by the caller.) */
    (void)struct_end;
    return total;
}

static void build(Fdt *f, uint64_t ram_base, uint64_t ram_size, uint32_t ncpus) {
    char node[64];
    begin(f, ""); /* root */
    prop_u32(f, "#address-cells", 2);
    prop_u32(f, "#size-cells", 2);
    prop_str(f, "compatible", "linux,dummy-virt");
    prop_str(f, "model", "linux,dummy-virt");

    begin(f, "chosen");
    prop_str(f, "stdout-path", "/pl011@9000000");
    end_node(f);

    begin(f, "aliases");
    prop_str(f, "serial0", "/pl011@9000000");
    end_node(f);

    snprintf(node, sizeof node, "memory@%llx", (unsigned long long)ram_base);
    begin(f, node);
    prop_str(f, "device_type", "memory");
    prop_reg(f, ram_base, ram_size);
    end_node(f);

    begin(f, "cpus");
    prop_u32(f, "#address-cells", 1);
    prop_u32(f, "#size-cells", 0);
    for (uint32_t i = 0; i < ncpus; i++) {
        snprintf(node, sizeof node, "cpu@%x", i);
        begin(f, node);
        prop_str(f, "device_type", "cpu");
        prop_str(f, "compatible", "arm,armv8");
        prop_u32(f, "reg", i);
        prop_str(f, "enable-method", "psci");
        end_node(f);
    }
    end_node(f);

    /* The GIC: distributor then redistributor. No ITS node, because there is no
       ITS: naming one would send the firmware looking for a device that is not
       there. It joins the tree with the ITS itself. */
    snprintf(node, sizeof node, "intc@%llx", (unsigned long long)VIRT_GIC_DIST);
    begin(f, node);
    prop_str(f, "compatible", "arm,gic-v3");
    prop_str(f, "interrupt-controller", "");
    prop_u32(f, "#interrupt-cells", 3);
    prop_u32(f, "phandle", 1);
    prop_u32(f, "#address-cells", 0);
    {
        /* Two <base size> pairs, two cells each, as the root's #address-cells = 2
           demands: QEMU writes the GIC's reg that way too. */
        uint32_t c[8] = {0, (uint32_t)VIRT_GIC_DIST, 0, (uint32_t)VIRT_GIC_DIST_SIZE,
                         0, (uint32_t)VIRT_GIC_REDIST, 0, (uint32_t)VIRT_GIC_REDIST_SIZE};
        prop_cells(f, "reg", c, 8);
    }
    end_node(f);

    snprintf(node, sizeof node, "pl011@%llx", (unsigned long long)VIRT_UART0);
    begin(f, node);
    prop_str(f, "compatible", "arm,pl011");
    prop_reg(f, VIRT_UART0, 0x1000);
    prop_u32(f, "interrupt-parent", 1); /* the GIC's phandle */
    prop_cells(f, "interrupts", (uint32_t[]){0, 1, 4}, 3);
    end_node(f);

    snprintf(node, sizeof node, "pl031@%llx", (unsigned long long)VIRT_RTC);
    begin(f, node);
    prop_str(f, "compatible", "arm,pl031");
    prop_reg(f, VIRT_RTC, 0x1000);
    prop_u32(f, "interrupt-parent", 1);
    prop_cells(f, "interrupts", (uint32_t[]){0, 2, 4}, 3);
    end_node(f);

    snprintf(node, sizeof node, "pl061@%llx", (unsigned long long)VIRT_GPIO);
    begin(f, node);
    prop_str(f, "compatible", "arm,pl061");
    prop_reg(f, VIRT_GPIO, 0x1000);
    prop_u32(f, "interrupt-parent", 1);
    prop_cells(f, "interrupts", (uint32_t[]){0, 7, 4}, 3);
    end_node(f);

    snprintf(node, sizeof node, "fw-cfg@%llx", (unsigned long long)VIRT_FW_CFG);
    begin(f, node);
    prop_str(f, "compatible", "qemu,fw-cfg-mmio");
    prop_reg(f, VIRT_FW_CFG, 0x18);
    prop_str(f, "dma-coherent", "");
    end_node(f);

    snprintf(node, sizeof node, "pcie@%llx", (unsigned long long)VIRT_PCIE_ECAM);
    begin(f, node);
    prop_str(f, "compatible", "pci-host-ecam-generic");
    prop_str(f, "device_type", "pci");
    prop_cells(f, "bus-range", (uint32_t[]){0, 255}, 2);
    prop_u32(f, "#address-cells", 3);
    prop_u32(f, "#size-cells", 2);
    {
        uint32_t c[4] = {(uint32_t)(VIRT_PCIE_ECAM >> 32), (uint32_t)VIRT_PCIE_ECAM,
                         (uint32_t)(VIRT_PCIE_ECAM_SIZE >> 32), (uint32_t)VIRT_PCIE_ECAM_SIZE};
        prop_cells(f, "reg", c, 4);
    }
    {
        /* One range: 32-bit MMIO. child = 3 address cells + 2 size cells,
           parent = 2 address cells (the root's) + 2 size cells: seven words. */
        uint32_t rg[7] = {0x02000000, 0, 0, (uint32_t)VIRT_PCIE_MMIO, 0, (uint32_t)VIRT_PCIE_MMIO,
                          (uint32_t)VIRT_PCIE_MMIO_SIZE};
        prop_cells(f, "ranges", rg, 7);
    }
    end_node(f);

    /* PSCI over HVC: there is no EL3 to take an SMC, and a firmware told to call
       one would stop at an exception level that does not exist without saying
       why. */
    begin(f, "psci");
    prop_str(f, "compatible", "arm,psci-1.0");
    prop_str(f, "method", "hvc");
    end_node(f);

    end_node(f); /* root */
}

uint64_t hv_dtb_build(uint8_t *buf, uint64_t cap, uint64_t ram_base, uint64_t ram_size, uint32_t ncpus) {
    if (!buf || cap < AT_END) return 0;
    memset(buf, 0, (size_t)AT_END); /* the reservation map and all padding */
    Fdt f = {.buf = buf, .pos = AT_STRUCT};
    build(&f, ram_base, ram_size, ncpus);
    return close_tree(&f);
}
