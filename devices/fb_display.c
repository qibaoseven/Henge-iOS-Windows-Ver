#include "fb_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RAMFBCfg written by EDK2 QemuRamFbDxe via fw_cfg DMA to "etc/ramfb" (28 bytes, big-endian):
   struct RAMFBCfg {
       uint64_t addr;
       uint32_t fourcc;
       uint32_t flags;
       uint32_t width;
       uint32_t height;
       uint32_t stride;
   } __attribute__((packed));
*/

static uint32_t be32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

static uint64_t be64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}

static void on_ramfb_write(void *opaque, const uint8_t *data, uint32_t len) {
    HvFbDisplay *fb = opaque;
    if (len < 28) return;

    uint64_t addr = be64(data + 0);
    uint32_t fourcc = be32(data + 8);
    uint32_t width = be32(data + 16);
    uint32_t height = be32(data + 20);
    uint32_t stride = be32(data + 24);

    if (width < 64 || width > 8192 || height < 64 || height > 8192) return;
    if (stride == 0) stride = width * 4;
    if (stride < width * 2 || stride > 65536) return;

    uint64_t total_bytes = (uint64_t)stride * (height - 1) + (uint64_t)width * 4;
    void *host_ptr = hv_phys_ptr(fb->m, addr, total_bytes);
    if (!host_ptr) {
        fprintf(stderr, "winvm: ramfb ignored invalid PA 0x%llx (%ux%u stride %u)\n",
                (unsigned long long)addr, width, height, stride);
        return;
    }

    pthread_mutex_lock(&fb->mu);
    fb->fb_pa = addr;
    fb->fb_host = (const uint8_t *)host_ptr;
    fb->width = width;
    fb->height = height;
    fb->stride = stride;
    fb->fourcc = fourcc;
    fb->active = 1;
    pthread_mutex_unlock(&fb->mu);

    fprintf(stderr, "winvm: ramfb configured %ux%u stride=%u fourcc=0x%08x at PA 0x%llx\n",
            width, height, stride, fourcc, (unsigned long long)addr);
}

HvFbDisplay *hv_fb_display_create(HvMachine *m, HvFwCfg *fw_cfg) {
    HvFbDisplay *fb = calloc(1, sizeof *fb);
    fb->m = m;
    pthread_mutex_init(&fb->mu, NULL);
    if (fw_cfg) {
        hv_fw_cfg_add_writable_file(fw_cfg, "etc/ramfb", 28, on_ramfb_write, fb);
    }
    return fb;
}

void hv_fb_display_damage(HvFbDisplay *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!fb || !w || !h) return;
    pthread_mutex_lock(&fb->mu);
    if (!fb->dirty) {
        fb->dirty_x = x;
        fb->dirty_y = y;
        fb->dirty_w = w;
        fb->dirty_h = h;
        fb->dirty = 1;
    } else {
        uint32_t x0 = fb->dirty_x < x ? fb->dirty_x : x;
        uint32_t y0 = fb->dirty_y < y ? fb->dirty_y : y;
        uint32_t x1a = fb->dirty_x + fb->dirty_w, x1b = x + w;
        uint32_t y1a = fb->dirty_y + fb->dirty_h, y1b = y + h;
        uint32_t x1 = x1a > x1b ? x1a : x1b;
        uint32_t y1 = y1a > y1b ? y1a : y1b;
        fb->dirty_x = x0;
        fb->dirty_y = y0;
        fb->dirty_w = x1 - x0;
        fb->dirty_h = y1 - y0;
    }
    pthread_mutex_unlock(&fb->mu);
}

int hv_fb_display_take_damage(HvFbDisplay *fb, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h) {
    if (!fb) return 0;
    int had;
    pthread_mutex_lock(&fb->mu);
    had = fb->dirty;
    if (had) {
        *x = fb->dirty_x;
        *y = fb->dirty_y;
        *w = fb->dirty_w;
        *h = fb->dirty_h;
        fb->dirty = 0;
    }
    pthread_mutex_unlock(&fb->mu);
    return had;
}

void hv_fb_display_set_hint(HvFbDisplay *fb, uint32_t w, uint32_t h) {
    if (!fb || w < 64 || h < 64 || w > 8192 || h > 8192) return;
    pthread_mutex_lock(&fb->mu);
    /* A mode change is the firmware saying "the guest asked for another
       resolution": one line here is the difference between "the guest never
       touched the display" and "it did, and then hung". */
    if (fb->hint_w != w || fb->hint_h != h)
        fprintf(stderr, "winvm: [fb] mode %ux%u (was %ux%u)\n", w, h, fb->hint_w, fb->hint_h);
    fb->hint_w = w;
    fb->hint_h = h;
    pthread_mutex_unlock(&fb->mu);
}

int hv_fb_display_set_scanout(HvFbDisplay *fb, const uint8_t *pixels, uint32_t w, uint32_t h, uint32_t stride,
                              uint32_t fourcc) {
    if (!fb) return -1;
    pthread_mutex_lock(&fb->mu);
    if (!pixels || !w || !h) {
        fb->fb_host = NULL;
        fb->width = fb->height = fb->stride = 0;
        fb->active = 0;
    } else {
        if (fb->fb_host != pixels || fb->width != w || fb->height != h) {
            fb->dirty = 1;
            fb->dirty_x = 0;
            fb->dirty_y = 0;
            fb->dirty_w = w;
            fb->dirty_h = h;
        }
        fb->fb_host = pixels;
        fb->width = w;
        fb->height = h;
        fb->stride = stride;
        fb->fourcc = fourcc;
        fb->active = 1;
    }
    pthread_mutex_unlock(&fb->mu);
    return 0;
}

int hv_fb_display_get_frame(HvFbDisplay *fb, const uint8_t **pixels, uint32_t *w, uint32_t *h, uint32_t *stride) {
    if (!fb) return -1;
    pthread_mutex_lock(&fb->mu);
    if (!fb->active || !fb->fb_host || !fb->width || !fb->height) {
        pthread_mutex_unlock(&fb->mu);
        return -1;
    }
    *pixels = fb->fb_host;
    *w = fb->width;
    *h = fb->height;
    *stride = fb->stride;
    pthread_mutex_unlock(&fb->mu);
    return 0;
}
