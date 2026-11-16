#include "virtio_gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the wire format (virtio spec, "virtio-gpu" section) ---- */

#define GPU_CMD_GET_DISPLAY_INFO 0x0100
#define GPU_CMD_RESOURCE_CREATE_2D 0x0101
#define GPU_CMD_RESOURCE_UNREF 0x0102
#define GPU_CMD_SET_SCANOUT 0x0103
#define GPU_CMD_RESOURCE_FLUSH 0x0104
#define GPU_CMD_TRANSFER_TO_HOST_2D 0x0105
#define GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define GPU_CMD_RESOURCE_DETACH_BACKING 0x0107
#define GPU_CMD_GET_CAPSET_INFO 0x0108
#define GPU_CMD_GET_CAPSET 0x0109
#define GPU_CMD_GET_EDID 0x010a

#define GPU_RESP_OK_NODATA 0x1100
#define GPU_RESP_OK_DISPLAY_INFO 0x1101
#define GPU_RESP_OK_CAPSET_INFO 0x1102
#define GPU_RESP_OK_CAPSET 0x1103
#define GPU_RESP_OK_EDID 0x1104
#define GPU_RESP_ERR_UNSPEC 0x1200
#define GPU_RESP_ERR_OUT_OF_RESOURCES 0x1201
#define GPU_RESP_ERR_INVALID_PARAM 0x1202
#define GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define GPU_RESP_ERR_INVALID_CONTEXT_ID 0x1204
#define GPU_RESP_ERR_INVALID_COMMAND 0x1205

#define GPU_MAX_DIM 4096u
#define GPU_MAX_BYTES (64u << 20) /* a 2D resource bigger than this is a driver bug */

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t pad[3];
} GpuHdr;

typedef struct __attribute__((packed)) {
    uint32_t x, y, width, height;
} GpuRect;

typedef struct __attribute__((packed)) {
    GpuHdr hdr;
    GpuRect rect;
} GpuRectCmd;

typedef struct __attribute__((packed)) {
    GpuHdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} GpuCreate2DCmd;

typedef struct __attribute__((packed)) {
    GpuHdr hdr;
    GpuRect rect;
    uint32_t scanout_id;
    uint32_t resource_id;
} GpuSetScanoutCmd;

typedef struct __attribute__((packed)) {
    GpuHdr hdr;
    GpuRect rect;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} GpuTransferCmd;

typedef struct __attribute__((packed)) {
    GpuHdr hdr;
} GpuSimpleCmd;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} GpuMemEntry;

typedef struct __attribute__((packed)) {
    GpuHdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} GpuAttachCmd;

typedef struct __attribute__((packed)) {
    GpuRect rect;
    uint32_t enabled;
    uint32_t flags;
} GpuDisplayOne;

typedef struct __attribute__((packed)) {
    GpuHdr hdr;
    GpuDisplayOne pmodes[16];
} GpuDisplayInfoResp;

typedef struct __attribute__((packed)) {
    GpuHdr hdr;
    uint32_t capset_id;
    uint32_t capset_max_version;
    uint32_t capset_max_size;
    uint32_t padding;
} GpuCapsetInfoResp;

/* The wire layout is fixed by the spec; a packing mistake here shows up as a
   guest that creates a resource of the wrong size, so check it at compile time. */
_Static_assert(sizeof(GpuHdr) == 24, "virtio_gpu_ctrl_hdr is 24 bytes");
_Static_assert(sizeof(GpuRect) == 16, "virtio_gpu_rect is 16 bytes");
_Static_assert(sizeof(GpuRectCmd) == 40, "rect command is 40 bytes");
_Static_assert(sizeof(GpuCreate2DCmd) == 40, "resource_create_2d is 40 bytes");
_Static_assert(sizeof(GpuSetScanoutCmd) == 48, "set_scanout is 48 bytes");
_Static_assert(sizeof(GpuTransferCmd) == 56, "transfer_to_host_2d is 56 bytes");
_Static_assert(sizeof(GpuAttachCmd) == 32, "resource_attach_backing is 32 bytes");
_Static_assert(sizeof(GpuMemEntry) == 16, "mem_entry is 16 bytes");
_Static_assert(sizeof(GpuDisplayInfoResp) == 408, "display info carries 16 pmodes");

/* Formats we actually store. The guest's buffers are always 32 bits per pixel. */
#define GPU_FMT_B8G8R8A8 1
#define GPU_FMT_B8G8R8X8 2
#define GPU_FMT_A8R8G8B8 3
#define GPU_FMT_X8R8G8B8 4
#define GPU_FMT_R8G8B8A8 67
#define GPU_FMT_X8R8G8B8_ALT 68 /* not a typo: the spec lists both, same bytes */

#define DRM_FORMAT_XRGB8888 0x34325258u /* 'XR24': what fb_metal expects */

static int fourcc_for(uint32_t format, uint32_t *fourcc) {
    switch (format) {
    case GPU_FMT_B8G8R8A8:
    case GPU_FMT_B8G8R8X8:
        *fourcc = DRM_FORMAT_XRGB8888;
        return 0;
    default:
        return -1; /* the others are byte-swapped; refuse rather than show wrong colours */
    }
}

static void gpu_hdr(GpuHdr *h, uint32_t type, const GpuHdr *req) {
    memset(h, 0, sizeof *h);
    h->type = type;
    h->flags = req ? req->flags : 0;
    h->fence_id = req ? req->fence_id : 0;
    h->ctx_id = req ? req->ctx_id : 0;
}

/* Reads `len` bytes of a resource's backing, starting at linear offset `off`. */
static int res_read(HvVirtioGpu *g, HvGpuRes *r, uint64_t off, uint8_t *dst, uint32_t len) {
    uint32_t done = 0;
    for (int s = 0; s < r->nseg && done < len; s++) {
        if (off >= r->seg[s].len) {
            off -= r->seg[s].len;
            continue;
        }
        uint32_t n = r->seg[s].len - (uint32_t)off;
        if (n > len - done) n = len - done;
        uint8_t *p = hv_phys_ptr(g->v.m, r->seg[s].addr + off, n);
        if (!p) return -1;
        memcpy(dst + done, p, n);
        done += n;
        off = 0;
    }
    return done == len ? 0 : -1;
}

static HvGpuRes *find(HvVirtioGpu *g, uint32_t id) {
    for (int i = 0; i < HV_GPU_MAX_RES; i++)
        if (g->res[i].used && g->res[i].id == id) return &g->res[i];
    return NULL;
}

static void res_free(HvGpuRes *r) {
    free(r->pixels);
    memset(r, 0, sizeof *r);
}

static uint32_t handle(HvVirtioGpu *g, const HvVirtqElem *e, GpuHdr *resp, uint32_t resp_cap) {
    GpuHdr req;
    if (hv_virtq_read(&g->v, e, 0, &req, sizeof req) != sizeof req) {
        gpu_hdr(resp, GPU_RESP_ERR_UNSPEC, NULL);
        return sizeof(GpuHdr);
    }
    uint32_t type = req.type;

    switch (type) {
    case GPU_CMD_GET_DISPLAY_INFO: {
        GpuDisplayInfoResp *r = (GpuDisplayInfoResp *)resp;
        gpu_hdr(&r->hdr, GPU_RESP_OK_DISPLAY_INFO, &req);
        r->pmodes[0].rect.x = 0;
        r->pmodes[0].rect.y = 0;
        r->pmodes[0].rect.width = g->fb ? g->fb->width : 1280;
        r->pmodes[0].rect.height = g->fb ? g->fb->height : 800;
        r->pmodes[0].enabled = 1;
        if (sizeof *r > resp_cap) {
            gpu_hdr(resp, GPU_RESP_ERR_UNSPEC, &req);
            return sizeof(GpuHdr);
        }
        return sizeof *r;
    }
    case GPU_CMD_GET_CAPSET_INFO: {
        GpuCapsetInfoResp *r = (GpuCapsetInfoResp *)resp;
        gpu_hdr(&r->hdr, GPU_RESP_OK_CAPSET_INFO, &req);
        r->capset_id = 0;
        r->capset_max_version = 0;
        r->capset_max_size = 0;
        return sizeof *r;
    }
    case GPU_CMD_GET_EDID:
        /* We do not advertise VIRTIO_GPU_F_EDID, so this should never arrive; if
           it does, saying "no EDID" is better than sending a made-up one. */
        gpu_hdr(resp, GPU_RESP_ERR_UNSPEC, &req);
        return sizeof(GpuHdr);

    case GPU_CMD_RESOURCE_CREATE_2D: {
        GpuCreate2DCmd c;
        if (hv_virtq_read(&g->v, e, 0, &c, sizeof c) != sizeof c) {
            gpu_hdr(resp, GPU_RESP_ERR_UNSPEC, &req);
            return sizeof(GpuHdr);
        }
        uint32_t fourcc;
        if (fourcc_for(c.format, &fourcc) != 0 || !c.width || !c.height || c.width > GPU_MAX_DIM ||
            c.height > GPU_MAX_DIM) {
            gpu_hdr(resp, GPU_RESP_ERR_INVALID_PARAM, &req);
            return sizeof(GpuHdr);
        }
        if ((uint64_t)c.width * c.height * 4 > GPU_MAX_BYTES) {
            gpu_hdr(resp, GPU_RESP_ERR_OUT_OF_RESOURCES, &req);
            return sizeof(GpuHdr);
        }
        if (find(g, c.resource_id)) {
            gpu_hdr(resp, GPU_RESP_ERR_INVALID_RESOURCE_ID, &req);
            return sizeof(GpuHdr);
        }
        HvGpuRes *slot = NULL;
        for (int i = 0; i < HV_GPU_MAX_RES && !slot; i++)
            if (!g->res[i].used) slot = &g->res[i];
        if (!slot) {
            gpu_hdr(resp, GPU_RESP_ERR_OUT_OF_RESOURCES, &req);
            return sizeof(GpuHdr);
        }
        uint8_t *pix = calloc((size_t)c.width * c.height, 4);
        if (!pix) {
            gpu_hdr(resp, GPU_RESP_ERR_OUT_OF_RESOURCES, &req);
            return sizeof(GpuHdr);
        }
        *slot = (HvGpuRes){.used = 1, .id = c.resource_id, .format = c.format,
                           .width = c.width, .height = c.height, .pixels = pix};
        gpu_hdr(resp, GPU_RESP_OK_NODATA, &req);
        return sizeof(GpuHdr);
    }

    case GPU_CMD_RESOURCE_UNREF: {
        GpuSimpleCmd c; /* resource_id sits where ctx_id would; read it separately */
        uint32_t id = 0;
        if (hv_virtq_read(&g->v, e, offsetof(GpuCreate2DCmd, resource_id), &id, sizeof id) != sizeof id) {
            gpu_hdr(resp, GPU_RESP_ERR_UNSPEC, &req);
            return sizeof(GpuHdr);
        }
        (void)c;
        HvGpuRes *r = find(g, id);
        if (!r) {
            gpu_hdr(resp, GPU_RESP_ERR_INVALID_RESOURCE_ID, &req);
            return sizeof(GpuHdr);
        }
        if (g->scanout_res == id) {
            g->scanout_res = 0; /* the scanout keeps the last pixels; nothing to show */
            if (g->fb) hv_fb_display_set_scanout(g->fb, NULL, 0, 0, 0, 0);
        }
        res_free(r);
        gpu_hdr(resp, GPU_RESP_OK_NODATA, &req);
        return sizeof(GpuHdr);
    }

    case GPU_CMD_RESOURCE_ATTACH_BACKING: {
        GpuAttachCmd c;
        if (hv_virtq_read(&g->v, e, 0, &c, sizeof c) != sizeof c) {
            gpu_hdr(resp, GPU_RESP_ERR_UNSPEC, &req);
            return sizeof(GpuHdr);
        }
        HvGpuRes *r = find(g, c.resource_id);
        if (!r) {
            gpu_hdr(resp, GPU_RESP_ERR_INVALID_RESOURCE_ID, &req);
            return sizeof(GpuHdr);
        }
        if (c.nr_entries < 1 || c.nr_entries > 8) {
            gpu_hdr(resp, GPU_RESP_ERR_INVALID_PARAM, &req);
            return sizeof(GpuHdr);
        }
        uint64_t total = 0;
        for (uint32_t i = 0; i < c.nr_entries; i++) {
            GpuMemEntry m;
            uint32_t off = (uint32_t)sizeof c + i * (uint32_t)sizeof m;
            if (hv_virtq_read(&g->v, e, off, &m, sizeof m) != sizeof m) {
                gpu_hdr(resp, GPU_RESP_ERR_UNSPEC, &req);
                return sizeof(GpuHdr);
            }
            if (!hv_phys_ptr(g->v.m, m.addr, m.length)) {
                gpu_hdr(resp, GPU_RESP_ERR_INVALID_PARAM, &req);
                return sizeof(GpuHdr);
            }
            r->seg[i].addr = m.addr;
            r->seg[i].len = m.length;
            total += m.length;
        }
        r->nseg = (int)c.nr_entries;
        r->attached = 1;
        if (total < (uint64_t)r->width * r->height * 4) {
            /* The driver may attach less than the full image and grow it later
               with another attach; treat it as usable but remember it is short. */
            fprintf(stderr, "virtio-gpu: resource %u attached %llu bytes, image needs %llu\n", r->id,
                    (unsigned long long)total, (unsigned long long)((uint64_t)r->width * r->height * 4));
        }
        gpu_hdr(resp, GPU_RESP_OK_NODATA, &req);
        return sizeof(GpuHdr);
    }

    case GPU_CMD_RESOURCE_DETACH_BACKING: {
        uint32_t id = 0;
        if (hv_virtq_read(&g->v, e, offsetof(GpuCreate2DCmd, resource_id), &id, sizeof id) != sizeof id) {
            gpu_hdr(resp, GPU_RESP_ERR_UNSPEC, &req);
            return sizeof(GpuHdr);
        }
        HvGpuRes *r = find(g, id);
        if (!r) {
            gpu_hdr(resp, GPU_RESP_ERR_INVALID_RESOURCE_ID, &req);
            return sizeof(GpuHdr);
        }
        r->nseg = 0;
        r->attached = 0;
        gpu_hdr(resp, GPU_RESP_OK_NODATA, &req);
        return sizeof(GpuHdr);
    }

    case GPU_CMD_TRANSFER_TO_HOST_2D: {
        GpuTransferCmd c;
        if (hv_virtq_read(&g->v, e, 0, &c, sizeof c) != sizeof c) {
            gpu_hdr(resp, GPU_RESP_ERR_UNSPEC, &req);
            return sizeof(GpuHdr);
        }
        HvGpuRes *r = find(g, c.resource_id);
        if (!r || !r->attached) {
            gpu_hdr(resp, GPU_RESP_ERR_INVALID_RESOURCE_ID, &req);
            return sizeof(GpuHdr);
        }
        if (c.rect.x + c.rect.width > r->width || c.rect.y + c.rect.height > r->height ||
            c.rect.width == 0 || c.rect.height == 0) {
            gpu_hdr(resp, GPU_RESP_ERR_INVALID_PARAM, &req);
            return sizeof(GpuHdr);
        }
        /* Row by row: the whole rectangle is four bytes per pixel, and the
           resource is packed tightly, so one linear read per row does it. */
        uint32_t row_bytes = c.rect.width * 4;
        for (uint32_t y = 0; y < c.rect.height; y++) {
            uint64_t off = c.offset + ((uint64_t)(c.rect.y + y) * r->width + c.rect.x) * 4;
            uint8_t *dst = r->pixels + ((uint64_t)(c.rect.y + y) * r->width + c.rect.x) * 4;
            if (res_read(g, r, off, dst, row_bytes) != 0) {
                gpu_hdr(resp, GPU_RESP_ERR_UNSPEC, &req);
                return sizeof(GpuHdr);
            }
        }
        gpu_hdr(resp, GPU_RESP_OK_NODATA, &req);
        return sizeof(GpuHdr);
    }

    case GPU_CMD_SET_SCANOUT: {
        GpuSetScanoutCmd c;
        if (hv_virtq_read(&g->v, e, 0, &c, sizeof c) != sizeof c) {
            gpu_hdr(resp, GPU_RESP_ERR_UNSPEC, &req);
            return sizeof(GpuHdr);
        }
        if (c.scanout_id != 0) {
            gpu_hdr(resp, GPU_RESP_ERR_INVALID_PARAM, &req);
            return sizeof(GpuHdr);
        }
        HvGpuRes *r = find(g, c.resource_id);
        if (!r) {
            gpu_hdr(resp, GPU_RESP_ERR_INVALID_RESOURCE_ID, &req);
            return sizeof(GpuHdr);
        }
        uint32_t fourcc;
        if (fourcc_for(r->format, &fourcc) != 0) {
            gpu_hdr(resp, GPU_RESP_ERR_INVALID_PARAM, &req);
            return sizeof(GpuHdr);
        }
        g->scanout_res = r->id;
        g->scanout_id = c.scanout_id;
        if (g->fb)
            hv_fb_display_set_scanout(g->fb, r->pixels + ((uint64_t)c.rect.y * r->width + c.rect.x) * 4,
                                      r->width, r->height, r->width * 4, fourcc);
        fprintf(stderr, "virtio-gpu: scanout 0 <- resource %u (%ux%u)\n", r->id, r->width, r->height);
        gpu_hdr(resp, GPU_RESP_OK_NODATA, &req);
        return sizeof(GpuHdr);
    }

    case GPU_CMD_RESOURCE_FLUSH: {
        GpuRectCmd c;
        if (hv_virtq_read(&g->v, e, 0, &c, sizeof c) != sizeof c) {
            gpu_hdr(resp, GPU_RESP_ERR_UNSPEC, &req);
            return sizeof(GpuHdr);
        }
        /* `resource_flush` carries the resource id right after the rect. */
        uint32_t id = 0;
        if (hv_virtq_read(&g->v, e, (uint32_t)sizeof c, &id, sizeof id) != sizeof id) {
            gpu_hdr(resp, GPU_RESP_ERR_UNSPEC, &req);
            return sizeof(GpuHdr);
        }
        HvGpuRes *r = find(g, id);
        if (!r) {
            gpu_hdr(resp, GPU_RESP_ERR_INVALID_RESOURCE_ID, &req);
            return sizeof(GpuHdr);
        }
        /* The scanout points straight at the resource, so the pixels are already
           where the presenter reads them; the flush only has to tell a dirty
           rectangle it can ignore. */
        gpu_hdr(resp, GPU_RESP_OK_NODATA, &req);
        return sizeof(GpuHdr);
    }

    default:
        if (type >= 0x0200 && type <= 0x0203) { /* cursor queue commands */
            gpu_hdr(resp, GPU_RESP_OK_NODATA, &req);
            return sizeof(GpuHdr);
        }
        fprintf(stderr, "virtio-gpu: unknown command %#x (%u)\n", type, type);
        gpu_hdr(resp, GPU_RESP_ERR_INVALID_COMMAND, &req);
        return sizeof(GpuHdr);
    }
}

static void drain(HvVirtioGpu *g, int qi) {
    HvVirtqElem e;
    int got;
    while ((got = hv_virtq_pop(&g->v, qi, &e)) > 0) {
        GpuHdr resp;
        uint32_t cap = hv_virtq_writable(&e);
        uint32_t written = handle(g, &e, &resp, cap);
        hv_virtq_write(&g->v, &e, 0, &resp, written);
        hv_virtq_push(&g->v, qi, &e, written);
        hv_virtio_interrupt(&g->v);
    }
    if (got < 0) hv_virtq_push(&g->v, qi, &e, 0);
}

static void on_notify(HvVirtio *v, int q) {
    HvVirtioGpu *g = v->dev;
    if (q == 0 || q == 1) drain(g, q);
}

HvVirtioGpu *hv_virtio_gpu_create(HvMachine *m, HvPcie *bus, HvFbDisplay *fb) {
    HvVirtioGpu *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->fb = fb;
    /* Device type 16 -> PCI 1af4:1050, which is what viogpudo.inf lists.
       Class 0x03/0x80: display, other (what QEMU's virtio-gpu-pci reports). */
    hv_virtio_init(&g->v, m, 16, 0x0380, 2, HV_VIRTIO_F_VERSION_1);
    g->v.dev = g;
    g->v.notify = on_notify;
    if (hv_pcie_attach(bus, &g->v.pci) < 0) {
        free(g);
        return NULL;
    }
    fprintf(stderr, "virtio-gpu: 1af4:1050 attached, 2D only (%d slots)\n", HV_GPU_MAX_RES);
    return g;
}
