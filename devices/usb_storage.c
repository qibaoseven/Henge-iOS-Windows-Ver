/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include "usb_storage.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- little endian helpers (the wire format, not the host's) --- */
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

/* SCSI CDBs and their responses are big endian, unlike the BOT signatures. */
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}
static void put32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}
static void put16be(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }

static void set_sense(HvUsbCd *cd, uint8_t key, uint8_t asc, uint8_t ascq) {
    cd->sense_key = key;
    cd->asc = asc;
    cd->ascq = ascq;
}

/* Fail the current command: no data, CSW status 1, and a sense to explain it. */
static void fail(HvUsbCd *cd, uint8_t key, uint8_t asc) {
    cd->failed = 1;
    cd->data_len = 0;
    cd->data_off = 0;
    cd->from_buf = 0;
    cd->buf_len = 0;
    set_sense(cd, key, asc, 0);
}

/* A short response held in cd->buf, truncated to what the CBW asked for. */
static void respond(HvUsbCd *cd, const uint8_t *src, uint32_t len, uint32_t alloc) {
    if (len > sizeof cd->buf) len = sizeof cd->buf;
    memcpy(cd->buf, src, len);
    cd->buf_len = len;
    cd->from_buf = 1;
    cd->data_off = 0;
    cd->data_len = alloc < len ? alloc : len;
}

/* --- geometry --- */

int hv_usb_cd_open(HvUsbCd *cd, const char *path) {
    if (!cd) return -1;
    hv_usb_cd_close(cd);
    memset(cd, 0, sizeof *cd);
    pthread_mutex_init(&cd->mu, NULL);
    if (!path || !path[0]) return 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[usb_cd] cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        fprintf(stderr, "[usb_cd] %s is empty or unreadable\n", path);
        close(fd);
        return -1;
    }
    cd->fd = fd;
    cd->bytes = (uint64_t)st.st_size;
    cd->blocks = (cd->bytes + HV_CD_BLOCK - 1) / HV_CD_BLOCK;
    cd->present = 1;
    snprintf(cd->path, sizeof cd->path, "%s", path);
    /* A CD is not 512-byte aligned, so the last block reads short on purpose; the
       CSW residue tells the host how much was actually there. */
    return 0;
}

void hv_usb_cd_close(HvUsbCd *cd) {
    if (!cd) return;
    if (cd->fd > 0) close(cd->fd);
    cd->fd = -1;
    cd->present = 0;
}

static void bot_begin(HvUsbCd *cd) {
    cd->tag = 0;
    cd->data_len = cd->data_off = 0;
    cd->data_pos = 0;
    cd->dir_in = 0;
    cd->failed = 0;
    cd->from_buf = 0;
    cd->buf_len = 0;
}

void hv_usb_cd_reset(HvUsbCd *cd) {
    if (!cd) return;
    bot_begin(cd);
    set_sense(cd, 0, 0, 0);
}

/* --- SCSI --- */

static void scsi_inquiry(HvUsbCd *cd, const uint8_t *cdb) {
    uint8_t page = cdb[2] & 0xff;
    int evpd = cdb[1] & 1;
    uint8_t r[64];
    memset(r, 0, sizeof r);

    if (!evpd) {
        r[0] = 0x05;            /* peripheral device type 5: CD-ROM */
        r[1] = 0x80;            /* removable */
        r[2] = 0x05;            /* SPC-3 */
        r[3] = 0x02;            /* response data format */
        r[4] = 31;              /* additional length */
        memcpy(r + 8, "HENGE   ", 8);
        memcpy(r + 16, "VIRTUAL CD-ROM  ", 16);
        memcpy(r + 32, "1.0 ", 4);
        respond(cd, r, 36, be16(cdb + 3));
        return;
    }
    if (page == 0x00) { /* supported pages */
        r[0] = 0x05;
        r[1] = 0x00;
        r[2] = 0x00;
        r[3] = 2;
        r[4] = 0x00;
        r[5] = 0x80;
        respond(cd, r, 6, be16(cdb + 3));
        return;
    }
    if (page == 0x80) { /* serial number */
        r[0] = 0x05;
        r[1] = 0x80;
        r[3] = 8;
        memcpy(r + 4, "HENGE001", 8);
        respond(cd, r, 12, be16(cdb + 3));
        return;
    }
    fail(cd, 0x05, 0x24); /* invalid field in CDB */
}

static void scsi_request_sense(HvUsbCd *cd, const uint8_t *cdb) {
    uint8_t r[18];
    memset(r, 0, sizeof r);
    r[0] = 0x70;      /* current error, fixed format */
    r[2] = cd->sense_key & 0x0f;
    r[7] = 10;        /* additional length */
    r[12] = cd->asc;
    r[13] = cd->ascq;
    respond(cd, r, 18, cdb[4] < 18 ? cdb[4] : 18);
    /* Sense is consumed by reading it. */
    set_sense(cd, 0, 0, 0);
}

static void scsi_read_capacity(HvUsbCd *cd, const uint8_t *cdb, int wide) {
    uint8_t r[32];
    memset(r, 0, sizeof r);
    uint64_t last = cd->blocks ? cd->blocks - 1 : 0;
    if (wide) {
        put32be(r + 0, (uint32_t)(last >> 32));
        put32be(r + 4, (uint32_t)last);
        put32be(r + 8, HV_CD_BLOCK);
        respond(cd, r, 32, be32(cdb + 10));
    } else {
        put32be(r + 0, (uint32_t)last);
        put32be(r + 4, HV_CD_BLOCK);
        /* READ CAPACITY(10) has no allocation length field: always eight bytes. */
        respond(cd, r, 8, 8);
    }
}

static void scsi_mode_sense(HvUsbCd *cd, const uint8_t *cdb, int wide) {
    uint8_t r[16];
    memset(r, 0, sizeof r);
    if (wide) {
        r[1] = 6;                 /* mode data length */
        r[3] = 0x80;              /* write protected */
        respond(cd, r, 8, be16(cdb + 7));
    } else {
        r[0] = 3;                 /* mode data length */
        r[2] = 0x80;              /* write protected */
        respond(cd, r, 4, cdb[4]);
    }
}

static void scsi_get_configuration(HvUsbCd *cd, const uint8_t *cdb) {
    uint8_t r[8];
    memset(r, 0, sizeof r);
    r[6] = 0x00; /* current profile high byte */
    r[7] = 0x08; /* profile 0x0008: CD-ROM */
    put32be(r + 0, 0); /* no feature descriptors follow */
    respond(cd, r, 8, be16(cdb + 7));
}

static void scsi_get_event_status(HvUsbCd *cd, const uint8_t *cdb) {
    uint8_t r[8];
    memset(r, 0, sizeof r);
    /* No event class is supported; NEA (bit 7 of byte 2) tells the host to stop
       asking. This is what a real drive returns when nothing has happened. */
    r[2] = 0x80;
    respond(cd, r, 8, be16(cdb + 7));
}

static void scsi_read_toc(HvUsbCd *cd, const uint8_t *cdb) {
    uint8_t r[4 + 2 * 8];
    memset(r, 0, sizeof r);
    put16be(r + 0, 2 + 2 * 8); /* data length: two descriptors */
    r[2] = 1;                /* first track */
    r[3] = 1;                /* last track */
    /* track 1 at LBA 0 */
    r[4 + 1] = 0x14; /* data track, address type 1 (Q subchannel) */
    r[4 + 2] = 1;
    put32be(r + 4 + 4, 0);
    /* lead-out */
    r[12 + 1] = 0x14;
    r[12 + 2] = 0xaa;
    put32be(r + 12 + 4, (uint32_t)(cd->blocks ? cd->blocks : 1));
    respond(cd, r, 4 + 2 * 8, be16(cdb + 7));
}

static void scsi_report_luns(HvUsbCd *cd, const uint8_t *cdb) {
    uint8_t r[16];
    memset(r, 0, sizeof r);
    put32be(r + 0, 8); /* one LUN follows */
    respond(cd, r, 16, be32(cdb + 6));
}

static void scsi_read(HvUsbCd *cd, const uint8_t *cdb, int cdb12) {
    if (!cd->present || cd->fd < 0) {
        fail(cd, 0x02, 0x3a); /* not ready, medium not present */
        return;
    }
    uint32_t lba = be32(cdb + 2);
    uint32_t nblocks = cdb12 ? be32(cdb + 6) : be16(cdb + 7);
    if (nblocks == 0) { /* nothing to do */
        cd->data_len = cd->data_off = 0;
        return;
    }
    uint64_t end = (uint64_t)lba + nblocks;
    if (end > cd->blocks) {
        fail(cd, 0x05, 0x21); /* LBA out of range */
        return;
    }
    cd->data_pos = (uint64_t)lba * HV_CD_BLOCK;
    uint64_t len64 = (uint64_t)nblocks * HV_CD_BLOCK;
    uint64_t avail = cd->bytes - cd->data_pos;
    if (len64 > avail) len64 = avail; /* the tail block is short by design */
    cd->data_len = (uint32_t)(len64 > 0xffffffffu ? 0xffffffffu : len64);
    cd->data_off = 0;
    cd->from_buf = 0;
    cd->dir_in = 1;
}

void hv_usb_cd_cbw(HvUsbCd *cd, const uint8_t *cbw) {
    if (!cd || !cbw) return;
    pthread_mutex_lock(&cd->mu);

    if (le32(cbw) != 0x43425355u) {
        /* Bad signature: the transport must stall; wipe the state and let the
           host's reset recover it. */
        bot_begin(cd);
        cd->failed = 1;
        set_sense(cd, 0x05, 0x1a, 0);
        pthread_mutex_unlock(&cd->mu);
        return;
    }
    bot_begin(cd);
    cd->tag = le32(cbw + 4);
    uint32_t xfer = le32(cbw + 8);
    cd->xfer_len = xfer;
    int flags = cbw[12];
    int cdb_len = cbw[14] & 0x0f;
    cd->dir_in = (flags & 0x80) != 0;

    if (cdb_len < 6 || cdb_len > 16) {
        fail(cd, 0x05, 0x1a);
        pthread_mutex_unlock(&cd->mu);
        return;
    }
    const uint8_t *cdb = cbw + 15;

    /* A write to read-only media is the one direction we cannot fake. */
    if (!cd->dir_in && xfer > 0) {
        fail(cd, 0x07, 0x27); /* data protect, write protected */
        pthread_mutex_unlock(&cd->mu);
        return;
    }

    switch (cdb[0]) {
    case 0x00: /* TEST UNIT READY */
        if (!cd->present) {
            fail(cd, 0x02, 0x3a);
        }
        break;
    case 0x03:
        scsi_request_sense(cd, cdb);
        break;
    case 0x12:
        scsi_inquiry(cd, cdb);
        break;
    case 0x1a:
        scsi_mode_sense(cd, cdb, 0);
        break;
    case 0x5a:
        scsi_mode_sense(cd, cdb, 1);
        break;
    case 0x25:
        scsi_read_capacity(cd, cdb, 0);
        break;
    case 0x9e:
        if ((cdb[1] & 0x1f) == 0x10) scsi_read_capacity(cd, cdb, 1);
        else fail(cd, 0x05, 0x20);
        break;
    case 0x28:
        scsi_read(cd, cdb, 0);
        break;
    case 0xa8:
        scsi_read(cd, cdb, 1);
        break;
    case 0x46:
        scsi_get_configuration(cd, cdb);
        break;
    case 0x4a:
        scsi_get_event_status(cd, cdb);
        break;
    case 0x43:
        scsi_read_toc(cd, cdb);
        break;
    case 0xa0:
        scsi_report_luns(cd, cdb);
        break;
    case 0x1b: /* START STOP UNIT */
    case 0x1e: /* PREVENT/ALLOW MEDIUM REMOVAL */
    case 0x35: /* SYNCHRONIZE CACHE */
    case 0x2f: /* VERIFY(10) */
    case 0x2e: /* VERIFY(10) wrong opcode */
    case 0x51: /* READ DISC INFORMATION: no data, but harmless */
        break;
    default:
        fail(cd, 0x05, 0x20); /* invalid command operation code */
        break;
    }

    /* The CBW said how much data it expects; never promise more than we have. */
    if (!cd->failed && cd->dir_in && cd->data_len > xfer) cd->data_len = xfer;
    pthread_mutex_unlock(&cd->mu);
}

static uint32_t emit_csw(HvUsbCd *cd, uint8_t *out, uint32_t want, int *is_csw) {
    uint8_t csw[HU_CD_CSW_SIZE];
    put32(csw + 0, 0x53425355u);
    put32(csw + 4, cd->tag);
    uint32_t moved = cd->data_off;
    uint32_t asked = cd->xfer_len;
    put32(csw + 8, asked > moved ? asked - moved : 0); /* residue */
    csw[12] = cd->failed ? 1 : 0;
    if (want < HU_CD_CSW_SIZE) return 0; /* hosts always ask for the full 13 */
    memcpy(out, csw, HU_CD_CSW_SIZE);
    cd->data_len = cd->data_off = 0;
    cd->xfer_len = 0;
    cd->from_buf = 0;
    cd->buf_len = 0;
    cd->failed = 0;
    *is_csw = 1;
    return HU_CD_CSW_SIZE;
}

uint32_t hv_usb_cd_read_in(HvUsbCd *cd, uint8_t *out, uint32_t want, int *is_csw) {
    if (!cd || !out) return 0;
    pthread_mutex_lock(&cd->mu);
    *is_csw = 0;

    if (cd->data_off < cd->data_len) {
        uint32_t n = cd->data_len - cd->data_off;
        if (n > want) n = want;
        if (cd->from_buf) {
            memcpy(out, cd->buf + cd->data_off, n);
        } else {
            ssize_t got = pread(cd->fd, out, n, (off_t)(cd->data_pos + cd->data_off));
            if (got <= 0) {
                cd->failed = 1;
                set_sense(cd, 0x03, 0x11, 0); /* medium error */
                cd->data_off = cd->data_len; /* stop promising more */
                pthread_mutex_unlock(&cd->mu);
                return emit_csw(cd, out, want, is_csw);
            }
            n = (uint32_t)got;
        }
        cd->data_off += n;
        pthread_mutex_unlock(&cd->mu);
        return n;
    }

    /* Data phase over (or never started): the transfer ends with the CSW. */
    uint32_t n = emit_csw(cd, out, want, is_csw);
    pthread_mutex_unlock(&cd->mu);
    return n;
}
