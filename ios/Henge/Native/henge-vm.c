/* Windows-only build: the macOS guest bridge this file used to be cannot compile
   here -- host/vm.h, host/install.h and the vmapple/virtio hardware it drove were
   removed in the Windows direction (f81479e). The Swift macOS screens still link
   against these entry points, so they stay as honest no-ops: nothing boots, the
   state reads idle, and only the memory probes keep their real behaviour
   (Entitlements reads henge_available_memory at launch). */
#include "henge-vm.h"

#include <stdio.h>
#if __has_include(<os/proc.h>)
#include <os/proc.h>
#else
extern size_t os_proc_available_memory(void); /* host compile check */
#endif

static const char *kNoMacGuest = "macOS guests are not part of this Windows build";

int henge_vm_boot(const char *rom, const char *disk, const char *aux, uint64_t ecid, uint64_t seed,
                  uint64_t ram_mb, int cpus, char *err, size_t errlen) {
    (void)rom; (void)disk; (void)aux; (void)ecid; (void)seed; (void)ram_mb; (void)cpus;
    if (err && errlen) snprintf(err, errlen, "%s", kNoMacGuest);
    return -1;
}

int henge_vm_install(const char *rom, const char *disk, const char *aux, uint64_t ecid, const char *ipsw,
                     const char *workdir, uint64_t ram_mb, int cpus, char *err, size_t errlen) {
    (void)rom; (void)disk; (void)aux; (void)ecid; (void)ipsw; (void)workdir;
    (void)ram_mb; (void)cpus;
    if (err && errlen) snprintf(err, errlen, "%s", kNoMacGuest);
    return -1;
}

int henge_install_step(void) { return 0; }
double henge_install_progress(void) { return 0; }
int henge_make_aux(const char *path) { (void)path; return -1; }

int henge_vm_state(void) { return 2; }
uint64_t henge_vm_generation(void) { return 0; }
int henge_vm_can_start(void) { return 0; }
const char *henge_vm_exit_reason(void) { return kNoMacGuest; }
int henge_vm_exit_outcome(void) { return HENGE_VM_OUTCOME_ERROR; }
void henge_vm_stop(void) {}
int henge_vm_knob(const char *name, int value) { (void)name; (void)value; return 0; }
long henge_vm_balloon(long target_mb) { (void)target_mb; return -1; }

uint64_t henge_vm_blocks(void) { return 0; }
uint64_t henge_vm_net_frames(int sent) { (void)sent; return 0; }
uint64_t henge_vm_dns(int which) { (void)which; return 0; }
int henge_vm_cpus_on(void) { return 0; }
int henge_vm_guest_el0(void) { return 0; }

void henge_vm_share(const char *folder, const char *socket_path, const char *agent) {
    (void)folder; (void)socket_path; (void)agent;
}
void henge_share_set_clipboard(const char *text) { (void)text; }
char *henge_share_take_clipboard(void) { return NULL; }
long henge_share_agent_seen(void) { return 0; }

void henge_vm_set_display(uint32_t width, uint32_t height) { (void)width; (void)height; }
void henge_vm_set_display_mm(uint32_t width_mm, uint32_t height_mm) { (void)width_mm; (void)height_mm; }
void henge_vm_key(uint16_t code, int down) { (void)code; (void)down; }
void henge_vm_key_hid(uint8_t usage, int down) { (void)usage; (void)down; }
void henge_vm_mouse_move(int dx, int dy) { (void)dx; (void)dy; }
void henge_vm_mouse_button(int button, int down) { (void)button; (void)down; }
void henge_vm_mouse_scroll(int dx, int dy) { (void)dx; (void)dy; }
void henge_vm_touch(int finger, int phase, int x, int y, uint32_t ts_ms) {
    (void)finger; (void)phase; (void)x; (void)y; (void)ts_ms;
}
void henge_vm_touch_frame(uint32_t ts_ms) { (void)ts_ms; }
void henge_vm_touch_button(int down) { (void)down; }
int henge_vm_pointer(int *x, int *y) { if (x) *x = 0; if (y) *y = 0; return -1; }
int henge_vm_snapshot(uint8_t *bgra, int width, int height) {
    (void)bgra; (void)width; (void)height; return -1;
}

void henge_vm_set_display_layer(void *layer) { (void)layer; }
uint64_t henge_vm_gpu_frames(void) { return 0; }
int henge_vm_boot_display(const uint8_t **pixels, int *width, int *height, int *stride) {
    if (pixels) *pixels = NULL;
    if (width) *width = 0;
    if (height) *height = 0;
    if (stride) *stride = 0;
    return -1;
}
void henge_vm_free_display(const uint8_t *pixels) { (void)pixels; }

uint64_t henge_largest_reservation_gb(void) { return 0; }
uint64_t henge_largest_mapping_mb(void) { return 0; }
size_t henge_available_memory(void) { return os_proc_available_memory(); }
