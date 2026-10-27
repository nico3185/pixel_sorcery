/*
 * present_metal.m -- macOS frame presenter on Metal, paced by the display (PS_PRESENTER=metal).
 *
 * SDL's Metal renderer hands frames to a CAMetalLayer queue and only blocks when the queue
 * is full, so the wait lands in bursts and the frame loop cannot tell when a refresh
 * happens. Here a CVDisplayLink signals every refresh: platform_sdl.c waits for it, sleeps
 * most of the refresh (low latency), runs the frame and presents it, and the drawable is
 * shown on the next refresh. The picture is the emulated frame scaled into the window,
 * letterboxed, nearest-neighbour, like SDL_RenderSetLogicalSize.
 */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreVideo/CoreVideo.h>
#import <AppKit/AppKit.h>
#include <SDL.h>
#include <SDL_metal.h>
#include <SDL_syswm.h>
#include <dispatch/dispatch.h>
#include <stdbool.h>
#include <stdint.h>

static SDL_MetalView s_view;
static CAMetalLayer *s_layer;
static id<MTLDevice> s_dev;
static id<MTLCommandQueue> s_queue;
static id<MTLRenderPipelineState> s_pipe;
static id<MTLSamplerState> s_samp;
static id<MTLTexture> s_tex;
static int s_tw, s_th;
static CVDisplayLinkRef s_link;
static dispatch_semaphore_t s_vbl;
static _Atomic uint64_t s_vcount;          /* refreshes since start */
static SDL_Window *s_win;

static const char *k_shader =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct V { float4 pos [[position]]; float2 uv; };\n"
    "vertex V vs(uint id [[vertex_id]]) {\n"
    "    float2 p = float2((id << 1) & 2, id & 2);\n"          /* one triangle covering the viewport */
    "    V o; o.pos = float4(p * 2.0 - 1.0, 0, 1); o.uv = float2(p.x, 1.0 - p.y); return o; }\n"
    "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "    return float4(t.sample(s, in.uv).rgb, 1); }\n";

static CVReturn on_vbl(CVDisplayLinkRef l, const CVTimeStamp *now, const CVTimeStamp *out,
                       CVOptionFlags fi, CVOptionFlags *fo, void *ctx) {
    (void)l; (void)now; (void)out; (void)fi; (void)fo; (void)ctx;
    s_vcount++;
    dispatch_semaphore_signal(s_vbl);
    return kCVReturnSuccess;
}

/* the display the window is on, for the display link */
static void follow_display(void) {
    SDL_SysWMinfo wm; SDL_VERSION(&wm.version);
    CGDirectDisplayID id = CGMainDisplayID();
    if (SDL_GetWindowWMInfo(s_win, &wm)) {
        NSScreen *sc = wm.info.cocoa.window.screen;
        NSNumber *n = sc.deviceDescription[@"NSScreenNumber"];
        if (n) id = (CGDirectDisplayID)n.unsignedIntValue;
    }
    static CGDirectDisplayID cur;              /* re-targeting resets the link: a refresh is lost */
    if (s_link && id != cur) { CVDisplayLinkSetCurrentCGDisplay(s_link, id); cur = id; }
}

bool metal_present_init(SDL_Window *win) {
    s_win = win;
    s_view = SDL_Metal_CreateView(win);
    if (!s_view) return false;
    s_layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(s_view);
    s_dev = MTLCreateSystemDefaultDevice();
    if (!s_layer || !s_dev) return false;
    s_layer.device = s_dev;
    s_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    s_layer.framebufferOnly = YES;
    s_layer.opaque = YES;                      /* lets macOS scan it out directly in fullscreen (no compositor pass) */
    s_layer.displaySyncEnabled = YES;          /* shown on a refresh: no tearing */
    s_layer.maximumDrawableCount = 3;
    s_queue = [s_dev newCommandQueue];
    NSError *err = nil;
    id<MTLLibrary> lib = [s_dev newLibraryWithSource:@(k_shader) options:nil error:&err];
    if (!lib) { fprintf(stderr, "metal: shader: %s\n", err.localizedDescription.UTF8String); return false; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    s_pipe = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!s_pipe) { fprintf(stderr, "metal: pipeline: %s\n", err.localizedDescription.UTF8String); return false; }
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    s_samp = [s_dev newSamplerStateWithDescriptor:sd];
    s_vbl = dispatch_semaphore_create(0);
    if (CVDisplayLinkCreateWithActiveCGDisplays(&s_link) != kCVReturnSuccess) return false;
    CVDisplayLinkSetOutputCallback(s_link, on_vbl, NULL);
    follow_display();
    CVDisplayLinkStart(s_link);
    return true;
}

/* the display's refresh rate (Hz), 0 if unknown */
double metal_refresh_hz(void) {
    if (!s_link) return 0;
    follow_display();
    CVTime t = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(s_link);
    return t.timeValue ? (double)t.timeScale / (double)t.timeValue : 0;
}

/* block until refresh number n has happened (returns at once if it already has); returns
 * the current count. A frame that ran late then starts right away instead of losing a
 * whole refresh, which waiting for "the next" refresh would cost. */
uint64_t metal_wait_vblank(uint64_t n) {
    while (s_vcount < n)
        dispatch_semaphore_wait(s_vbl, dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC));
    while (dispatch_semaphore_wait(s_vbl, DISPATCH_TIME_NOW) == 0) ;   /* drop the backlog */
    return s_vcount;
}

uint64_t metal_vblank_count(void) { return s_vcount; }

static double s_lat_sum; static long s_lat_n;   /* input poll -> on screen (presentedTime) */
static _Atomic int s_inflight;                  /* presented, not on screen yet */
static long s_drained;                          /* frames not presented to drain a backlog */
/* commit -> on screen: the latest, and the floor (the display path's own delay: ~22 ms
 * fullscreen, ~38 ms in a window), drifting up slowly so a moved window re-learns it */
static double s_c2s_last, s_c2s_floor;
long metal_drained(void) { return s_drained; }

static double s_q_sum; static long s_q_n; static int s_q_max;   /* in flight when presenting */
static double s_sub_sum; static long s_sub_n;   /* commit -> on screen */
void metal_queue_stats(double *avg_inflight, int *max_inflight, double *commit_to_screen_ms) {
    *avg_inflight = s_q_n ? s_q_sum / s_q_n : 0; *max_inflight = s_q_max;
    *commit_to_screen_ms = s_sub_n ? s_sub_sum / s_sub_n : 0;
    s_q_sum = 0; s_q_n = 0; s_q_max = 0; s_sub_sum = 0; s_sub_n = 0;
}

/* average input poll -> on screen (ms) since the last call, from the drawables' presented
 * times: the real figure, where a present call's return says nothing on Metal */
double metal_latency_ms(void) {
    double r = s_lat_n ? s_lat_sum / s_lat_n : 0;
    s_lat_sum = 0; s_lat_n = 0;
    return r;
}

void metal_present_frame(const uint8_t *xrgb, int w, int h, double poll_ms_ago) {
    @autoreleasepool {
        double poll_t = CACurrentMediaTime() - poll_ms_ago / 1000.0;
        if (!s_tex || s_tw != w || s_th != h) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
            td.storageMode = MTLStorageModeShared;
            td.usage = MTLTextureUsageShaderRead;
            s_tex = [s_dev newTextureWithDescriptor:td];
            s_tw = w; s_th = h;
        }
        /* XRGB8888 in memory on little-endian is B,G,R,X: BGRA8 as is */
        [s_tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0
                   withBytes:xrgb bytesPerRow:(NSUInteger)w * 4];
        /* a frame queued behind another adds a refresh to every frame after it: when commit
         * -> screen runs clearly above the display path's own floor, skip presenting this
         * frame (the screen keeps the previous one a refresh longer, once) and the queue
         * is back to its own depth */
        static int cooldown, recent, window, pause;
        if (++window == 600) { window = 0; recent = 0; }
        if (pause > 0) pause--;
        else if (cooldown > 0) cooldown--;
        else if (s_inflight >= 2 ||
                 (s_c2s_last > 0 && s_c2s_floor > 0 && s_c2s_last > s_c2s_floor + 0.7 * 1000.0 / 60.0)) {
            s_drained++; cooldown = 30; s_c2s_last = 0;
            if (++recent > 4) { pause = 600; recent = 0; }   /* this path wants the depth: leave it */
            return;
        }
        int dw, dh;
        SDL_Metal_GetDrawableSize(s_win, &dw, &dh);
        s_layer.drawableSize = CGSizeMake(dw, dh);
        id<CAMetalDrawable> d = [s_layer nextDrawable];
        if (!d) return;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = d.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        double ar = (double)w / h, vw = dw, vh = dh;     /* letterbox, square logical pixels */
        if (vw / vh > ar) vw = vh * ar; else vh = vw / ar;
        [e setViewport:(MTLViewport){ (dw - vw) / 2, (dh - vh) / 2, vw, vh, 0, 1 }];
        [e setRenderPipelineState:s_pipe];
        [e setFragmentTexture:s_tex atIndex:0];
        [e setFragmentSamplerState:s_samp atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
        int q = s_inflight;
        s_q_sum += q; s_q_n++; if (q > s_q_max) s_q_max = q;
        s_inflight++;
        double commit_t = CACurrentMediaTime();
        [d addPresentedHandler:^(id<MTLDrawable> dr) {
            s_inflight--;
            if (dr.presentedTime > 0) {
                s_lat_sum += (dr.presentedTime - poll_t) * 1000.0; s_lat_n++;
                double c2s = (dr.presentedTime - commit_t) * 1000.0;
                s_sub_sum += c2s; s_sub_n++;
                s_c2s_last = c2s;
                s_c2s_floor = s_c2s_floor <= 0 || c2s < s_c2s_floor ? c2s : s_c2s_floor + 0.002;
            }
        }];
        [cb presentDrawable:d];
        [cb commit];
    }
}

void metal_present_shutdown(void) {
    if (s_link) { CVDisplayLinkStop(s_link); CVDisplayLinkRelease(s_link); s_link = NULL; }
    if (s_view) { SDL_Metal_DestroyView(s_view); s_view = NULL; }
}
