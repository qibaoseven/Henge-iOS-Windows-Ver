/* Smoke test for the new virt platform pieces: pflash, fw_cfg, virt.
   Runs on the host (no JIT needed): it exercises the device callbacks directly
   through the machine's MMIO dispatch, the same path a guest would take. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "machine.h"
#include "virt.h"

static int fails, checks;
#define CHECK(c, ...)                                     \
    do {                                                  \
        checks++;                                         \
        if (!(c)) {                                       \
            fails++;                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

/* As a guest would: through the machine's MMIO map. */
static uint64_t rd(HvMachine *m, uint64_t pa, unsigned size) {
    HvMmio *x = hv_mmio_lookup(m, pa);
    if (!x) return ~0ull;
    return x->read(x->opaque, pa - x->pa, size);
}
static void wr(HvMachine *m, uint64_t pa, uint64_t v, unsigned size) {
    HvMmio *x = hv_mmio_lookup(m, pa);
    if (x) x->write(x->opaque, pa - x->pa, v, size);
}

static void sel(HvMachine *m, uint16_t s) { wr(m, VIRT_FW_CFG + 8, ((uint64_t)(s & 0xff) << 8) | (s >> 8), 2); }

/* Read a whole fw_cfg file by name, the way the firmware does: walk FILE_DIR,
   then select the entry and read its bytes one at a time. Returns the length,
   or 0. */
static uint64_t fw_file(HvMachine *m, const char *name, uint8_t *buf, uint64_t cap) {
    sel(m, HV_FWCFG_FILE_DIR);
    uint32_t count = 0;
    for (int i = 0; i < 4; i++) count = (count << 8) | (uint8_t)rd(m, VIRT_FW_CFG, 1);
    for (uint32_t i = 0; i < count && i < 64; i++) {
        uint8_t ent[64];
        for (int j = 0; j < 64; j++) ent[j] = (uint8_t)rd(m, VIRT_FW_CFG, 1);
        uint32_t size = ((uint32_t)ent[0] << 24) | ((uint32_t)ent[1] << 16) | ((uint32_t)ent[2] << 8) | ent[3];
        uint16_t s = (uint16_t)((ent[4] << 8) | ent[5]);
        if (strncmp((const char *)ent + 8, name, HV_FWCFG_NAME)) continue;
        sel(m, s);
        uint64_t n = size < cap ? size : cap;
        for (uint64_t k = 0; k < size; k++) {
            uint8_t b = (uint8_t)rd(m, VIRT_FW_CFG, 1);
            if (k < n) buf[k] = b;
        }
        return size;
    }
    return 0;
}

static uint32_t be32r(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t be64r(const uint8_t *p) { return ((uint64_t)be32r(p) << 32) | be32r(p + 4); }

int main(void) {
    HvMachine *m = calloc(1, sizeof *m);
    if (hv_machine_init(m, 8u << 20) != 0) return 1;

    /* --- the map is exactly QEMU's --- */
    CHECK(VIRT_FLASH_BASE == 0 && VIRT_FLASH_SIZE == 0x08000000, "flash");
    CHECK(VIRT_GIC_DIST == 0x08000000 && VIRT_GIC_ITS == 0x08080000, "gic");
    CHECK(VIRT_GIC_REDIST == 0x080A0000, "redist");
    CHECK(VIRT_UART0 == 0x09000000 && VIRT_RTC == 0x09010000, "serial/rtc");
    CHECK(VIRT_FW_CFG == 0x09020000 && VIRT_GPIO == 0x09030000, "fw_cfg/gpio");
    CHECK(VIRT_PCIE_MMIO == 0x10000000 && VIRT_PCIE_ECAM == 0x3F000000, "pcie");

    /* --- pflash --- */
    char code[] = "/tmp/vt_code.bin", vars[] = "/tmp/vt_vars.bin";
    FILE *f = fopen(code, "wb");
    const unsigned char image[] = {0x00, 0x00, 0x02, 0x58, 0xAA, 0x55}; /* looks like code, not important */
    fwrite(image, 1, sizeof image, f);
    fclose(f);
    f = fopen(vars, "wb");
    fputc(0x11, f);
    fclose(f);

    HvMachine *m2 = calloc(1, sizeof *m2);
    if (hv_machine_init(m2, 8u << 20) != 0) return 1;
    HvPflash *pf = hv_pflash_create(m2, 0x0, 0x4000000, 0x4000000, code, vars);
    CHECK(pf != NULL, "pflash create");
    /* Flash is RAM to the engine, because the JIT only fetches from RAM. */
    HvRam *cr = hv_ram_lookup(m2, 0x0), *vr = hv_ram_lookup(m2, 0x4000000);
    CHECK(cr && cr->readonly == 1, "code bank is read-only RAM");
    CHECK(vr && vr->readonly == 0, "vars bank is writable RAM");
    CHECK(cr->size == 0x4000000 && vr->size == 0x4000000, "each bank 64 MB");
    /* It is NOT an MMIO device: an MMIO window there would stop the firmware
       from executing at all. */
    CHECK(hv_mmio_lookup(m2, 0x0) == NULL, "flash is not behind an MMIO window");
    /* The image is in the RAM the engine fetches from. */
    CHECK(cr->host[0] == 0x00 && cr->host[3] == 0x58, "code image in RAM");
    CHECK(cr->host[0x2000] == 0xff, "erased past the image");
    CHECK(vr->host[0] == 0x11, "vars bank loaded");
    /* Code bank writes are refused by the engine (read-only RAM). */
    hv_phys_write(m2, 0x2000, "\x42", 1);
    CHECK(cr->host[0x2000] == 0xff, "code bank refused a write");
    hv_phys_write(m2, 0x4000010, "\x42", 1);
    CHECK(vr->host[0x10] == 0x42, "vars bank accepted a write");
    CHECK(hv_pflash_save_vars(pf, vars) == 0, "save vars");
    f = fopen(vars, "rb");
    fseek(f, 0x10, SEEK_SET);
    int got = fgetc(f);
    fclose(f);
    CHECK(got == 0x42, "vars written to disk: got %02x", got);
    hv_pflash_destroy(pf);

    /* --- fw_cfg --- */
    HvFwCfg *fc = hv_fw_cfg_create(m2, VIRT_FW_CFG);
    CHECK(fc != NULL, "fw_cfg create");
    sel(m2, HV_FWCFG_SIGNATURE);
    CHECK(rd(m2, VIRT_FW_CFG, 1) == 'Q' && rd(m2, VIRT_FW_CFG, 1) == 'E' && rd(m2, VIRT_FW_CFG, 1) == 'M' &&
              rd(m2, VIRT_FW_CFG, 1) == 'U',
          "signature reads QEMU");
    CHECK(rd(m2, VIRT_FW_CFG + 8, 2) == 0, "selector reads 0");
    /* re-selecting the same entry restarts it */
    sel(m2, HV_FWCFG_SIGNATURE);
    CHECK(rd(m2, VIRT_FW_CFG, 1) == 'Q', "reselect restarts");

    const char payload[] = "etc/acpi payload!";
    CHECK(hv_fw_cfg_add_file(fc, "etc/acpi/rsdp", payload, sizeof payload - 1) == 0, "add file");
    uint16_t s = fc->file[0].select;
    CHECK(s == HV_FWCFG_FIRST_FILE, "first file at 0x20");
    sel(m2, s);
    for (size_t i = 0; i < sizeof payload - 1; i++)
        CHECK(rd(m2, VIRT_FW_CFG, 1) == (uint8_t)payload[i], "file byte %zu", i);
    CHECK(rd(m2, VIRT_FW_CFG, 1) == 0, "past the end reads 0");

    /* FILE_DIR */
    hv_fw_cfg_finalize(fc);
    sel(m2, HV_FWCFG_FILE_DIR);
    uint64_t count = ((uint64_t)rd(m2, VIRT_FW_CFG, 1) << 24) | ((uint64_t)rd(m2, VIRT_FW_CFG, 1) << 16) |
                     ((uint64_t)rd(m2, VIRT_FW_CFG, 1) << 8) | (uint64_t)rd(m2, VIRT_FW_CFG, 1);
    CHECK(count == 1, "file dir count %llu", (unsigned long long)count);
    uint64_t sz = ((uint64_t)rd(m2, VIRT_FW_CFG, 1) << 24) | ((uint64_t)rd(m2, VIRT_FW_CFG, 1) << 16) |
                  ((uint64_t)rd(m2, VIRT_FW_CFG, 1) << 8) | (uint64_t)rd(m2, VIRT_FW_CFG, 1);
    CHECK(sz == sizeof payload - 1, "file dir size");
    uint64_t selbe = ((uint64_t)rd(m2, VIRT_FW_CFG, 1) << 8) | (uint64_t)rd(m2, VIRT_FW_CFG, 1);
    CHECK(selbe == HV_FWCFG_FIRST_FILE, "file dir select");
    rd(m2, VIRT_FW_CFG, 1);
    rd(m2, VIRT_FW_CFG, 1); /* reserved */
    char name[13];
    for (int i = 0; i < 13; i++) name[i] = (char)rd(m2, VIRT_FW_CFG, 1);
    CHECK(memcmp(name, "etc/acpi/rsdp", 13) == 0, "file dir name");

    /* the DMA register stores and nothing more */
    wr(m2, VIRT_FW_CFG + 16, 0x1234, 8);
    CHECK(rd(m2, VIRT_FW_CFG + 16, 8) == 0x1234, "dma register records");
    CHECK(fc->dir_len == 4 + 64, "dir length");

    hv_fw_cfg_destroy(fc);
    hv_machine_free(m2);
    free(m2);
    unlink(code);
    unlink(vars);

    /* --- virt, end to end --- */
    HvMachine *m3 = calloc(1, sizeof *m3);
    if (hv_machine_init(m3, 8u << 20) != 0) return 1;
    HvCpu cpus[4];
    memset(cpus, 0, sizeof cpus);
    HvVirt v;
    f = fopen("/tmp/vt_code2.bin", "wb");
    fputc(0x00, f);
    fclose(f);
    f = fopen("/tmp/vt_vars2.bin", "wb");
    fputc(0xff, f);
    fclose(f);
    int ok = hv_virt_create(&v, m3, cpus, 4, 512u << 20, "/tmp/vt_code2.bin", "/tmp/vt_vars2.bin");
    CHECK(ok == 0, "virt create (%d)", ok);
    CHECK(m3->nram >= 1 && m3->ram[0].pa == VIRT_RAM_BASE, "guest RAM at %llx",
          (unsigned long long)(m3->main_ram ? m3->main_ram->pa : 0));
    hv_virt_reset_cpu(&v);
    CHECK(cpus[0].pc == 0x0, "cpu0 PC at the flash entry, got %llx", (unsigned long long)cpus[0].pc);
    CHECK(cpus[0].off == 0, "cpu0 running");
    CHECK(cpus[1].off == 1, "cpu1 parked for PSCI CPU_ON");
    CHECK(cpus[0].index == 0 && cpus[1].mpidr == 1, "mpidr");
    CHECK(m3->tick != NULL, "tick installed");
    /* the UART is where QEMU puts it */
    HvMmio *u = hv_mmio_lookup(m3, VIRT_UART0);
    CHECK(u && strcmp(u->name, "pl011") == 0, "pl011 at 0x09000000");
    /* The ITS is there, at QEMU's address, and is the GIC's MSI path. */
    HvMmio *its = hv_mmio_lookup(m3, VIRT_GIC_ITS);
    CHECK(its && strcmp(its->name, "gicv3-its") == 0, "ITS at 0x08080000");
    CHECK(its && its->size == VIRT_GIC_ITS_SIZE, "ITS window size");
    /* Its TYPER tells the guest its table entry size and LPI count: read through
       the window, as a driver would. */
    {
        uint64_t typer = its->read(its->opaque, 0x8, 8);
        CHECK((typer & 1) == 1, "physical LPIs");
        CHECK(((typer >> 8) & 0xff) == 7, "8-byte ITT entries");
        CHECK(((typer >> 24) & 0x3ff) == HV_ITS_LPI_COUNT - 1, "LPI count in TYPER");
        uint64_t ctlr = its->read(its->opaque, 0x0, 4);
        CHECK(ctlr == 0, "an ITS starts disabled");
    }
    hv_virt_destroy(&v);
    hv_machine_free(m3);
    free(m3);
    unlink("/tmp/vt_code2.bin");
    unlink("/tmp/vt_vars2.bin");
    hv_machine_free(m);
    free(m);


    /* --- RAM above 4 GiB, with the ACPI window held out ---
       A 512 MiB guest does not reach the tables; a 2 GiB one does, and must
       continue above them rather than through them. */
    {
        HvMachine *m4 = calloc(1, sizeof *m4);
        if (hv_machine_init(m4, 8u << 20) != 0) return 1;
        HvCpu c4[4];
        HvVirt v4;
        memset(&v4, 0, sizeof v4);
        /* A 1 MiB image, so the PCI side has a device to enumerate. */
        {
            FILE *img = fopen("/tmp/vt_disk.bin", "wb");
            fseek(img, (long)(1u << 20) - 1, SEEK_SET);
            fputc(0, img);
            fclose(img);
        }
        v4.disk_path = "/tmp/vt_disk.bin";
        FILE *g2 = fopen("/tmp/vt_code3.bin", "wb");
        fputc(0x00, g2);
        fclose(g2);
        g2 = fopen("/tmp/vt_vars3.bin", "wb");
        fputc(0xff, g2);
        fclose(g2);
        int ok4 = hv_virt_create(&v4, m4, c4, 4, 2ull << 30, "/tmp/vt_code3.bin", "/tmp/vt_vars3.bin");
        CHECK(ok4 == 0, "virt create with 2 GiB (%d)", ok4);
        HvRam *lo = hv_ram_lookup(m4, VIRT_RAM_BASE);
        HvRam *hi = hv_ram_lookup(m4, VIRT_RAM_HIGH);
        CHECK(lo && lo->size == VIRT_ACPI_BASE - VIRT_RAM_BASE, "low RAM stops at the ACPI window");
        CHECK(lo && lo->pa + lo->size == VIRT_ACPI_BASE, "low RAM end");
        {
            HvRam *at = hv_ram_lookup(m4, VIRT_ACPI_BASE);
            CHECK(at == NULL || at->pa != VIRT_ACPI_BASE, "no RAM starts at 0x48000000");
        }
        CHECK(hi && hi->pa == VIRT_RAM_HIGH, "high RAM at 4 GiB");
        CHECK(hi && lo && lo->size + hi->size == (2ull << 30), "the two halves are the whole guest RAM");

        /* The tables the firmware will read, fetched as it fetches them. */
        uint8_t rsdp[64], madt[512], fadt[512], iort[512], xsdt[256], mcfg[128];
        uint64_t rlen = fw_file(m4, "etc/acpi/rsdp", rsdp, sizeof rsdp);
        CHECK(rlen == 36, "rsdp entry length %llu", (unsigned long long)rlen);
        CHECK(memcmp(rsdp, "RSD PTR ", 8) == 0, "rsdp signature");
        CHECK(be64r(rsdp + 24) == VIRT_ACPI_BASE + v4.acpi.off_xsdt, "rsdp -> xsdt");
        uint64_t xlen = fw_file(m4, "etc/acpi/xsdt", xsdt, sizeof xsdt);
        CHECK(xlen >= 36 + 8 * 8, "xsdt holds every table");
        CHECK(memcmp(xsdt, "XSDT", 4) == 0, "xsdt signature");

        uint64_t flen = fw_file(m4, "etc/acpi/facp", fadt, sizeof fadt);
        CHECK(flen == 276, "fadt length %llu", (unsigned long long)flen);
        CHECK(be32r(fadt + 36 + 80) & (1u << 20), "HW_REDUCED_ACPI");
        CHECK(((fadt[36 + 109] << 8) | fadt[36 + 110]) == 0x5, "PSCI via HVC only, got %#x",
              (fadt[36 + 109] << 8) | fadt[36 + 110]);

        uint64_t alen = fw_file(m4, "etc/acpi/apic", madt, sizeof madt);
        CHECK(alen == 44 + 4 * 80 + 24 + 24 + 20, "madt length %llu", (unsigned long long)alen);
        /* every CPU has a GICC that names its own redistributor frame */
        int gicc = 0, its_found = 0;
        for (uint32_t o = 44; o + 2 <= alen;) {
            uint8_t type = madt[o], len = madt[o + 1];
            if (len < 2) break;
            if (type == 0x0b && len == 80) {
                CHECK(be64r(madt + o + 60) == VIRT_GIC_REDIST + (uint64_t)gicc * 0x20000ull, "gicc %d GICR", gicc);
                CHECK(be32r(madt + o + 8) == (uint32_t)gicc, "gicc %d UID", gicc);
                gicc++;
            }
            if (type == 0x0f) {
                its_found = 1;
                CHECK(len == 20, "ITS structure length %u", len);
                CHECK(be32r(madt + o + 4) == VIRT_GIC_ITS_ID, "ITS id");
                CHECK(be64r(madt + o + 8) == VIRT_GIC_ITS, "ITS address");
                CHECK(be32r(madt + o + 16) == VIRT_GIC_LPI_BASE, "ITS GSI base");
            }
            o += len;
        }
        CHECK(gicc == 4, "one GICC per CPU (%d)", gicc);
        CHECK(its_found, "MADT names the ITS");

        uint64_t ilen = fw_file(m4, "etc/acpi/iort", iort, sizeof iort);
        CHECK(ilen >= 48 && memcmp(iort, "IORT", 4) == 0, "iort (%llu)", (unsigned long long)ilen);
        uint64_t mlen = fw_file(m4, "etc/acpi/mcfg", mcfg, sizeof mcfg);
        CHECK(mlen >= 60 && memcmp(mcfg, "MCFG", 4) == 0, "mcfg");
        CHECK(be64r(mcfg + 44) == VIRT_PCIE_ECAM, "mcfg ECAM base");

        /* Two devices on the bridge, both modern virtio 1.0 and both with MSI-X,
           which is the only interrupt transport Windows' virtio drivers take.
           Read them out of configuration space as the firmware would. */
        HvPciDev *fn1 = NULL, *fn2 = NULL;
        int ndev = 0;
        for (int i = 0; i < HV_PCI_MAX_DEV; i++) {
            /* s->pcie.dev[] also holds the host bridge itself: skip it (dev 0). */
            if (!v4.pci->dev[i] || (i >> 3) == 0) continue;
            if (!fn1) fn1 = v4.pci->dev[i];
            else if (!fn2) fn2 = v4.pci->dev[i];
            ndev++;
        }
        CHECK(ndev == 2, "two functions on the bridge (%d)", ndev);
        if (fn1 && fn2) {
            uint16_t ven1 = (uint16_t)(fn1->config[0] | (fn1->config[1] << 8));
            uint16_t dev1 = (uint16_t)(fn1->config[2] | (fn1->config[3] << 8));
            uint16_t ven2 = (uint16_t)(fn2->config[0] | (fn2->config[1] << 8));
            uint16_t dev2 = (uint16_t)(fn2->config[2] | (fn2->config[3] << 8));
            CHECK(ven1 == 0x1af4 && dev1 == 0x1042, "virtio-blk at dev1: %04x:%04x", ven1, dev1);
            CHECK(ven2 == 0x1af4 && dev2 == 0x1041, "virtio-net at dev2: %04x:%04x", ven2, dev2);
            int msix = 0;
            for (uint8_t c = fn1->config[0x34]; c && c < 0x100; c = fn1->config[c + 1])
                if ((fn1->config[c] & 0x7f) == 0x11) msix = 1;
            CHECK(msix, "virtio-blk advertises MSI-X");
            CHECK(fn1->msix_vectors >= 2 && fn2->msix_vectors >= 2, "a vector per queue");
        }
        CHECK(v4.net != NULL, "the NIC exists");
        CHECK(v4.blk != NULL, "the disk was attached");
        if (v4.blk) CHECK((v4.blk->vio.config[0] | (v4.blk->vio.config[1] << 8)) == 2048, "capacity in 512-byte sectors");
        hv_virt_destroy(&v4);
        hv_machine_free(m4);
        free(m4);
        unlink("/tmp/vt_code3.bin");
        unlink("/tmp/vt_vars3.bin");
        unlink("/tmp/vt_disk.bin");
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
