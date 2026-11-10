#ifndef HENGEVM_PFLASH_H
#define HENGEVM_PFLASH_H

#include <stdint.h>

#include "machine.h"

/* The two nor-flash banks of QEMU's virt: as in QEMU, the first is mapped
   read-only (firmware code) and holds UEFI's code part, the second is writable
   and holds its variables, which UEFI writes through the same window.
   Henge backs each bank with host memory of its own, so a write to the
   read-only bank is ignored rather than faulting: UEFI probes for it.

   QEMU's default codes are 64 MB each (VIRT_FLASH is 128 MB, split in two),
   but the ArmVirtQemu images are 64 MB only when padded: the real files are
   smaller, so a bank takes any size up to that and the rest reads as 0xFF
   (erased flash), which is what the firmware expects of untouched flash. */

typedef struct HvPflash {
    HvMachine *m;
    uint64_t base[2], size[2]; /* size[0] + size[1] = the whole window */
    uint8_t *mem[2];
} HvPflash;

/* Maps the window [pa, pa + code_size + vars_size) and loads the two files if
   given (NULL: left erased). Returns NULL if the window or either file does not
   fit, or if the region is already taken. */
HvPflash *hv_pflash_create(HvMachine *m, uint64_t pa, uint64_t code_size, uint64_t vars_size,
                           const char *code_path, const char *vars_path);
void hv_pflash_destroy(HvPflash *p);

/* Writes the writable bank back to disk (UEFI's variables), so the next boot
   sees what this one changed. Returns 0 on success. */
int hv_pflash_save_vars(HvPflash *p, const char *path);

#endif
