#include "gpu_renderer.h"

#include <stddef.h>

/* The placeholder backend: it exists so virtio_gpu.c never has to test for NULL
   in a path that only runs when a renderer is attached anyway, and so it is
   obvious what an unimplemented 3D path does -- it refuses, cleanly. */

static int none_capset_info(void *ud, uint32_t *id, uint32_t *version, uint32_t *size) {
    (void)ud;
    (void)id;
    (void)version;
    (void)size;
    return -1;
}

static const HvGpuRenderer kNone = {
    .name = "none",
    .ud = NULL,
    .capset_info = none_capset_info,
    .capset = NULL,
    .ctx_create = NULL,
    .ctx_destroy = NULL,
    .res_create = NULL,
    .res_unref = NULL,
    .transfer_to_host = NULL,
    .transfer_from_host = NULL,
    .submit = NULL,
};

/* Fields above are all function pointers; this keeps the initialiser honest if
   the struct grows. */
_Static_assert(offsetof(HvGpuRenderer, name) == 0, "renderer layout changed");

const HvGpuRenderer *hv_gpu_renderer_none(void) { return &kNone; }
