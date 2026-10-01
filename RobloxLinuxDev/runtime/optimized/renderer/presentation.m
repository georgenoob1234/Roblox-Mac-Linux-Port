// Initialize at the first device request, after dyld/category startup. Never
// initialize a display merely because a crash helper inherits this dylib.
#import <Foundation/NSObject.h>
#import <Foundation/NSNotification.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

@interface NSDisplay : NSObject
+ (id)currentDisplay;
@end

// Match Track B's Linux CLOCK_BOOTTIME epoch, including suspend. Do not use
// Darling's unstable kern.boottime fallback or pull in the ARM host runtime.
struct TrackAElfCalls { void *(*open)(const char *); int (*close)(void *); void *(*symbol)(void *,const char *); };
extern struct TrackAElfCalls *_elfcalls;
static int (*native_clock)(int,void *);
static int trackb_clock_gettime(clockid_t clock,struct timespec *value) {
    if(clock==CLOCK_MONOTONIC && native_clock) {
        int result=native_clock(7,value);
        if(result<0){errno=-result;return -1;}
        return result;
    }
    errno=ENOSYS;return -1;
}
#include "../../profiler/api.h"
#include "../../../native/frame-trace.h"
#include "../../../native/metal-present.h"

static const struct RbxProfilerAPI *profiler;
int tracka_profiler_enabled(void) {
    const struct RbxProfilerAPI *p=__atomic_load_n(&profiler,__ATOMIC_ACQUIRE);
    return p && p->enabled();
}
void tracka_profiler_metric(unsigned metric,double ms) {
    const struct RbxProfilerAPI *p=__atomic_load_n(&profiler,__ATOMIC_ACQUIRE);
    if(p)p->metric(metric,ms);
}
unsigned tracka_profiler_cpu_begin(unsigned stage) {
 const struct RbxProfilerAPI *p=__atomic_load_n(&profiler,__ATOMIC_ACQUIRE);
 return p && p->version>=2?p->cpu_begin(stage):UINT32_MAX;
}
void tracka_profiler_cpu_end(unsigned parent) {
 const struct RbxProfilerAPI *p=__atomic_load_n(&profiler,__ATOMIC_ACQUIRE);
 if(p && p->version>=2)p->cpu_end(parent);
}
void tracka_profiler_span_begin(struct RbxProfilerSpan *span) {
 const struct RbxProfilerAPI *p=__atomic_load_n(&profiler,__ATOMIC_ACQUIRE);
 *span=(struct RbxProfilerSpan){0};
 if(p && p->version>=6)p->span_begin(span);
}
void tracka_profiler_span_end(const struct RbxProfilerSpan *span,unsigned id,uint64_t bytes,uint64_t object,uint64_t detail) {
 const struct RbxProfilerAPI *p=__atomic_load_n(&profiler,__ATOMIC_ACQUIRE);
 if(p && p->version>=6)p->span_end(span,id,bytes,object,detail);
}
const void *tracka_profiler_allocator(unsigned stage) {
 const struct RbxProfilerAPI *p=__atomic_load_n(&profiler,__ATOMIC_ACQUIRE);
 return p && p->version>=2?p->allocator(stage):NULL;
}
static void (*profile_original_flush)(id,SEL),(*profile_original_render)(id,SEL);
static void profiled_render(id object,SEL selector) {
    int active=tracka_profiler_enabled();uint64_t start=active?frame_now():0;
    unsigned parent=tracka_profiler_cpu_begin(RBX_PROF_COMPOSITE);
    unsigned gpu=profiler->version>=3?profiler->gpu_begin(RBX_PROF_COMPOSITE):0;
    @try { profile_original_render(object,selector); }
    @finally { if(profiler->version>=3)profiler->gpu_end(gpu);tracka_profiler_cpu_end(parent); }
    if(active && tracka_profiler_enabled())tracka_profiler_metric(RBX_PROF_COMPOSITE,(frame_now()-start)/1e6);
}
static void profiled_flush(id object,SEL selector) {
    // CARenderer has finished drawing with its EGL context still current.
    // ImGui's GL backend restores that context's render state before the swap.
    if(profiler->version<8 || !profiler->hosted())profiler->draw();
    int active=tracka_profiler_enabled();uint64_t start=active?frame_now():0;
    unsigned long long serial=gl_swap_serial?gl_swap_serial():0;
    unsigned parent=tracka_profiler_cpu_begin(RBX_PROF_SWAP);
    @try { profile_original_flush(object,selector); }
    @finally { tracka_profiler_cpu_end(parent); }
    if(active && tracka_profiler_enabled() && (!gl_swap_serial || gl_swap_serial()!=serial)){
        tracka_profiler_metric(RBX_PROF_SWAP,(frame_now()-start)/1e6);profiler->frame();
    }
}
static void init_profiler(void) {
    const char *path=getenv("ROBLOX_MAC_WAYLAND_HELPER");if(!path)return;
    void *host=_elfcalls->open(path);
    const struct RbxProfilerAPI *(*get)(void)=host?_elfcalls->symbol(host,"rbx_profiler_api"):NULL;
    const struct RbxProfilerAPI *p=get?get():NULL;
    if(!p || p->version<1 || !native_clock)return;
    // GTK HUDs leave Vulkan active; Cocoa's skipped GL flushes are not frames.
    gl_swap_serial=dlsym(RTLD_DEFAULT,"rbxWaylandGLSwapSerial");
    // Symbol resolution is invoked only on the guest render thread, where
    // Darwin TLS is present, never from the native sampling worker.
    if(p->version>=4)p->set_symbolizer((int(*)(const void*,void*))dladdr);
    Method flush=class_getInstanceMethod(objc_getClass("CALayerContext"),sel_registerName("flush"));
    Method render=class_getInstanceMethod(objc_getClass("CALayerContext"),sel_registerName("render"));
    if(!flush || !render)return;
    __atomic_store_n(&profiler,p,__ATOMIC_RELEASE);
    profile_original_flush=(void*)method_setImplementation(flush,(IMP)profiled_flush);
    profile_original_render=(void*)method_setImplementation(render,(IMP)profiled_render);
}

// Swapchain presentation bypasses CALayerContext.flush: report its frames
// through the renderer's hooks instead. A GTK HUD does not require GL fallback.
struct TrackbPresentHooks { uint64_t (*begin)(void); void (*end)(const void *,uint64_t); int (*glRequired)(void); };
static uint64_t present_begin(void) { return frame_now(); }
static void present_end(const void *context,uint64_t start) {
    uint64_t end=frame_now();
    if(frame_trace && getpid()==frame_trace_owner)fprintf(frame_trace,"%p,%llu,%llu\n",context,(unsigned long long)end,(unsigned long long)(end-start));
    const struct RbxProfilerAPI *p=__atomic_load_n(&profiler,__ATOMIC_ACQUIRE);
    if(p && p->enabled()){p->metric(RBX_PROF_SWAP,(end-start)/1e6);p->frame();}
}
static int (*software_cursor_visible)(void);
static int present_gl_required(void) {
    if(software_cursor_visible && software_cursor_visible())return 1;
    const struct RbxProfilerAPI *p=__atomic_load_n(&profiler,__ATOMIC_ACQUIRE);
    return p && p->version>=5 && p->shown() && (p->version<8 || !p->hosted());
}
static const struct TrackbPresentHooks present_hooks={present_begin,present_end,present_gl_required};

@interface TrackAFrameTrace : NSObject
+ (void)terminate:(NSNotification *)notification;
@end
@implementation TrackAFrameTrace
+ (void)terminate:(NSNotification *)notification { (void)notification; flush_frame_trace(); }
@end

static pthread_once_t initialized=PTHREAD_ONCE_INIT;
static void *metal_function(const char *name);
static void initialize_presentation(void) {
    @autoreleasepool {
        if(![NSDisplay currentDisplay]) { fputs("Track A: display initialization failed\n",stderr); abort(); }
        // Interposition can reach us before Metal's constructor. It initializes
        // Vulkan, which the drawable hooks resolve during installation below.
        metal_function("MTLCreateSystemDefaultDevice");
        if(getenv("TRACKB_FRAME_TRACE") || getenv("ROBLOX_MAC_WAYLAND_HELPER")) {
            void *vdso=_elfcalls->open("linux-vdso.so.1");
            native_clock=vdso?_elfcalls->symbol(vdso,"__vdso_clock_gettime"):NULL;
            if(!native_clock) { fputs("Track A: frame trace clock unavailable\n",stderr); abort(); }
        }
        if(!init_metal_presentation() || !init_frame_trace()) {
            fputs("Track A: optimized presentation initialization failed\n",stderr); abort();
        }
        init_profiler();
        software_cursor_visible=dlsym(RTLD_DEFAULT,"rbxWaylandSoftwareCursorVisible");
        // Without the native clock no trace or profiler exists to report to.
        void (*set_hooks)(const struct TrackbPresentHooks *)=dlsym(RTLD_DEFAULT,"trackb_set_present_hooks");
        if(set_hooks && native_clock)set_hooks(&present_hooks);
        [[NSNotificationCenter defaultCenter] addObserver:[TrackAFrameTrace class]
            selector:@selector(terminate:) name:@"NSApplicationWillTerminateNotification" object:nil];
    }
}
static void prepare_device_request(void) {
    const char *program=getprogname();
    if(program && (!strcmp(program,"RobloxPlayer") || !strcmp(program,"tracka-render-check")))
        pthread_once(&initialized,initialize_presentation);
}
static void *metal_function(const char *name) {
    void *library=dlopen("/System/Library/Frameworks/Metal.framework/Metal",RTLD_LAZY);
    void *function=library?dlsym(library,name):NULL;
    if(!function) { fputs("Track A: Metal device API unavailable\n",stderr); abort(); }
    return function;
}
id MTLCreateSystemDefaultDevice(void) {
    prepare_device_request();
    return ((id(*)(void))metal_function("MTLCreateSystemDefaultDevice"))();
}
id MTLCopyAllDevices(void) {
    prepare_device_request();
    return ((id(*)(void))metal_function("MTLCopyAllDevices"))();
}
id MTLCopyAllDevicesWithObserver(id *observer,id handler) {
    prepare_device_request();
    return ((id(*)(id *,id))metal_function("MTLCopyAllDevicesWithObserver"))(observer,handler);
}
