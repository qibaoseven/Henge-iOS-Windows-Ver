/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The Metal presenter for the Windows machine's ramfb.
 *
 * devices/fb_display.c owns the scanout: EDK2's GOP writes `etc/ramfb` into fw_cfg,
 * the guest draws into guest RAM, and fb_display hands out a pointer, width, height
 * and stride. This file does the only thing fb_display cannot: put those pixels on
 * the screen. There is no dirty tracking anywhere in the chain (the guest writes
 * wherever it likes), so we poll a cheap signature of the frame and re-upload when
 * it changes; a static desktop then costs nothing.
 *
 * Deliberately has no shaders of its own: one fullscreen triangle sampling a
 * BGRA8 texture, compiled at runtime so this file stays self-contained and there is
 * no .metallib to add to the Xcode project.
 *
 * Ownership: fb->metal holds a __bridge_retained pointer to one presenter. The
 * presenter keeps its own polling thread; set_layer(NULL) parks it. Everything the
 * C side calls is safe from any thread, which is what host/winvm.c needs.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include "fb_display.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Poll cadence. 60 Hz is what the guest's cursor wants; the signature check below
   keeps an idle desktop from uploading anything at all. */
#define HV_FB_TICK_US (1000 * 10)
#define HV_FB_MAX_FPS 60

/* How many uint32 words of the frame go into the change signature. Sampling
   every 512th word is enough to notice a cursor blink and cheap enough (a few
   thousand loads) to run 100 times a second. */
#define HV_FB_SIG_STEP 512

static NSString *const kHvFbVertexSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct VOut { float4 pos [[position]]; float2 uv; };\n"
     "vertex VOut hv_fb_vertex(uint vid [[vertex_id]]) {\n"
     "  float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n"
     "  VOut o;\n"
     "  o.pos = float4(p[vid], 0.0, 1.0);\n"
     "  o.uv = float2((p[vid].x + 1.0) * 0.5, 1.0 - (p[vid].y + 1.0) * 0.5);\n"
     "  return o;\n"
     "}\n"
     "fragment float4 hv_fb_fragment(VOut in [[stage_in]],\n"
     "                               texture2d<float> tex [[texture(0)]]) {\n"
     "  constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
     "  return tex.sample(s, in.uv);\n"
     "}\n";

static void *hv_fb_thread_main(void *ctx);

@interface HvFbMetalPresenter : NSObject
/* The view's layer; weak because the view owns it and may go away while we poll. */
@property (nonatomic, weak) CAMetalLayer *layer;
@property (nonatomic) id<MTLDevice> device;
@property (nonatomic) id<MTLCommandQueue> queue;
@property (nonatomic) id<MTLRenderPipelineState> pipeline;
@property (nonatomic) id<MTLTexture> texture;
@property (nonatomic) NSUInteger texWidth;
@property (nonatomic) NSUInteger texHeight;
@property (nonatomic) CGFloat contentsScale;
@property (atomic) BOOL running;
- (BOOL)buildPipelineWithError:(NSError **)error;
- (void)pollLoop;
@end

static atomic_uint_fast64_t g_frames;

@implementation HvFbMetalPresenter {
    pthread_t _thread;
    BOOL _threadStarted;
    /* Previous signature, so an idle frame costs one scan and no upload. */
    uint64_t _lastSig;
    BOOL _sigValid;
    HvFbDisplay *_fb; /* not retained: fb_display outlives this presenter */
}

- (instancetype)initWithDisplay:(HvFbDisplay *)fb {
    if ((self = [super init])) {
        _fb = fb;
        _device = MTLCreateSystemDefaultDevice();
        _queue = [_device newCommandQueue];
        _contentsScale = 1.0;
        _running = NO;
    }
    return self;
}

- (BOOL)buildPipelineWithError:(NSError **)error {
    if (_pipeline) return YES;
    if (!_device) {
        if (error) {
            *error = [NSError errorWithDomain:@"HvFbMetal"
                                         code:1
                                     userInfo:@{NSLocalizedDescriptionKey:
                                                    @"No Metal device on this host"}];
        }
        return NO;
    }
    NSError *libErr = nil;
    id<MTLLibrary> lib = [_device newLibraryWithSource:kHvFbVertexSource options:nil error:&libErr];
    if (!lib) {
        if (error) *error = libErr;
        return NO;
    }
    id<MTLFunction> vfn = [lib newFunctionWithName:@"hv_fb_vertex"];
    id<MTLFunction> ffn = [lib newFunctionWithName:@"hv_fb_fragment"];
    if (!vfn || !ffn) {
        if (error) {
            *error = [NSError errorWithDomain:@"HvFbMetal"
                                         code:2
                                     userInfo:@{NSLocalizedDescriptionKey:
                                                    @"Shader functions missing from the runtime library"}];
        }
        return NO;
    }
    MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = vfn;
    desc.fragmentFunction = ffn;
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    /* The guest's framebuffer is XR24/AR24: the fourth byte is padding, not alpha,
       so blending must stay off or the whole screen turns transparent. */
    desc.colorAttachments[0].blendingEnabled = NO;
    NSError *pipeErr = nil;
    _pipeline = [_device newRenderPipelineStateWithDescriptor:desc error:&pipeErr];
    if (!_pipeline) {
        if (error) *error = pipeErr;
        return NO;
    }
    return YES;
}

- (void)configureLayer:(CAMetalLayer *)layer {
    if (!layer) return;
    layer.device = _device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.opaque = YES;
    CGFloat scale = layer.contentsScale;
    if (scale <= 0.0) scale = 1.0;
    _contentsScale = scale;
}

/* Signature of the frame, so a static desktop does not re-upload 4 MB forever.
   FNV-1a over sampled words: order sensitive, unlike a plain sum. */
- (uint64_t)signatureForPixels:(const uint8_t *)pixels
                          width:(uint32_t)w
                         height:(uint32_t)h
                         stride:(uint32_t)stride {
    if (!pixels || w == 0 || h == 0 || stride == 0) return 0;
    uint64_t sig = 1469598103934665603ULL;
    uint64_t words = (uint64_t)h * (uint64_t)(stride / 4);
    uint64_t step = HV_FB_SIG_STEP;
    for (uint64_t i = 0; i < words; i += step) {
        uint32_t v = ((const uint32_t *)pixels)[i];
        sig ^= (uint64_t)v;
        sig *= 1099511628211ULL;
    }
    /* Always include the tail: status bars and clocks live down there. */
    sig ^= ((const uint32_t *)pixels)[words - 1];
    sig *= 1099511628211ULL;
    return sig;
}

- (BOOL)ensureTextureForWidth:(uint32_t)w height:(uint32_t)h {
    if (_texture && _texWidth == w && _texHeight == h) return YES;
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                            width:w
                                                           height:h
                                                        mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    /* Private storage needs a blit to fill; Managed would let replaceRegion work
       directly. iOS has no Managed, so upload through a staging buffer instead. */
    if (!td) return NO;
    _texture = [_device newTextureWithDescriptor:td];
    _texWidth = w;
    _texHeight = h;
    return _texture != nil;
}

/* Uploads one rectangle of the frame. A guest with a flush command (virtio-gpu)
   tells us exactly what moved, so a 4 MB frame usually costs a few rows of
   memcpy instead of all of it. */
- (void)uploadRect:(const uint8_t *)pixels
             width:(uint32_t)w
             height:(uint32_t)h
            stride:(uint32_t)stride
                 x:(uint32_t)x
                 y:(uint32_t)y
                rw:(uint32_t)rw
                rh:(uint32_t)rh
             bytes:(void (^)(id<MTLBlitCommandEncoder> blit))encode {
    if (!pixels || !_texture || rw == 0 || rh == 0) return;
    NSUInteger tightRow = (NSUInteger)rw * 4u;
    NSUInteger needed = tightRow * (NSUInteger)rh;
    id<MTLBuffer> staging = [_device newBufferWithLength:needed
                                                options:MTLResourceStorageModeShared];
    if (!staging) return;
    uint8_t *dst = (uint8_t *)staging.contents;
    for (NSUInteger row = 0; row < rh; row++) {
        memcpy(dst + row * tightRow,
               pixels + ((uint64_t)y + row) * stride + (uint64_t)x * 4u,
               tightRow);
    }
    id<MTLCommandBuffer> cb = [_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromBuffer:staging
            sourceOffset:0
        sourceBytesPerRow:tightRow
      sourceBytesPerHeight:rh
               toTexture:_texture
        destinationSlice:0
        destinationLevel:0
     destinationOrigin:MTLOriginMake(x, y, 0)];
    [blit endEncoding];
    encode(cb);
    [cb commit];
    /* Wait, so the staging buffer is not freed while the GPU still reads it. */
    [cb waitUntilCompleted];
}

- (void)presentFrame {
    HvFbDisplay *fb = _fb;
    const uint8_t *pixels = NULL;
    uint32_t w = 0, h = 0, stride = 0;
    if (!fb || hv_fb_display_get_frame(fb, &pixels, &w, &h, &stride) != 0) return;
    if (!pixels || w == 0 || h == 0) return;

    CAMetalLayer *layer = self.layer;
    if (!layer) return;
    if (layer.device != _device) [self configureLayer:layer];

    /* A guest that flushes (virtio-gpu) hands us the rectangle it changed. ramfb
       has no flush at all, so there the signature scan stays as the fallback --
       that is the honest difference between the two devices. */
    uint32_t dx = 0, dy = 0, dw = 0, dh = 0;
    BOOL damaged = hv_fb_display_take_damage(fb, &dx, &dy, &dw, &dh) != 0;
    BOOL resized = (_texture == nil || _texWidth != w || _texHeight != h);
    if (resized) {
        dx = 0;
        dy = 0;
        dw = w;
        dh = h;
    } else if (!damaged) {
        uint64_t sig = [self signatureForPixels:pixels width:w height:h stride:stride];
        if (_sigValid && sig == _lastSig) return; /* nothing changed */
        _lastSig = sig;
        _sigValid = YES;
        dx = 0;
        dy = 0;
        dw = w;
        dh = h;
    } else {
        _sigValid = NO; /* the signature no longer describes the texture */
    }
    if (dx >= w || dy >= h) return;
    if (dx + dw > w) dw = w - dx;
    if (dy + dh > h) dh = h - dy;
    if (dw == 0 || dh == 0) return;

    if (![self ensureTextureForWidth:w height:h]) return;

    CGFloat scale = _contentsScale;
    CGSize want = CGSizeMake((CGFloat)w * scale, (CGFloat)h * scale);
    if (!CGSizeEqualToSize(layer.drawableSize, want)) layer.drawableSize = want;

    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (!drawable) return; /* window minimised or occluded */

    [self uploadRect:pixels
                width:w
               height:h
               stride:stride
                    x:dx
                    y:dy
                   rw:dw
                   rh:dh
                bytes:^(id<MTLCommandBuffer> cb){}];

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

    id<MTLCommandBuffer> cb = [_queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
    if (enc && _pipeline) {
        [enc setRenderPipelineState:_pipeline];
        [enc setFragmentTexture:_texture atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [enc endEncoding];
    [cb presentDrawable:drawable];
    [cb commit];
    atomic_fetch_add(&g_frames, 1);
}

- (void)pollLoop {
    while (self.running) {
        @autoreleasepool {
            @try {
                [self presentFrame];
            } @catch (NSException *e) {
                /* A detached layer during rotation is not fatal; keep polling. */
            }
        }
        usleep(HV_FB_TICK_US);
    }
}

- (void)startThread {
    if (_threadStarted) return;
    self.running = YES;
    /* Retained for the thread's lifetime; released in stopThread. */
    void *ctx = (__bridge_retained void *)self;
    if (pthread_create(&_thread, NULL, hv_fb_thread_main, ctx) != 0) {
        CFRelease(ctx);
        self.running = NO;
        return;
    }
    _threadStarted = YES;
}

- (void)stopThread {
    if (!_threadStarted) return;
    self.running = NO;
    pthread_join(_thread, NULL);
    _threadStarted = NO;
    _sigValid = NO;
}

@end

/* Trampoline: the retain count handed over in startThread is released here. */
static void *hv_fb_thread_main(void *ctx) {
    HvFbMetalPresenter *p = (__bridge_transfer HvFbMetalPresenter *)ctx;
    [p pollLoop];
    return NULL;
}

/* ---- C interface used by fb_display.c and host/winvm.c ---------------------- */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void hv_fb_metal_set_layer(HvFbDisplay *fb, void *ca_metal_layer) {
    if (!fb) return;
    pthread_mutex_lock(&g_lock);
    HvFbMetalPresenter *p = (__bridge HvFbMetalPresenter *)fb->metal;
    if (!p) {
        if (!ca_metal_layer) {
            pthread_mutex_unlock(&g_lock);
            return;
        }
        p = [[HvFbMetalPresenter alloc] initWithDisplay:fb];
        NSError *err = nil;
        if (![p buildPipelineWithError:&err]) {
            NSLog(@"[fb_metal] Metal unavailable: %@", err.localizedDescription);
            pthread_mutex_unlock(&g_lock);
            return;
        }
        [p configureLayer:(__bridge CAMetalLayer *)ca_metal_layer];
        p.layer = (__bridge CAMetalLayer *)ca_metal_layer;
        [p startThread];
        /* Kept alive for the life of the machine; never handed back by the C API
           because the display is torn down with the process. */
        fb->metal = (__bridge_retained void *)p;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (!ca_metal_layer) {
        [p stopThread];
        p.layer = nil;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    [p configureLayer:(__bridge CAMetalLayer *)ca_metal_layer];
    p.layer = (__bridge CAMetalLayer *)ca_metal_layer;
    if (!p.running) [p startThread];
    pthread_mutex_unlock(&g_lock);
}

uint64_t hv_fb_metal_frames(HvFbDisplay *fb) {
    (void)fb;
    return atomic_load(&g_frames);
}

int hv_fb_save_png(HvFbDisplay *fb, const char *path) {
    if (!fb || !path) return -1;
    const uint8_t *pixels = NULL;
    uint32_t w = 0, h = 0, stride = 0;
    if (hv_fb_display_get_frame(fb, &pixels, &w, &h, &stride) != 0) return -1;
    if (!pixels || w == 0 || h == 0) return -1;

    NSUInteger row = (NSUInteger)w * 4u;
    size_t len = row * h;
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) return -1;
    for (uint32_t y = 0; y < h; y++) {
        memcpy(copy + (NSUInteger)y * row, pixels + (uint64_t)y * stride, row);
    }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    /* BGRX: skip the fourth byte rather than pretend it is alpha. */
    CGContextRef ctx = CGBitmapContextCreate(copy, w, h, 8, row, cs,
                                             kCGImageAlphaNoneSkipFirst |
                                                 kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (!ctx) {
        free(copy);
        return -1;
    }
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    free(copy);
    if (!img) return -1;

    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    CGImageDestinationRef dest =
        CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, NULL);
    if (!dest) {
        CGImageRelease(img);
        return -1;
    }
    CGImageDestinationAddImage(dest, img, NULL);
    bool ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    CGImageRelease(img);
    return ok ? 0 : -1;
}
