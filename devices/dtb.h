#ifndef HENGEVM_DTB_H
#define HENGEVM_DTB_H

#include <stdint.h>

/* A minimal flattened device tree, written the way QEMU writes virt's.

   UEFI on arm64 takes the platform description either as a DT or as ACPI, and
   ArmVirtQemu exposes whichever it was given. The DT path costs nothing here --
   the address goes in x0 at reset -- while ACPI would need QEMU's table blobs
   and their relocation, so the machine starts with a DT and only grows ACPI
   when Windows, which will not read a DT at all, is the guest.

   No ITS node: there is no ITS yet, and a DT that named one would send the
   firmware looking for a device that is not there. */

/* Writes the tree into buf (capacity cap) and returns its length, or 0 if it
   does not fit. */
uint64_t hv_dtb_build(uint8_t *buf, uint64_t cap, uint64_t ram_base, uint64_t ram_size, uint32_t ncpus);

#endif
