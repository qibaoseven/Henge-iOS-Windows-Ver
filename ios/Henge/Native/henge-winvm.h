/* The Windows machine (host/winvm.h) as the Swift side sees it.
   This replaces henge-vm.c, which still speaks the macOS-guest API of host/vm.h. */
#ifndef HENGE_WINVM_H
#define HENGE_WINVM_H

#include <stddef.h>
#include <stdint.h>

/* Builds the board, attaches the media, and starts the CPUs. Every path is a
   file inside the app container. cd0..cd2 may be NULL; a NULL disk0 means an
   empty machine (only useful for a firmware screen).
   0, or -1 with a reason in err. */
int henge_winvm_boot(const char *code_fd, const char *vars_fd, const char *disk0, const char *cd0, const char *cd1,
                     const char *cd2, uint64_t ram_mb, int ncpus, char *err, size_t errlen);

/* A blank NVMe image, sized the way `df` would report it. Sparse: it costs what
   the guest writes, not what it can see. 0, or -1. Existing files are kept. */
int henge_winvm_make_disk(const char *path, uint64_t gb);

/* 0 not started, 1 running, 2 stopped, 3 starting. */
int henge_winvm_state(void);
/* A new generation starts with each successful boot. */
uint64_t henge_winvm_generation(void);
/* Why it stopped last time (a short sentence), or "" if it has not stopped. */
const char *henge_winvm_exit_reason(void);
void henge_winvm_stop(void);
/* ACPI power button: an orderly shutdown, not a kill. */
void henge_winvm_power_button(void);
/* Syncs the guest's writes to the host, for "the guest is probably done" moments. */
void henge_winvm_flush_disks(void);

/* Where the guest's frames go: a CAMetalLayer. Keep it alive while the machine runs.
   Passing NULL detaches (it does not free the layer). */
void henge_winvm_set_display_layer(void *ca_metal_layer);
int henge_winvm_display_size(uint32_t *w, uint32_t *h);
uint64_t henge_winvm_display_frames(void);
/* A PNG of the last frame; 0, or -1 if nothing has been drawn. */
int henge_winvm_save_png(const char *path);

/* A hardware keyboard: iOS hands each key's HID usage straight through. */
void henge_winvm_key_hid(uint8_t usage, int down);
/* The touchscreen as a USB absolute tablet: x, y normalized 0..1; buttons bit0
   left, bit1 right, bit2 middle; wheel -127..127. */
void henge_winvm_tablet(double x, double y, uint8_t buttons, int8_t wheel);
/* Ctrl+Alt+Del, for the sign-in screen (Windows ignores it before sign-in is needed). */
void henge_winvm_ctrl_alt_del(void);

/* Whether the guest has bound the tablet/keyboard and how much it took. */
void henge_winvm_usb_stats(int *kbd_ready, int *tab_ready, uint64_t *delivered);

#endif
