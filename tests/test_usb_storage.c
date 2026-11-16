/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Unit tests for the USB CD's SCSI layer, on the host, with no machine attached.
   Builds a fake image whose blocks are recognizable, then drives CBW/CSW pairs
   exactly the way the xHCI bulk endpoints will. */
#include "../devices/usb_storage.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

#define BLOCKS 8

static void put32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

static char *make_image(void) {
    static char path[] = "/tmp/henge_usbcd_test.iso";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return NULL;
    for (int b = 0; b < BLOCKS; b++) {
        uint8_t block[HV_CD_BLOCK];
        memset(block, 0, sizeof block);
        memcpy(block, "BLOCK", 5);
        block[5] = (uint8_t)('0' + b);
        put32le(block + 8, (uint32_t)b);
        if (write(fd, block, sizeof block) != (ssize_t)sizeof block) {
            close(fd);
            return NULL;
        }
    }
    close(fd);
    return path;
}

static void make_cbw(uint8_t *cbw, uint32_t tag, uint32_t xfer, int dir_in, const uint8_t *cdb, int cdb_len) {
    memset(cbw, 0, 31);
    put32le(cbw + 0, 0x43425355u);
    put32le(cbw + 4, tag);
    put32le(cbw + 8, xfer);
    cbw[12] = dir_in ? 0x80 : 0x00;
    cbw[13] = 0;
    cbw[14] = (uint8_t)(cdb_len & 0x0f);
    memcpy(cbw + 15, cdb, (size_t)cdb_len);
}

/* Drain the bulk-IN side until the CSW arrives; returns payload bytes copied. */
static uint32_t drain(HvUsbCd *cd, uint8_t *out, uint32_t want, int *status) {
    uint32_t got = 0;
    for (int guard = 0; guard < 64; guard++) {
        int is_csw = 0;
        uint32_t n = hv_usb_cd_read_in(cd, out + got, want - got, &is_csw);
        got += n;
        if (is_csw) {
            *status = out[got - 1];
            return got - HU_CD_CSW_SIZE;
        }
        if (n == 0) break;
    }
    *status = -1;
    return got;
}

static void test_inquiry(HvUsbCd *cd) {
    uint8_t cdb[6] = {0x12, 0, 0, 0, 36, 0};
    uint8_t cbw[31];
    make_cbw(cbw, 0x11111111, 36, 1, cdb, 6);
    hv_usb_cd_cbw(cd, cbw);

    uint8_t out[64];
    int status = 0;
    uint32_t n = drain(cd, out, 36 + HU_CD_CSW_SIZE, &status);
    CHECK(n == 36);
    CHECK(status == 0);
    CHECK(out[0] == 0x05);          /* CD-ROM */
    CHECK((out[1] & 0x80) != 0);    /* removable */
    CHECK(memcmp(out + 8, "HENGE", 5) == 0);
}

static void test_capacity(HvUsbCd *cd) {
    uint8_t cdb[10] = {0x25};
    uint8_t cbw[31];
    make_cbw(cbw, 0x22222222, 8, 1, cdb, 10);
    hv_usb_cd_cbw(cd, cbw);
    uint8_t out[32];
    int status = 0;
    uint32_t n = drain(cd, out, 8 + HU_CD_CSW_SIZE, &status);
    CHECK(n == 8);
    CHECK(status == 0);
    uint32_t last = (uint32_t)out[0] << 24 | (uint32_t)out[1] << 16 | (uint32_t)out[2] << 8 | (uint32_t)out[3];
    uint32_t bs = (uint32_t)out[4] << 24 | (uint32_t)out[5] << 16 | (uint32_t)out[6] << 8 | (uint32_t)out[7];
    CHECK(last == BLOCKS - 1);
    CHECK(bs == HV_CD_BLOCK);
}

static void test_read(HvUsbCd *cd) {
    uint8_t cdb[10] = {0x28, 0, 0, 0, 0, 2, 0, 0, 2, 0}; /* lba 2, 2 blocks */
    uint8_t cbw[31];
    make_cbw(cbw, 0x33333333, 2 * HV_CD_BLOCK, 1, cdb, 10);
    hv_usb_cd_cbw(cd, cbw);
    uint8_t out[2 * 4096];
    int status = 0;
    uint32_t n = drain(cd, out, 2 * HV_CD_BLOCK + HU_CD_CSW_SIZE, &status);
    CHECK(n == 2 * HV_CD_BLOCK);
    CHECK(status == 0);
    CHECK(memcmp(out, "BLOCK2", 6) == 0);
    CHECK(memcmp(out + HV_CD_BLOCK, "BLOCK3", 6) == 0);
}

static void test_read_out_of_range(HvUsbCd *cd) {
    uint8_t cdb[10] = {0x28, 0, 0, 0, 0, 9, 0, 0, 4}; /* lba 9 of 8 -> wrong */
    uint8_t cbw[31];
    make_cbw(cbw, 0x44444444, 4 * HV_CD_BLOCK, 1, cdb, 10);
    hv_usb_cd_cbw(cd, cbw);
    uint8_t out[64];
    int status = 0;
    uint32_t n = drain(cd, out, sizeof out, &status);
    CHECK(n == 0);
    CHECK(status == 1);

    uint8_t sense_cdb[6] = {0x03, 0, 0, 0, 18, 0};
    make_cbw(cbw, 0x55555555, 18, 1, sense_cdb, 6);
    hv_usb_cd_cbw(cd, cbw);
    uint8_t s[32];
    n = drain(cd, s, 18 + HU_CD_CSW_SIZE, &status);
    CHECK(n == 18);
    CHECK(s[2] == 0x05); /* illegal request */
    CHECK(s[12] == 0x21); /* LBA out of range */
}

static void test_unknown_command(HvUsbCd *cd) {
    uint8_t cdb[6] = {0x99, 0, 0, 0, 0, 0};
    uint8_t cbw[31];
    make_cbw(cbw, 0x66666666, 0, 1, cdb, 6);
    hv_usb_cd_cbw(cd, cbw);
    uint8_t out[32];
    int status = 0;
    uint32_t n = drain(cd, out, sizeof out, &status);
    CHECK(n == 0);
    CHECK(status == 1);
}

static void test_write_is_refused(HvUsbCd *cd) {
    uint8_t cdb[10] = {0x2a, 0, 0, 0, 0, 0, 0, 0, 1, 0}; /* WRITE(10) */
    uint8_t cbw[31];
    make_cbw(cbw, 0x77777777, HV_CD_BLOCK, 0, cdb, 10);
    hv_usb_cd_cbw(cd, cbw);
    uint8_t out[32];
    int status = 0;
    uint32_t n = drain(cd, out, sizeof out, &status);
    CHECK(n == 0);
    CHECK(status == 1);
}

static void test_event_status_and_toc(HvUsbCd *cd) {
    uint8_t cbw[31];
    uint8_t cdb_evt[10] = {0x4a, 0x01, 0, 0, 0, 0, 0, 0, 8, 0};
    make_cbw(cbw, 0x88888888, 8, 1, cdb_evt, 10);
    hv_usb_cd_cbw(cd, cbw);
    uint8_t out[32];
    int status = 0;
    uint32_t n = drain(cd, out, 8 + HU_CD_CSW_SIZE, &status);
    CHECK(n == 8);
    CHECK(status == 0);
    CHECK((out[2] & 0x80) != 0); /* NEA: no event available */

    uint8_t cdb_toc[10] = {0x43, 0, 0, 0, 0, 0, 0, 0, 20, 0};
    make_cbw(cbw, 0x99999999, 20, 1, cdb_toc, 10);
    hv_usb_cd_cbw(cd, cbw);
    n = drain(cd, out, 20 + HU_CD_CSW_SIZE, &status);
    CHECK(status == 0);
    CHECK(out[2] == 1 && out[3] == 1); /* one track */
}

int main(void) {
    char *path = make_image();
    if (!path) {
        fprintf(stderr, "FAIL: cannot build the test image\n");
        return 1;
    }
    HvUsbCd cd;
    if (hv_usb_cd_open(&cd, path) != 0) {
        fprintf(stderr, "FAIL: cannot open the test image\n");
        return 1;
    }
    CHECK(cd.blocks == BLOCKS);
    CHECK(cd.present == 1);

    test_inquiry(&cd);
    test_capacity(&cd);
    test_read(&cd);
    test_read_out_of_range(&cd);
    test_unknown_command(&cd);
    test_write_is_refused(&cd);
    test_event_status_and_toc(&cd);

    hv_usb_cd_close(&cd);
    unlink(path);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("usb_storage: all checks passed\n");
    return 0;
}
