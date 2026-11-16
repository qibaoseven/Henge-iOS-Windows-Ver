#ifndef HENGEVM_VIRTIO_GPU_H
#define HENGEVM_VIRTIO_GPU_H

#include "fb_display.h"
#include "machine.h"
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
    uint32_t id, format, width, height;
    int attached;
    int nseg;
    struct {
        uint64_t addr;
        uint32_t len;
    } seg[8];
    uint8_t *pixels; /* width * height * 4 */
} HvGpuRes;

typedef struct HvVirtioGpu {
    HvVirtio v;
    HvFbDisplay *fb;
    HvGpuRes res[HV_GPU_MAX_RES];
    uint32_t scanout_res;
    uint32_t scanout_id;
} HvVirtioGpu;

HvVirtioGpu *hv_virtio_gpu_create(HvMachine *m, HvPcie *bus, HvFbDisplay *fb);

#endif
