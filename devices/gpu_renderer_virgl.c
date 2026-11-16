#include "gpu_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* virglrenderer as the 3D backend. This is the same library AppleVM ships
   behind its virtio-gpu: it takes a virgl command stream and turns it into
   GL/GLES calls, which on iOS are ANGLE-on-Metal or KosmicKrisp-on-Metal.
   That host-side context creation is the caller's business (it needs EGL and a
   backend choice), because it does not belong in a device model.

   Build with VIRGL=1 to compile this in; without it the file still produces an
   object whose factory returns NULL, so the device stays 2D and nothing about
   the build changes. */

#ifdef HV_HAVE_VIRGL

#include <virglrenderer.h>

#include "virtio_gpu.h"

static void vr_write_fence(void *cookie, uint32_t fence) {
    /* The device answers every request synchronously, so a fence is complete by
       the time the response leaves; remembering the id is only useful for logs. */
    (void)cookie;
    (void)fence;
}

static const struct virgl_renderer_callbacks kCallbacks = {
    .version = 1,
    .write_fence = vr_write_fence,
    .cookie = NULL,
};

static int vr_capset_info(void *ud, uint32_t *id, uint32_t *version, uint32_t *size) {
    (void)ud;
    uint32_t max_ver = 0, max_size = 0;
    /* Capset 1 is virgl (GL). If the renderer was not initialised this returns
       non-zero and the device refuses to advertise the feature. */
    if (virgl_renderer_get_cap_set(1, &max_ver, &max_size) != 0) return -1;
    *id = 1;
    *version = max_ver;
    *size = max_size;
    return 0;
}

static int vr_capset(void *ud, uint32_t id, uint32_t version, uint32_t size, uint8_t *out, uint32_t *out_len) {
    (void)ud;
    if (id != 1) return -1;
    if (out && size) {
        /* fill_caps wants a buffer of the size it reported; the driver asks for
           exactly that. */
        if (virgl_renderer_fill_caps(1, version, out) != 0) return -1;
        *out_len = size;
    } else {
        *out_len = size;
    }
    return 0;
}

static int vr_ctx_create(void *ud, uint32_t ctx_id, const char *debug_name, uint32_t context_init) {
    (void)ud;
    (void)context_init;
    return virgl_renderer_context_create(ctx_id, (uint32_t)strlen(debug_name), debug_name) == 0 ? 0 : -1;
}

static void vr_ctx_destroy(void *ud, uint32_t ctx_id) {
    (void)ud;
    virgl_renderer_context_destroy(ctx_id);
}

static int vr_res_create(void *ud, const HvGpuRes3DInfo *info) {
    (void)ud;
    struct virgl_renderer_resource_create_args args;
    memset(&args, 0, sizeof args);
    args.handle = info->resource_id;
    args.target = info->target;
    args.format = info->format;
    args.bind = info->bind;
    args.width = info->width;
    args.height = info->height;
    args.depth = info->depth;
    args.array_size = info->array_size;
    args.last_level = info->last_level;
    args.nr_samples = info->nr_samples;
    args.flags = info->flags;
    /* No iovec here: the guest's pages arrive through transfer_*_3d, which is
       where virtio-gpu puts them too. */
    return virgl_renderer_resource_create(&args, NULL, 0) == 0 ? 0 : -1;
}

static void vr_res_unref(void *ud, uint32_t resource_id) {
    (void)ud;
    virgl_renderer_resource_unref(resource_id);
}

static struct virgl_box box_of(const uint32_t b[6]) {
    struct virgl_box box;
    box.x = b[0];
    box.y = b[1];
    box.z = b[2];
    box.w = b[3];
    box.h = b[4];
    box.d = b[5];
    return box;
}

static int vr_transfer_to_host(void *ud, uint32_t resource_id, uint64_t offset, uint32_t level, uint32_t stride,
                               uint32_t layer_stride, const uint8_t *src, uint32_t src_len, const uint32_t b[6]) {
    (void)ud;
    struct iovec iov = {.iov_base = (void *)src, .iov_len = src_len};
    struct virgl_box box = box_of(b);
    return virgl_renderer_transfer_write_iov(resource_id, 0, (int)level, stride, layer_stride, &box, offset, &iov,
                                             1) == 0
               ? 0
               : -1;
}

static int vr_transfer_from_host(void *ud, uint32_t resource_id, uint64_t offset, uint32_t level, uint32_t stride,
                                 uint32_t layer_stride, uint8_t *dst, uint32_t dst_len, const uint32_t b[6]) {
    (void)ud;
    struct iovec iov = {.iov_base = dst, .iov_len = dst_len};
    struct virgl_box box = box_of(b);
    return virgl_renderer_transfer_read_iov(resource_id, 0, level, stride, layer_stride, &box, offset, &iov, 1) == 0
               ? 0
               : -1;
}

static int vr_submit(void *ud, uint32_t ctx_id, const uint8_t *cmds, uint32_t len, uint32_t flags) {
    (void)ud;
    (void)flags;
    /* virgl counts in 32-bit words, and the stream the driver sends is already
       word aligned (the device refuses sizes that are not). */
    if (len % 4) return -1;
    return virgl_renderer_submit_cmd((void *)cmds, (int)ctx_id, (int)(len / 4)) == 0 ? 0 : -1;
}

static const HvGpuRenderer kVirgl = {
    .name = "virgl",
    .ud = NULL,
    .capset_info = vr_capset_info,
    .capset = vr_capset,
    .ctx_create = vr_ctx_create,
    .ctx_destroy = vr_ctx_destroy,
    .res_create = vr_res_create,
    .res_unref = vr_res_unref,
    .transfer_to_host = vr_transfer_to_host,
    .transfer_from_host = vr_transfer_from_host,
    .submit = vr_submit,
};

const HvGpuRenderer *hv_gpu_renderer_virgl(void) {
    /* Initialisation is idempotent in practice but virgl does not promise it, so
       do it once. EGL/backend setup must already have happened in the caller. */
    static int tried;
    static int ok;
    if (!tried) {
        tried = 1;
        ok = virgl_renderer_init(NULL, 0, (struct virgl_renderer_callbacks *)&kCallbacks) == 0;
        if (!ok) fprintf(stderr, "virtio-gpu: virgl_renderer_init failed, staying 2D\n");
    }
    return ok ? &kVirgl : NULL;
}

#else /* !HV_HAVE_VIRGL */

const HvGpuRenderer *hv_gpu_renderer_virgl(void) {
    /* No library linked: the honest answer is "no 3D backend". The device then
       never advertises VIRTIO_GPU_F_VIRGL. */
    return NULL;
}

#endif
