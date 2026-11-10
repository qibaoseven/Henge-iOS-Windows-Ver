#ifndef HENGEVM_FW_CFG_H
#define HENGEVM_FW_CFG_H

#include <stdint.h>

#include "machine.h"

/* QEMU's fw_cfg, the byte-stream half only.

   The guest writes a 16-bit entry selector to +0x08 (big endian, the only
   access the selector takes) and then reads the entry a byte at a time from
   +0x00, also big endian. Reading past the end of an entry gives 0.

   +0x10 (the DMA address register, one access wide, 64 bits) is not
   implemented: a write is recorded and reads back as written, so a firmware
   that probes for DMA sees a register that stores and nothing else. No entry
   here is delivered through it, because the ACPI tables sit at fixed physical
   addresses and arrive by their own entries instead of etc/table-loader
   (see virt.c).

   Entry numbers below 0x20 are QEMU's fixed ones; files live above that. */

#define HV_FWCFG_SIGNATURE 0x0000u
#define HV_FWCFG_ID 0x0001u
#define HV_FWCFG_FILE_DIR 0x0019u /* (count, {size, select, reserved, name[56]}) big endian */
#define HV_FWCFG_FIRST_FILE 0x0020u
#define HV_FWCFG_MAX_FILES 32
#define HV_FWCFG_NAME 56

typedef struct HvFwCfgFile {
    char name[HV_FWCFG_NAME];
    uint8_t *data;
    uint64_t len;
    uint16_t select;
} HvFwCfgFile;

typedef struct HvFwCfg {
    HvMachine *m;
    uint64_t base, size;
    uint16_t sel;
    uint64_t pos;   /* bytes read from the selected entry */
    uint64_t dma;   /* last value written to +0x10 */
    int nfiles;
    HvFwCfgFile file[HV_FWCFG_MAX_FILES];
    uint8_t *dir;   /* the FILE_DIR blob, built when all files are in */
    uint64_t dir_len;
} HvFwCfg;

/* Maps the window (QEMU: base 0x09020000, size 0x18). NULL if the region is taken. */
HvFwCfg *hv_fw_cfg_create(HvMachine *m, uint64_t base);
void hv_fw_cfg_destroy(HvFwCfg *c);

/* Adds a file entry ("etc/acpi/rsdp" and so on), copying the data. -1 if full. */
int hv_fw_cfg_add_file(HvFwCfg *c, const char *name, const void *data, uint64_t len);
/* Builds FILE_DIR from the files added so far. Called by the first read of it. */
void hv_fw_cfg_finalize(HvFwCfg *c);

#endif
