#include "henge-winvm.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "henge-ios-jit.h"
#include "winvm.h"

#ifndef HENGE_EXPORT
#define HENGE_EXPORT __attribute__((visibility("default")))
#endif

/* 0 never started, 1 running, 2 stopped, 3 starting. The UI reads this on every
   frame, so it is a plain atomic and the mutex only guards transitions. */
static _Atomic int state;
static pthread_mutex_t api_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t generation;
static const char *exit_reason;
static int board_in_use; /* the board holds open media that must be released */
static int display_kind = 2; /* both, until the UI says otherwise */
static uint32_t panel_w, panel_h;
static int gpu3d;

HENGE_EXPORT HengeDualMapping *henge_ios_jit_get_mapping(void);

static void on_exit(int reason) {
    pthread_mutex_lock(&api_lock);
    exit_reason = hv_winvm_reason_name(reason);
    if (board_in_use) {
        hv_winvirt_detach_cds(hv_winvm_board());
        board_in_use = 0;
    }
    atomic_store(&state, 2);
    pthread_mutex_unlock(&api_lock);
    fprintf(stderr, "henge: the Windows machine stopped (%s)\n", exit_reason ? exit_reason : "unknown");
}

int henge_winvm_state(void) { return atomic_load(&state); }

uint64_t henge_winvm_generation(void) { return atomic_load(&generation); }

const char *henge_winvm_exit_reason(void) {
    return atomic_load(&state) == 2 && exit_reason ? exit_reason : "";
}

int henge_winvm_boot(const char *code_fd, const char *vars_fd, const char *disk0, const char *cd0, const char *cd1,
                     const char *cd2, uint64_t ram_mb, int ncpus, char *err, size_t errlen) {
    if (!code_fd || !vars_fd) {
        snprintf(err, errlen, "The firmware files (code.fd, vars.fd) are missing.");
        return -1;
    }
    pthread_mutex_lock(&api_lock);
    if (atomic_load(&state) == 1 || atomic_load(&state) == 3) {
        snprintf(err, errlen, "The previous machine is still running or shutting down.");
        pthread_mutex_unlock(&api_lock);
        return -1;
    }
    atomic_store(&state, 3);

    HengeDualMapping *jit = henge_ios_jit_get_mapping();
    if (!jit || !jit->rw_addr) {
        snprintf(err, errlen, "Henge has no JIT region. Set up JIT first.");
        atomic_store(&state, 0);
        pthread_mutex_unlock(&api_lock);
        return -1;
    }

    HvWinVmConfig c;
    memset(&c, 0, sizeof c);
    c.code_fd = code_fd;
    c.vars_fd = vars_fd;
    c.disk0 = disk0;
    c.cd0 = cd0;
    c.cd1 = cd1;
    c.cd2 = cd2;
    c.ram_mb = ram_mb ? ram_mb : 6144; /* the design's default: 6 GB */
    c.ncpus = ncpus;
    c.jit_bytes = jit->size;
    c.jit_rx = jit->rx_addr;
    c.jit_rw = jit->rw_addr;
    c.gpu3d = gpu3d;
    c.display_kind = display_kind;
    c.panel_w = panel_w;
    c.panel_h = panel_h;
    c.log = 0;
    c.enable_sound = 0; /* HDA is for the Mac guest; Windows has no driver for it yet */

    fprintf(stderr, "henge: booting Windows with %d CPUs, %llu MB RAM, %zu MB JIT, disk=%s cd0=%s cd1=%s cd2=%s\n",
            c.ncpus, (unsigned long long)c.ram_mb, jit->size >> 20, disk0 ? disk0 : "(none)",
            cd0 ? cd0 : "(none)", cd1 ? cd1 : "(none)", cd2 ? cd2 : "(none)");

    if (hv_winvm_create(&c, err, errlen) != 0) {
        atomic_store(&state, 0);
        pthread_mutex_unlock(&api_lock);
        return -1;
    }
    board_in_use = 1;
    exit_reason = NULL;
    hv_winvm_start(1, on_exit); /* CPU 0 gets its own thread; Swift stays free */
    atomic_store(&state, 1);
    atomic_fetch_add(&generation, 1);
    pthread_mutex_unlock(&api_lock);
    return 0;
}

int henge_winvm_make_disk(const char *path, uint64_t gb) {
    if (!path || !gb) return -1;
    struct stat st;
    if (stat(path, &st) == 0) return 0; /* never shrink or wipe an existing machine */
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)(gb << 30)) != 0) {
        close(fd);
        unlink(path);
        return -1;
    }
    close(fd);
    return 0;
}

void henge_winvm_stop(void) {
    if (atomic_load(&state) != 1) return;
    hv_winvm_stop();
}

void henge_winvm_power_button(void) { hv_winvm_power_button(); }

void henge_winvm_flush_disks(void) { hv_winvm_flush_disks(); }

void henge_winvm_set_display(int kind, uint32_t width, uint32_t height) {
    display_kind = kind < 0 || kind > 2 ? 2 : kind;
    panel_w = width;
    panel_h = height;
}

void henge_winvm_set_gpu3d(int enabled) { gpu3d = enabled ? 1 : 0; }

void henge_winvm_set_display_layer(void *layer) { hv_winvm_set_display_layer(layer); }

int henge_winvm_display_size(uint32_t *w, uint32_t *h) { return hv_winvm_display_size(w, h); }

uint64_t henge_winvm_display_frames(void) { return hv_winvm_display_frames(); }

int henge_winvm_save_png(const char *path) { return hv_winvm_save_png(path); }

void henge_winvm_key_hid(uint8_t usage, int down) { hv_winvm_key_hid(usage, down); }

void henge_winvm_tablet(double x, double y, uint8_t buttons, int8_t wheel) {
    if (x < 0.0) x = 0.0;
    if (x > 1.0) x = 1.0;
    if (y < 0.0) y = 0.0;
    if (y > 1.0) y = 1.0;
    hv_winvm_tablet(x, y, buttons, wheel);
}

void henge_winvm_ctrl_alt_del(void) { hv_winvm_send_ctrl_alt_del(); }

void henge_winvm_usb_stats(int *kbd_ready, int *tab_ready, uint64_t *delivered) {
    hv_winvm_usb_stats(kbd_ready, tab_ready, delivered);
}
