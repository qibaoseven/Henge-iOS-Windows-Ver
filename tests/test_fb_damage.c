/* The damage rectangle is what lets the presenter upload a few rows instead of
   four megabytes: virtio-gpu's RESOURCE_FLUSH hands us exactly the rectangle the
   guest says it repainted. If the union or the clearing were wrong, the screen
   would either flicker (taken too early) or stop updating (never cleared). */

#include "fb_display.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* fb_display.c only reaches this through the ramfb fw_cfg callback, which this
   test never triggers; it still needs the symbol to link. */
/* The display registers its ramfb callback on creation; this test does not have
   a fw_cfg to register it with, so the symbol is stubbed out. */
int hv_fw_cfg_add_writable_file(HvFwCfg *fc, const char *name, uint32_t len, HvFwCfgWriteCb cb, void *opaque) {
    (void)fc;
    (void)name;
    (void)len;
    (void)cb;
    (void)opaque;
    return 0;
}

void *hv_phys_ptr(HvMachine *m, uint64_t pa, uint64_t len) {
    (void)m;
    (void)pa;
    (void)len;
    return NULL;
}

int main(void) {
    HvFbDisplay *fb = hv_fb_display_create(NULL, NULL);
    assert(fb);
    uint32_t x = 99, y = 99, w = 99, h = 99;

    /* Nothing scanned out yet: nothing to take, and the display is inactive. */
    assert(hv_fb_display_take_damage(fb, &x, &y, &w, &h) == 0);

    uint8_t frame[64 * 32 * 4];
    memset(frame, 0x5a, sizeof frame);
    assert(hv_fb_display_set_scanout(fb, frame, 64, 32, 64 * 4, 0x34325258) == 0);

    /* A new scanout invalidates the whole frame -- the texture is stale. */
    assert(hv_fb_display_take_damage(fb, &x, &y, &w, &h) == 1);
    if (x || y || w != 64 || h != 32) {
        fprintf(stderr, "FAIL new scanout: %u,%u %ux%u\n", x, y, w, h);
        return 1;
    }
    /* ... and only once. */
    assert(hv_fb_display_take_damage(fb, &x, &y, &w, &h) == 0);

    /* Two flushes union into one rectangle. */
    hv_fb_display_damage(fb, 10, 10, 5, 5);
    hv_fb_display_damage(fb, 40, 20, 8, 3);
    assert(hv_fb_display_take_damage(fb, &x, &y, &w, &h) == 1);
    if (x != 10 || y != 10 || w != 38 || h != 13) {
        fprintf(stderr, "FAIL union: %u,%u %ux%u (want 10,10 38x13)\n", x, y, w, h);
        return 1;
    }

    /* A zero-sized flush is not damage. */
    hv_fb_display_damage(fb, 0, 0, 0, 0);
    assert(hv_fb_display_take_damage(fb, &x, &y, &w, &h) == 0);

    /* Same pointer, same size: still reports what the guest says it changed, so
       a flush must not be swallowed by "geometry did not change". */
    hv_fb_display_damage(fb, 1, 2, 3, 4);
    assert(hv_fb_display_take_damage(fb, &x, &y, &w, &h) == 1);
    if (x != 1 || y != 2 || w != 3 || h != 4) {
        fprintf(stderr, "FAIL flush rect: %u,%u %ux%u\n", x, y, w, h);
        return 1;
    }

    /* The hint is what GET_DISPLAY_INFO answers with. */
    hv_fb_display_set_hint(fb, 1440, 900);
    assert(fb->hint_w == 1440 && fb->hint_h == 900);
    hv_fb_display_set_hint(fb, 10, 10); /* too small: ignored */
    assert(fb->hint_w == 1440);

    /* Detaching clears the display (the presenter stops polling a dead buffer). */
    hv_fb_display_set_scanout(fb, NULL, 0, 0, 0, 0);
    const uint8_t *p = NULL;
    uint32_t pw = 0, ph = 0, ps = 0;
    assert(hv_fb_display_get_frame(fb, &p, &pw, &ph, &ps) != 0);

    printf("fb_damage: all checks passed\n");
    return 0;
}
