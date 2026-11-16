#ifndef HENGEVM_VIRTIO_GPU_H
#define HENGEVM_VIRTIO_GPU_H

#include "fb_display.h"
#include "machine.h"
#include "gpu_renderer.h"
#include "virtio.h"

/* virtio-gpu (device type 16, PCI 1af4:1050 -- the ID viogpudo.inf matches),
   2D only. That is all Windows needs from us: the driver creates a 2D resource,
   attaches guest pages as its backing, transfers rectangles into it and asks the
   device to scan it out. 3D (virgl) is deliberately not advertised; the driver
   then never asks for a capset it cannot get.

   Scanout goes to the same HvFbDisplay ramfb writes, so firmware (GOP/ramfb) and
   Windows (virtio-gpu) present through one path and the iOS side sees no
   difference. */

#define HV_GPU_MAX_RES 24

typedef struct HvGpuRes {
    int used;
    int is3d; /* created by RESOURCE_CREATE_3D: the renderer owns the memory */
    uint32_t id, format, width, height;
    /* 3D-only fields, straight from resource_create_3d. */
    uint32_t target, bind, depth, array_size, last_level, nr_samples, gpu_flags;
    int attached;
    int nseg;
    struct {
        uint64_t addr;
        uint32_t len;
    } seg[8];
    uint8_t *pixels; /* 2D only: width * height * 4 */
} HvGpuRes;

#define HV_GPU_MAX_CTX 8

typedef struct HvVirtioGpu {
    HvVirtio v;
    HvFbDisplay *fb;
    HvGpuRes res[HV_GPU_MAX_RES];
    struct {
        int used;
        uint32_t id;
    } ctx[HV_GPU_MAX_CTX];
    uint32_t scanout_res;
    uint32_t scanout_id;
    /* NULL means 2D only: then VIRTIO_GPU_F_VIRGL is not advertised and the
       driver never sends ctx_create, so nobody ends up with an error 43. */
    const HvGpuRenderer *rend;
} HvVirtioGpu;

HvVirtioGpu *hv_virtio_gpu_create(HvMachine *m, HvPcie *bus, HvFbDisplay *fb);

/* Attach a 3D backend (virglrenderer on the iOS side). Call before the guest
   starts: it turns on the VIRGL feature bit and enables the 3D commands.
   Without one the device is exactly the 2D device it was. */
void hv_virtio_gpu_set_renderer(HvVirtioGpu *g, const HvGpuRenderer *r);

#endif
