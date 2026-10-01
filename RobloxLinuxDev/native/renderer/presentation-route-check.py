#!/usr/bin/env python3
"""Run the real routing/reset/semaphore code without a display or GPU."""
import pathlib
import subprocess
import sys
import tempfile

here = pathlib.Path(__file__).resolve().parent
drawable = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else here / "drawable-reset.mm"
route = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else here / "presentation-route.hpp"
swapchain = pathlib.Path(sys.argv[3]) if len(sys.argv) > 3 else here / "swapchain.cpp"
texture = here / "../../runtime/src/indium/src/indium/texture.cpp"


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


fixture = r'''
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vulkan/vulkan.h>
#include "presentation-route.hpp"
namespace Indium {
struct BinarySemaphore {};
struct PrivateTexture {
    std::mutex _presentationMutex;
    std::shared_ptr<BinarySemaphore> _presentationSemaphore;
    void beginUpdatingPresentationSemaphore(std::shared_ptr<BinarySemaphore>);
    void endUpdatingPresentationSemaphore();
    std::shared_ptr<BinarySemaphore> synchronizePresentation();
};
bool commandTimingEnabled() { return false; }
}
struct CAMetalDrawableActual {
    std::shared_ptr<Indium::PrivateTexture> _texture;
    unsigned _glSemaphore = 0;
    std::shared_ptr<Indium::BinarySemaphore> _semaphore;
    std::function<void()> _wantsToPresentCallback, _didPresentCallback;
    double _presentedTime = 0;
    bool _queued = false;
    void reset();
    void didDrop();
    void didPresent();
};
double CACurrentMediaTime() { return 1; }
std::atomic<bool> presentTimesPending{false};
std::mutex presentTimesMutex;
std::map<const CAMetalDrawableActual*, int> presentTimes;
std::mutex queuedPresentSerialsMutex;
std::map<const CAMetalDrawableActual*, uint64_t> queuedPresentSerials;
'''
texture_source = texture.read_text()
drawable_source = drawable.read_text()
fixture = fixture.replace('#include "presentation-route.hpp"', route.read_text())
fixture += function(drawable_source, "uint64_t takeQueuedPresentSerial(") + "\n"
fixture += function(drawable_source, "uint64_t trackb_next_present_serial()") + "\n"
for signature in (
    "void Indium::PrivateTexture::beginUpdatingPresentationSemaphore(",
    "void Indium::PrivateTexture::endUpdatingPresentationSemaphore(",
    "std::shared_ptr<Indium::BinarySemaphore> Indium::PrivateTexture::synchronizePresentation(",
):
    fixture += function(texture_source, signature) + "\n"
fixture += function(drawable_source, "void CAMetalDrawableActual::reset()")
actual = here / "../../runtime/src/darling/src/external/cocotron/QuartzCore/CAMetalDrawable.mm"
for method in ("didDrop", "didPresent"):
    fixture += function(actual.read_text(), f"void CAMetalDrawableActual::{method}()")

# Probe the actual statements immediately before the Vulkan present call.
# Ownership must already block an older GL flush when the replacement queues.
present = function(drawable_source, "void presentVulkan(")
before_present = present.split("const uint64_t begin =", 1)[1].split("\n", 1)[1].split("using Result =", 1)[0]
fixture += r'''
static bool presentSync(int){return false;}
static void drawableDidPresent(CAMetalDrawableActual*){}
void checkOwnershipBeforePresent() {
    bool owns=false,called=false,pendingHide=true,glSurfaceAttached=true;
    struct Window {
        bool& owns;bool& pendingHide;
        void ownership(int value){owns=value;}
        void takeover(){assert(owns);pendingHide=false;}
    } win{owns,pendingHide};
    struct Presenter {
        bool& owns;bool& called;bool& pendingHide;
        int present(Indium::PrivateTexture&,int,bool,std::function<void()>){
            assert(owns && !pendingHide);called=true;return 0;
        }
    } impl{owns,called,pendingHide};
    auto window=&win;auto presenter=&impl;
    Indium::PrivateTexture texture;int extent=1,layer=1,drawable=1;
    CAMetalDrawableActual* actual=nullptr;
''' + before_present + r'''
    (void)result;assert(called);
}
'''
# The sequence is submission order, not delayed present-callback order.
command = (here / "command-buffer.cpp").read_text()
assert command.index("std::unique_lock queueLock(") < command.index("const uint64_t presentSerial =") < command.index("queueLock.unlock()")
assert "trackb_present_drawable(drawable, presentSerial)" in command
alpha = swapchain.read_text().split("VkCompositeAlphaFlagBitsKHR alpha =", 1)[1].split("// Every copy", 1)[0]
fixture += '''
bool opaqueSwapchain(VkCompositeAlphaFlagsKHR supported) {
    VkSurfaceCapabilitiesKHR capabilities{};
    capabilities.supportedCompositeAlpha = supported;
    VkCompositeAlphaFlagBitsKHR alpha =''' + alpha + '''
    assert(alpha == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);
    return true;
}
'''
wayland = (here / "../../runtime/browser/wayland.cpp").read_text()
fixture += r'''
namespace HandoffCheck {
struct wl_callback {};
struct wl_surface {};
struct wl_proxy {};
struct wl_callback_listener { void (*done)(void*,wl_callback*,uint32_t); };
wl_callback callback, stale;
wl_callback *vulkan_handoff_callback=nullptr;
wl_surface glSurface, vkSurface;
wl_surface *vulkan_surface=&vkSurface;
void *game=(void*)1, *current=nullptr, *subsurface=nullptr, *display=nullptr, *pointer_queue=nullptr;
bool can_present=true, pointer_closing=false;
unsigned attached=0, committed=0, desynced=0, destroyed=0;
constexpr int SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER=0;
void *SDL_GetWindowProperties(void*) { return nullptr; }
void *SDL_GetPointerProperty(void*,int,void*) { return &glSurface; }
void wl_callback_destroy(wl_callback*) { ++destroyed; }
void wl_surface_attach(wl_surface *s,void *buffer,int,int) { assert(s==&glSurface && !buffer);++attached; }
void wl_surface_commit(wl_surface *s) { if(s==&glSurface){assert(attached>committed);++committed;} }
void wl_subsurface_set_desync(void*) { assert(committed>desynced);++desynced; }
void wl_display_flush(void*) {}
wl_callback *wl_surface_frame(wl_surface*) { return &callback; }
void wl_proxy_set_queue(wl_proxy*,void*) {}
void wl_callback_add_listener(wl_callback*,const wl_callback_listener*,void*) {}
template<class F> auto on_ui(F f) { return f(); }
'''
for signature in ("static void cancel_vulkan_handoff()", "static void complete_vulkan_handoff(",
                  'extern "C" void rbx_wayland_vulkan_retire_gl()', 'extern "C" void rbx_wayland_vulkan_cancel_retire_gl()'):
    fixture += function(wayland, signature) + "\n"
fixture += r'''
void check() {
    rbx_wayland_vulkan_retire_gl();
    assert(vulkan_handoff_callback && !attached); // old frame until acknowledgment
    complete_vulkan_handoff(nullptr,&stale,0);assert(!attached);
    complete_vulkan_handoff(nullptr,&callback,0);
    assert(attached==1 && committed==1 && desynced==1 && !vulkan_handoff_callback);
    rbx_wayland_vulkan_retire_gl();
    rbx_wayland_vulkan_cancel_retire_gl(); // fallback starts before acknowledgment
    complete_vulkan_handoff(nullptr,&callback,0);assert(attached==1);
    rbx_wayland_vulkan_retire_gl();current=(void*)1;
    complete_vulkan_handoff(nullptr,&callback,0);assert(attached==1);
}
}
'''
# A skipped Cocoa redraw must not be counted as a frame during Vulkan ownership.
egl = (here / "../../runtime/shims/appkit/egl-config.m").read_text()
fixture += r'''
#include <pthread.h>
#include <unistd.h>
namespace TraceCheck {
static pthread_rwlock_t presentation_lock=PTHREAD_RWLOCK_INITIALIZER;
static pthread_once_t frame_symbols_once=PTHREAD_ONCE_INIT;
static int vulkan_owns_presentation,hide_vulkan_pending,gl_frame_ready;
static __thread unsigned long long gl_swap_serial;
bool visible=true;
int rbxWaylandCanPresent(){return visible;}
void load_frame_symbols(){}
int success(void*){return 0;}
int (*flush_drawable)(void*)=success;
void *(*current_context)()=nullptr,*(*current_display)()=nullptr,*(*current_surface)(int)=nullptr;
unsigned (*swap_buffers)(void*,void*)=nullptr;
void (*hide_vulkan)()=nullptr;
'''
fixture += function(egl, "unsigned long long rbxWaylandGLSwapSerial(") + "\n"
fixture += function(egl, "int CGLFlushDrawable(") + "\n"
fixture += r'''
void check(){
    vulkan_owns_presentation=1;CGLFlushDrawable(nullptr);assert(rbxWaylandGLSwapSerial()==0);
    vulkan_owns_presentation=0;visible=false;CGLFlushDrawable(nullptr);assert(rbxWaylandGLSwapSerial()==0);
    visible=true;CGLFlushDrawable(nullptr);assert(rbxWaylandGLSwapSerial()==1);
    hide_vulkan_pending=1;CGLFlushDrawable(nullptr);assert(rbxWaylandGLSwapSerial()==1);
    gl_frame_ready=1;CGLFlushDrawable(nullptr);assert(rbxWaylandGLSwapSerial()==1); // failed swap
    hide_vulkan_pending=0;CGLFlushDrawable(nullptr);assert(rbxWaylandGLSwapSerial()==2);
}
}
'''
# Exercise the actual shared timeline pool, including texture owners.
fixture += r'''
#include <vector>
constexpr unsigned completionSemaphoreCapacity = 2;
constexpr unsigned RBX_DIAG_COMPLETION_SEMAPHORE=0, RBX_DIAG_SEMAPHORE_POOL_LOCK=1;
namespace RbxProfiler {
struct DiagnosticScope { unsigned detail=0; DiagnosticScope(unsigned,unsigned,const void*,unsigned=0) {} void finish() {} };
}
namespace Indium {
struct PrivateDevice;
struct TimelineSemaphore { std::shared_ptr<PrivateDevice> device; VkSemaphore semaphore; uint64_t count; };
struct PrivateDevice: std::enable_shared_from_this<PrivateDevice> {
    std::mutex recycledCommandMutex;
    std::vector<std::pair<VkSemaphore,uint64_t>> recycledCompletionSemaphores;
    uintptr_t made=0; unsigned destroyed=0;
    TimelineSemaphore getTimelineSemaphore() { return {shared_from_this(),reinterpret_cast<VkSemaphore>(++made),0}; }
    void putTimelineSemaphore(const TimelineSemaphore&) { ++destroyed; }
    std::shared_ptr<TimelineSemaphore> getCompletionSemaphore();
    std::shared_ptr<TimelineSemaphore> getWrappedTimelineSemaphore();
};
}
'''
device_source = (here / "device.cpp").read_text()
for signature in ("std::shared_ptr<Indium::TimelineSemaphore> Indium::PrivateDevice::getCompletionSemaphore()",
                  "std::shared_ptr<Indium::TimelineSemaphore> Indium::PrivateDevice::getWrappedTimelineSemaphore()"):
    fixture += function(device_source, signature) + "\n"
fixture += r'''
void checkTimelineReuse() {
    auto device=std::make_shared<Indium::PrivateDevice>();
    auto texture=device->getWrappedTimelineSemaphore();
    auto handle=texture->semaphore; texture->count=17;
    auto pending=texture; texture.reset();
    auto other=device->getCompletionSemaphore();
    assert(other->semaphore!=handle); // Pending GPU owner prevents reuse.
    pending.reset();
    auto reused=device->getWrappedTimelineSemaphore();
    assert(reused->semaphore==handle && reused->count==17 && device->made==2);
    ++reused->count; reused.reset();
    reused=device->getCompletionSemaphore(); assert(reused->count==18);
    auto overflow=device->getWrappedTimelineSemaphore();
    other.reset(); reused.reset(); overflow.reset();
    assert(device->recycledCompletionSemaphores.size()==2 && device->destroyed==1);
    auto exhausted=device->getWrappedTimelineSemaphore(); exhausted->count=UINT64_MAX; exhausted.reset();
    assert(device->destroyed==2); // Never wrap a timeline counter.
    std::weak_ptr<Indium::PrivateDevice> lifetime=device;
    device.reset(); assert(lifetime.expired()); // Idle handles cannot pin the device.
}
'''
fixture += r'''
std::vector<std::function<void()>> scheduledWork, completedWork;
extern "C" void* dispatch_queue_create(const char*, void*) { return &scheduledWork; }
extern "C" void* dispatch_get_global_queue(long priority, unsigned long flags) {
    assert(!priority && !flags); return &completedWork;
}
extern "C" void dispatch_async_f(void* queue, void* context, void (*work)(void*)) {
    assert(queue==&scheduledWork || queue==&completedWork);
    static_cast<std::vector<std::function<void()>>*>(queue)->push_back([context,work]{work(context);});
}
'''
fixture += function((here / "command-buffer.cpp").read_text(), "void scheduleMetalHandlers(")
fixture += r'''
void checkScheduledDispatch() {
    auto owner=std::make_shared<int>(7); std::weak_ptr<int> lifetime=owner;
    std::vector<int> order;
    scheduleMetalHandlers([owner,&order]{assert(*owner==7);order.push_back(1);});
    scheduleMetalHandlers([&order]{order.push_back(2);});
    owner.reset(); assert(!lifetime.expired() && order.empty());
    for(auto& work:scheduledWork) work(); scheduledWork.clear();
    assert((order==std::vector<int>{1,2}) && lifetime.expired());
}
'''
fixture += r'''
#include <condition_variable>
#include <future>
#include <thread>
constexpr unsigned RBX_DIAG_COMMAND_WAIT=0, RBX_PROF_WAIT=0;
namespace RbxProfiler { struct CpuScope { CpuScope(unsigned) {} }; }
namespace Indium {
enum class CommandSpan { Wait };
int commandTimingStart(){return 0;}
void reportCommandTiming(CommandSpan,int){}
struct PrivateCommandBuffer: std::enable_shared_from_this<PrivateCommandBuffer> {
    std::mutex _mutex;
    std::condition_variable _completedCondvar;
    std::vector<std::function<void(std::shared_ptr<PrivateCommandBuffer>)>> _completedHandlers;
    bool _handlersCompleted=false;
    void finishCompletedHandlers();
    void waitUntilCompleted();
};
}
'''
for signature in ("void Indium::PrivateCommandBuffer::finishCompletedHandlers()",
                  "void Indium::PrivateCommandBuffer::waitUntilCompleted()"):
    fixture += function((here / "command-buffer.cpp").read_text(), signature) + "\n"
fixture += r'''
void checkCompletionDispatch() {
    auto slow=std::make_shared<Indium::PrivateCommandBuffer>();
    auto independent=std::make_shared<Indium::PrivateCommandBuffer>();
    std::promise<void> entered,release;
    auto unblock=release.get_future().share();
    std::vector<int> order;
    slow->_completedHandlers.push_back([&](auto){entered.set_value();unblock.wait();order.push_back(1);});
    slow->_completedHandlers.push_back([&](auto){order.push_back(2);});
    scheduleMetalHandlers([slow]{slow->finishCompletedHandlers();},true);
    scheduleMetalHandlers([independent]{independent->finishCompletedHandlers();},true);
    assert(scheduledWork.empty() && completedWork.size()==2);
    std::thread blocked(completedWork[0]);
    entered.get_future().wait();
    auto waiter=std::async(std::launch::async,[slow]{slow->waitUntilCompleted();});
    assert(waiter.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);
    // A blocked application callback cannot prevent another GPU completion.
    completedWork[1](); independent->waitUntilCompleted();
    assert(independent->_handlersCompleted && !slow->_handlersCompleted);
    release.set_value();blocked.join();waiter.get();completedWork.clear();
    assert(slow->_handlersCompleted && (order==std::vector<int>{1,2}));
}
'''
fixture += r'''
int main() {
    // Completion releases the drawable to a new frame before returning.
    // The old invocation must neither destroy itself nor erase its successor.
    for (auto complete : {&CAMetalDrawableActual::didDrop, &CAMetalDrawableActual::didPresent}) {
        CAMetalDrawableActual reused;
        unsigned first=0, second=0;
        reused._didPresentCallback=[&]{
            ++first;
            reused._didPresentCallback=[&]{++second;};
        };
        (reused.*complete)();
        assert(first==1 && second==0 && reused._didPresentCallback);
        (reused.*complete)();
        (reused.*complete)();
        assert(first==1 && second==1 && !reused._didPresentCallback);
    }
    checkTimelineReuse();
    checkScheduledDispatch();
    checkCompletionDispatch();
    HandoffCheck::check();
    TraceCheck::check();
    checkOwnershipBeforePresent();
    // Exercise every combination of the four Vulkan composite-alpha flags.
    // A surface without OPAQUE must take GL fallback, never expose the underlay.
    for (unsigned flags = 0; flags < 16; ++flags)
        assert(opaqueSwapchain(flags) == bool(flags & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR));
    using Route = PresentationRoute;
    bool vulkanActive = true;
    // Both commands submitted before cursor fallback, so neither has a GL copy.
    const bool submittedGLExports[] = {false, false};
    assert(presentationRoute(submittedGLExports[0], vulkanActive) == Route::Vulkan);
    vulkanActive = false; // First command observes the cursor and begins fallback.
    assert(presentationRoute(submittedGLExports[1], vulkanActive) == Route::Drop);
    assert(presentationRoute(true, vulkanActive) == Route::GL);

    // A GL export can be queued before a newer Vulkan frame, or its commit
    // can pause after submission and only enqueue after fallback has begun.
    const auto oldSerial=trackb_next_present_serial();
    const auto vkSerial=trackb_next_present_serial();
    const auto freshSerial=trackb_next_present_serial();
    assert(oldSerial < vkSerial && vkSerial < freshSerial);
    CAMetalDrawableActual old;
    old._texture=std::make_shared<Indium::PrivateTexture>();
    auto gpuSignal=std::make_shared<Indium::BinarySemaphore>();
    std::weak_ptr<Indium::BinarySemaphore> pendingGPU=gpuSignal;
    old._semaphore=gpuSignal;
    unsigned callbacks=0;
    old._didPresentCallback=[&]{++callbacks;old._semaphore.reset();};
    queuedPresentSerials[&old]=oldSerial;
    auto dequeued=takeQueuedPresentSerial(&old);
    assert(presentationSerialIsStale(dequeued,vkSerial));
    old.didDrop();old.didDrop(); // callback remains one-shot
    assert(callbacks==1 && queuedPresentSerials.empty() && !pendingGPU.expired());
    gpuSignal.reset();assert(pendingGPU.expired());
    vulkanActive=false; // a delayed old export still cannot satisfy fallback
    assert(presentationRoute(true,vulkanActive)==Route::GL);
    assert(presentationSerialIsStale(oldSerial,vkSerial));
    assert(!presentationSerialIsStale(freshSerial,vkSerial));
    // A late Vulkan callback must not replace newer GL content either.
    assert(presentationSerialIsStale(vkSerial,freshSerial));
    queuedPresentSerials[&old]=freshSerial;
    old.reset();assert(queuedPresentSerials.empty());
    vulkanActive = true;
    assert(presentationRoute(true, vulkanActive) == Route::GL);

    CAMetalDrawableActual drawable;
    drawable._texture = std::make_shared<Indium::PrivateTexture>();
    auto pendingCommand = std::make_shared<Indium::BinarySemaphore>();
    std::weak_ptr<Indium::BinarySemaphore> oldExport = pendingCommand;
    drawable._texture->beginUpdatingPresentationSemaphore(pendingCommand);
    drawable._texture->endUpdatingPresentationSemaphore();
    // Generation one rendered but was abandoned without presenting. Reacquire
    // it and submit generation two directly to Vulkan, without another export.
    drawable.reset();
    auto exportForNewFrame = drawable._texture->synchronizePresentation();
    assert(!exportForNewFrame);
    assert(presentationRoute(bool(exportForNewFrame), true) == Route::Vulkan);
    assert(presentationRoute(bool(exportForNewFrame), false) == Route::Drop);
    assert(!oldExport.expired()); // The old GPU submission still owns its signal.
    pendingCommand.reset();
    assert(oldExport.expired());
    // A new GL generation still publishes and consumes its own export once.
    auto fresh = std::make_shared<Indium::BinarySemaphore>();
    drawable._texture->beginUpdatingPresentationSemaphore(fresh);
    drawable._texture->endUpdatingPresentationSemaphore();
    assert(drawable._texture->synchronizePresentation() == fresh);
    assert(!drawable._texture->synchronizePresentation());
    std::puts("PASS: completion callback survives drawable reuse; pre-queue Vulkan ownership; stale GL/Vulkan submission ordering; one-shot drop with pending GPU retention; opaque underlay; export generations; timeline reuse; deferred handler lifetime; independent completions and handler wait ordering");
}
'''
with tempfile.TemporaryDirectory(prefix="presentation-route-") as temporary:
    path = pathlib.Path(temporary)
    source = path / "check.cpp"
    source.write_text(fixture)
    subprocess.run(["c++", "-std=c++17", "-O2", "-pthread", "-I", str(here / "../../runtime/src/Vulkan-Headers-1.3.290/include"), str(source), "-o", str(path / "check")], check=True)
    subprocess.run([str(path / "check")], check=True)

    # Exercise the actual guest presentation hook, including older helper ABIs.
    presentation = (here / "../../runtime/optimized/renderer/presentation.m").read_text()
    flush = function(presentation, "static void profiled_flush(")
    flush = flush.replace("@try", "").replace("@finally", "")
    source.write_text(f'#include "{here / "../../runtime/profiler/api.h"}"\n' + r'''
#include <cassert>
#include <cstdio>
static const RbxProfilerAPI *profiler;
static int (*software_cursor_visible)();
using id=void*;using SEL=void*;
static void (*profile_original_flush)(id,SEL);
static unsigned long long swaps,(*gl_swap_serial)();
static unsigned frames,draws,metrics;
static int tracka_profiler_enabled(){return profiler->enabled();}
static uint64_t frame_now(){return 1;}
static unsigned tracka_profiler_cpu_begin(unsigned){return 0;}
static void tracka_profiler_cpu_end(unsigned){}
static void tracka_profiler_metric(unsigned,double){++metrics;}
''' + function(presentation, "static int present_gl_required(") + flush + r'''
int main() {
    assert(!present_gl_required());
    RbxProfilerAPI api{};profiler=&api;
    api.version=4;assert(!present_gl_required());
    api.version=5;api.shown=[]{return 1;};assert(present_gl_required());
    api.version=8;api.hosted=[]{return 0;};assert(present_gl_required());
    api.hosted=[]{return 1;};assert(!present_gl_required());
    software_cursor_visible=[]{return 1;};assert(present_gl_required());
    software_cursor_visible=nullptr;api.shown=[]{return 0;};assert(!present_gl_required());
    api.enabled=[]{return 1;};api.draw=[]{++draws;};api.frame=[]{++frames;};
    profile_original_flush=[](id,SEL){};gl_swap_serial=[]{return swaps;};
    profiled_flush(nullptr,nullptr);assert(!draws && !frames && !metrics);
    profile_original_flush=[](id,SEL){++swaps;};
    profiled_flush(nullptr,nullptr);assert(!draws && frames==1 && metrics==1);
    api.hosted=[]{return 0;};profiled_flush(nullptr,nullptr);
    assert(draws==1 && frames==2 && metrics==2);
    std::puts("PASS: GTK HUD preserves Vulkan presentation; legacy HUD/cursor retain GL fallback; skipped GL swaps do not inflate FPS");
}
''')
    subprocess.run(["c++", "-std=c++17", "-O2", str(source), "-o", str(path / "check-hud")], check=True)
    subprocess.run([str(path / "check-hud")], check=True)
