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
    CHECK(rd(m2, 0x0, 4) == 0x58020000u, "code bank first word (LE as the guest sees it): got %llx",
          (unsigned long long)rd(m2, 0, 4));
    CHECK(rd(m2, 0x4000000, 1) == 0x11, "vars bank loaded");
    /* past the end of what the file filled: erased flash */
    CHECK(rd(m2, 0x2000, 1) == 0xff, "erased past the image");
    /* the code bank ignores writes, in the image and past it alike */
    wr(m2, 0x2000, 0x42, 1);
    CHECK(rd(m2, 0x2000, 1) == 0xff, "code bank kept erased bytes erased");
    wr(m2, 0x4, 0x42, 1);
    CHECK(rd(m2, 0x4, 1) == 0xAA, "code bank image unchanged by a write");
    /* the vars bank takes them */
    wr(m2, 0x4000010, 0x42, 1);
    CHECK(rd(m2, 0x4000010, 1) == 0x42, "vars bank writable");
    /* and they survive a save/reload */
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
    CHECK(m3->nram >= 1 && m3->main_ram->pa == VIRT_RAM_BASE, "guest RAM at %llx",
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
    /* the ITS window is unmapped on purpose, and stays so until it is built */
    CHECK(hv_mmio_lookup(m3, VIRT_GIC_ITS) == NULL, "ITS not faked");
    hv_virt_destroy(&v);
    hv_machine_free(m3);
    free(m3);
    unlink("/tmp/vt_code2.bin");
    unlink("/tmp/vt_vars2.bin");
    hv_machine_free(m);
    free(m);

    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
