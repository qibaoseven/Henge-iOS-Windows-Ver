#ifndef HENGEVM_GPU_RENDERER_H
#define HENGEVM_GPU_RENDERER_H

#include <stdint.h>

/* The 3D half of virtio-gpu, as a pluggable backend.

   The device side (devices/virtio_gpu.c) speaks the virtio protocol: it parses
   ctx_create / resource_create_3d / transfer / submit_3d, checks ids, and moves
   bytes in and out of guest memory. What it cannot do itself is translate a
   virgl command stream into Metal, which is what virglrenderer does -- the same
   library AppleVM (via UTM's Neptune build) puts behind this device.

   So the boundary is here: a renderer gets a context id and a command buffer,
   and must not know anything about virtqueues. Implementing this against
   virglrenderer is ~200 lines of calls (virgl_renderer_init,
   virgl_renderer_resource_create, virgl_renderer_submit_cmd, ...) and needs the
   library linked, which this tree does not have yet; hv_gpu_renderer_none below
   is the honest placeholder until it does.

   Guests only see a working 3D path once a renderer is attached: without one,
   VIRTIO_GPU_F_VIRGL is not advertised and every 3D command is refused. */

typedef struct HvGpuRes3DInfo {
    uint32_t resource_id;
    uint32_t target;      /* PIPE_TEXTURE_2D, PIPE_BUFFER, ... */
    uint32_t format;      /* PIPE_FORMAT_* */
    uint32_t bind;        /* PIPE_BIND_* */
    uint32_t width, height, depth;
    uint32_t array_size, last_level, nr_samples, flags;
} HvGpuRes3DInfo;

typedef struct HvGpuRenderer {
    const char *name;
    void *ud;

    /* What capset this backend speaks (virgl: id 1, version 1, size ~= 3084
       bytes of caps). GET_CAPSET_INFO / GET_CAPSET hand this to the driver. */
    int (*capset_info)(void *ud, uint32_t *id, uint32_t *version, uint32_t *size);
    int (*capset)(void *ud, uint32_t id, uint32_t version, uint32_t size, uint8_t *out, uint32_t *out_len);

    int (*ctx_create)(void *ud, uint32_t ctx_id, const char *debug_name, uint32_t context_init);
    void (*ctx_destroy)(void *ud, uint32_t ctx_id);

    int (*res_create)(void *ud, const HvGpuRes3DInfo *info);
    void (*res_unref)(void *ud, uint32_t resource_id);

    /* The device reads guest pages into `src` (a bounce buffer, so the renderer
       never has to know about scatter lists) and hands it over here. */
    int (*transfer_to_host)(void *ud, uint32_t resource_id, uint64_t offset, uint32_t level, uint32_t stride,
                            uint32_t layer_stride, const uint8_t *src, uint32_t src_len, const uint32_t box[6]);
    int (*transfer_from_host)(void *ud, uint32_t resource_id, uint64_t offset, uint32_t level, uint32_t stride,
                              uint32_t layer_stride, uint8_t *dst, uint32_t dst_len, const uint32_t box[6]);

    int (*submit)(void *ud, uint32_t ctx_id, const uint8_t *cmds, uint32_t len, uint32_t flags);
} HvGpuRenderer;

/* The real thing: virglrenderer, if this tree was built with VIRGL=1 and the
   host has already made its EGL context current. Returns NULL otherwise. */
const HvGpuRenderer *hv_gpu_renderer_virgl(void);

/* A renderer that accepts nothing, so the device can stay a pure 2D device
   without a NULL check at every call site. */
const HvGpuRenderer *hv_gpu_renderer_none(void);

#endif
