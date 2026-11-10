#ifndef HENGEVM_PFLASH_H
#define HENGEVM_PFLASH_H

#include <stdint.h>

#include "machine.h"

/* The two nor-flash banks of QEMU's virt: the code bank at 0x0 (read-only,
   UEFI's code part) and the variables bank after it (writable, UEFI's
   settings).

   This is RAM to the engine, not an MMIO device. The JIT only ever fetches
   instructions out of HvMachine.ram[], so a firmware behind an MMIO window
   could not execute a single instruction -- which is why QEMU maps its flash
   as a memory region too, and why vmapple.c maps its boot ROM the same way.

   The read-only bank is registered as read-only RAM: the engine turns writes
   away from it and never lets a TLB entry cache it as writable. The variables
   bank is plain read-write RAM, saved back to its file on request. */

typedef struct HvPflash {
    HvMachine *m;
    uint64_t base[2], size[2];
    uint8_t *mem[2];
} HvPflash;

/* Maps the window [pa, pa + code_size + vars_size) and loads the two files if
   given (NULL: left erased). Returns NULL if the window or either file does not
   fit, or if the region is already taken. */
HvPflash *hv_pflash_create(HvMachine *m, uint64_t pa, uint64_t code_size, uint64_t vars_size,
                           const char *code_path, const char *vars_path);
void hv_pflash_destroy(HvPflash *p);

/* Writes the writable bank back to disk (UEFI's variables). Returns 0 on success. */
int hv_pflash_save_vars(HvPflash *p, const char *path);

#endif
