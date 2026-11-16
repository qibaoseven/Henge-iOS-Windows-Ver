/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A USB-attached read-only CD-ROM: the Bulk-Only Transport state machine plus the
 * SCSI command set a CD needs. No guest memory, no controller: this file only turns
 * an image file plus a stream of SCSI commands into the bytes a driver expects, so
 * it can be unit tested without a machine (tests/test_usb_storage.c does exactly
 * that).
 *
 * Why CD and not a disk: Windows only attaches its cdfs/udf drivers to devices whose
 * INQUIRY says peripheral device type 5 (CD-ROM). That is the whole reason a >4 GB
 * install image can be read at all.
 */

#ifndef HENGEVM_USB_STORAGE_H
#define HENGEVM_USB_STORAGE_H

#include <pthread.h>
#include <stdint.h>

#define HV_CD_BLOCK 2048u
#define HU_CD_CSW_SIZE 13u

typedef struct HvUsbCd {
    int fd;
    uint64_t bytes;  /* image size in bytes */
    uint64_t blocks; /* bytes / HV_CD_BLOCK (rounded up) */
    char path[256];
    int present; /* media inserted (tray closed) */

    /* Bulk-Only Transport, one command at a time */
    uint32_t tag;     /* from the last CBW, echoed in the CSW */
    uint32_t xfer_len; /* allocation length the CBW asked for (residue maths) */
    uint32_t data_len; /* bytes we actually have to move */
    uint32_t data_off; /* how far we have produced/consumed */
    uint64_t data_pos; /* image offset the data phase starts at */
    int dir_in;        /* CBW direction bit: 1 = device-to-host */
    int failed;        /* command failed -> CSW status 1 */
    int from_buf;      /* data comes from `buf` rather than from the image */
    uint8_t buf[512];  /* small command responses (INQUIRY, capacity, ...) */
    uint32_t buf_len;

    /* Sense, so REQUEST SENSE can report what went wrong */
    uint8_t sense_key, asc, ascq;

    pthread_mutex_t mu;
} HvUsbCd;

/* Open an image file read-only. Returns 0, or -1 (and leaves the device empty). */
int hv_usb_cd_open(HvUsbCd *cd, const char *path);
void hv_usb_cd_close(HvUsbCd *cd);
/* Throw the media away without touching the file handle (used when resetting). */
void hv_usb_cd_reset(HvUsbCd *cd);

/* One CBW arrived (31 bytes, little endian as it came off the wire). Parses it,
 * executes the CDB, and lays out the data phase the following IN transfers consume. */
void hv_usb_cd_cbw(HvUsbCd *cd, const uint8_t *cbw);

/* Answer one bulk-IN transfer of up to `want` bytes. Returns the number of bytes
 * written to out. Writes is_csw=1 when those bytes are the 13-byte CSW, which ends
 * the command. Zero bytes means "nothing to send" (a subsequent CBW is expected). */
uint32_t hv_usb_cd_read_in(HvUsbCd *cd, uint8_t *out, uint32_t want, int *is_csw);

#endif
