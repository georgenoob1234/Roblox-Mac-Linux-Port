// Ensure Cocotron deletes old drawable semaphores in their owning GL context.
#define GL_GLEXT_PROTOTYPES 1
#include "CAMetalDrawableInternal.h"
#include <OpenGL/glext.h>
#include <objc/runtime.h>
#include <dlfcn.h>
#include <cstdio>
#include <mutex>
#include <map>
#include <atomic>
#include <unistd.h>
#include <pthread.h>
#include <memory>
#include <utility>
#include <chrono>
#include <array>
#include <indium/dynamic-vk.hpp>
#include <indium/gpu-timing.hpp>
#include <indium/instance.private.hpp>
#include <vulkan/vulkan_wayland.h>
#include "presentation-semaphore.hpp"
#include "presentation-route.hpp"
#include "swapchain.hpp"

struct PresentationImport {
    std::shared_ptr<PresentationSemaphores::Entry> entry;
    GLuint name;
    GLsync fence = nullptr;
};
@interface TrackAImportedSemaphores : NSObject {
@public
    CGLContextObj context;
    std::vector<PresentationImport> imports;
}
@end
@implementation TrackAImportedSemaphores
- (void)dealloc {
    CGLContextObj previous = CGLGetCurrentContext();
    if (CGLSetCurrentContext(context) != kCGLNoError) std::abort();
    for (auto& imported : imports) {
        glDeleteSemaphoresEXT(1, &imported.name);
        if (imported.fence) glDeleteSync(imported.fence);
    }
    imports.clear();
    CGLSetCurrentContext(previous);
    CGLReleaseContext(context);
    [super dealloc];
}
@end
static TrackAImportedSemaphores* importedSemaphores(id layer) {
    static char key;
    auto owner = (TrackAImportedSemaphores*)objc_getAssociatedObject(layer, &key);
    if (!owner || owner->context != CGLGetCurrentContext()) {
        owner = [TrackAImportedSemaphores new];
        owner->context = CGLRetainContext(CGLGetCurrentContext());
        objc_setAssociatedObject(layer, &key, owner, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [owner release];
    }
    return owner;
}

@interface NSObject (TrackBQueuePresent)
- (void)queuePresent:(NSUInteger)drawableID;
- (BOOL)displaySyncEnabled;
@end

namespace {
std::mutex presentTimesMutex;
std::atomic<bool> presentTimesPending{false};
std::map<const CAMetalDrawableActual*,Indium::CommandClock::time_point> presentTimes;
}
// Swapchain presentation. The drawable is copied straight into a Vulkan
// swapchain on its own Wayland subsurface: no GL import, no compositor pass
// and no EGL swap inside the commit. GL remains the fallback, and is used
// while the profiler HUD (drawn in GL) is on screen.
struct TrackbPresentHooks {
    uint64_t (*begin)(void);
    void (*end)(const void *context, uint64_t begin); // frame trace row, profiler frame
    int (*glRequired)(void);
};
static std::atomic<const TrackbPresentHooks*> presentHooks{nullptr};
extern "C" void trackb_set_present_hooks(const TrackbPresentHooks* hooks) { presentHooks = hooks; }

namespace {
struct ElfCalls { void *(*dlopen)(const char *); int (*dlclose)(void *); void *(*dlsym)(void *, const char *); };
}
extern "C" ElfCalls* _elfcalls;
namespace {
// Exports of the Wayland helper (browser/wayland.cpp).
struct VulkanWindow {
    bool (*surface)(void**, void**);
    void (*size)(int*, int*);
    int (*visible)(void);
    void (*ownership)(int);
    void (*deferHide)(void);
    void (*takeover)(void);
    void (*hide)(void);
    void (*retireGL)(void);
    void (*cancelRetireGL)(void);
};
const VulkanWindow* vulkanWindow() {
    static const VulkanWindow* window = []() -> const VulkanWindow* {
        const char* mode = std::getenv("ROBLOX_MAC_PRESENT");
        const char* wayland = std::getenv("ROBLOX_MAC_WAYLAND");
        const char* path = std::getenv("ROBLOX_MAC_WAYLAND_HELPER");
        if ((mode && !std::strcmp(mode, "gl")) || !wayland || std::strcmp(wayland, "1") || !path || !_elfcalls) return nullptr;
        void* helper = _elfcalls->dlopen(path);
        if (!helper) return nullptr;
        static VulkanWindow value {
            reinterpret_cast<bool (*)(void**, void**)>(_elfcalls->dlsym(helper, "rbx_wayland_vulkan_surface")),
            reinterpret_cast<void (*)(int*, int*)>(_elfcalls->dlsym(helper, "rbx_wayland_vulkan_size")),
            reinterpret_cast<int (*)(void)>(_elfcalls->dlsym(helper, "rbx_wayland_vulkan_visible")),
            reinterpret_cast<void (*)(int)>(dlsym(RTLD_DEFAULT, "rbxWaylandSetVulkanOwnership")),
            reinterpret_cast<void (*)(void)>(dlsym(RTLD_DEFAULT, "rbxWaylandDeferVulkanHide")),
            reinterpret_cast<void (*)(void)>(_elfcalls->dlsym(helper, "rbx_wayland_vulkan_takeover")),
            reinterpret_cast<void (*)(void)>(_elfcalls->dlsym(helper, "rbx_wayland_vulkan_hide")),
            reinterpret_cast<void (*)(void)>(_elfcalls->dlsym(helper, "rbx_wayland_vulkan_retire_gl")),
            reinterpret_cast<void (*)(void)>(_elfcalls->dlsym(helper, "rbx_wayland_vulkan_cancel_retire_gl")),
        };
        return value.surface && value.size && value.visible && value.ownership && value.deferHide && value.takeover && value.hide && value.retireGL && value.cancelRetireGL ? &value : nullptr;
    }();
    return window;
}
// Leaked at exit on purpose: destroying it would race Wayland/driver teardown.
std::mutex presenterMutex;
Indium::SwapchainPresenter* presenter;
bool presenterUnavailable, glSurfaceAttached;
uint64_t newestPresentationSerial;
// Only queued drawables are recorded (at most three per Metal layer). This
// separate lock also permits reset during recreateDrawables' condition lock.
std::mutex queuedPresentSerialsMutex;
std::map<const CAMetalDrawableActual*,uint64_t> queuedPresentSerials;
uint64_t takeQueuedPresentSerial(const CAMetalDrawableActual* drawable) {
    std::scoped_lock lock(queuedPresentSerialsMutex);
    const auto found = queuedPresentSerials.find(drawable);
    if (found == queuedPresentSerials.end()) return 0;
    const auto serial = found->second;
    queuedPresentSerials.erase(found);
    return serial;
}
void traceStalePresentation(const char* route, uint64_t serial) {
    if (!std::getenv("RBX_PRESENT_RESOURCE_TRACE")) return;
    static uint64_t drops;
    ++drops;
    if (drops <= 4 || !(drops & (drops-1)))
        std::fprintf(stderr,"PRESENT stale %s serial=%llu newest=%llu drops=%llu\n",route,
            (unsigned long long)serial,(unsigned long long)newestPresentationSerial,(unsigned long long)drops);
}

template<class T> T quartzFunction(const char* name) {
    auto result = reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
    if (!result) std::abort();
    return result;
}
void drawableDidPresent(CAMetalDrawableActual* drawable) {
    static auto function = quartzFunction<void (*)(CAMetalDrawableActual*)>("_ZN21CAMetalDrawableActual10didPresentEv");
    function(drawable);
}
void drawableDidDrop(CAMetalDrawableActual* drawable) {
    static auto function = quartzFunction<void (*)(CAMetalDrawableActual*)>("_ZN21CAMetalDrawableActual7didDropEv");
    function(drawable);
}
bool glRequired() {
    auto hooks = presentHooks.load();
    return hooks && hooks->glRequired && hooks->glRequired();
}
// The layer releases each GL-presented drawable one frame late; after a
// switch to Vulkan no later GL frame comes, so release the last one now.
std::shared_ptr<CAMetalDrawableActual> takeLastGLDrawable(id layer) {
    static const ptrdiff_t offset = ivar_getOffset(class_getInstanceVariable(objc_getClass("CAMetalLayerInternal"), "_lastPresentedDrawable"));
    auto& last = *reinterpret_cast<std::shared_ptr<CAMetalDrawableActual>*>(reinterpret_cast<char*>(layer) + offset);
    @synchronized(layer) {
        return std::exchange(last, nullptr);
    }
}
// After a GL frame: set up the swapchain (once the game surface exists), or
// resume it once the profiler HUD is hidden again.
void startVulkanPresentation(Indium::PrivateDevice& device) {
    std::scoped_lock lock(presenterMutex);
    glSurfaceAttached = true;
    if (presenterUnavailable || glRequired()) return;
    if (presenter) { device.vulkanPresentation = true; return; }
    auto window = vulkanWindow();
    if (!window) { presenterUnavailable = true; return; }
    void *display = nullptr, *surface = nullptr;
    if (!window->surface(&display, &surface)) return; // no game surface yet
    static Indium::DynamicVK::DynamicFunction<PFN_vkCreateWaylandSurfaceKHR> createSurface("vkCreateWaylandSurfaceKHR");
    VkWaylandSurfaceCreateInfoKHR info {VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
    info.display = static_cast<wl_display*>(display);
    info.surface = static_cast<wl_surface*>(surface);
    VkSurfaceKHR vulkanSurface = VK_NULL_HANDLE;
    if (!createSurface.isAvailable() || createSurface(Indium::globalInstance, &info, nullptr, &vulkanSurface) != VK_SUCCESS) {
        std::fprintf(stderr, "Metal presentation: no Vulkan Wayland surface, staying on GL\n");
        presenterUnavailable = true;
        return;
    }
    try {
        presenter = new Indium::SwapchainPresenter(device.shared_from_this(), vulkanSurface);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Metal presentation: %s, staying on GL\n", error.what());
        presenterUnavailable = true;
        return;
    }
    device.vulkanPresentation = true;
    std::fprintf(stderr, "Metal presentation: Vulkan swapchain\n");
}
bool presentSync(id layer) {
    // Same policy as the GL presenter: launcher override, else Metal's property.
    const char* interval = std::getenv("ROBLOX_MAC_SWAP_INTERVAL");
    if (interval && (!std::strcmp(interval, "0") || !std::strcmp(interval, "1"))) return interval[0] == '1';
    return [layer displaySyncEnabled];
}
// A frame without a GL export can only use the swapchain or be dropped.
void presentVulkan(const std::shared_ptr<Indium::Drawable>& drawable, CAMetalDrawableActual* actual, Indium::PrivateTexture& texture, id layer, Indium::PrivateDevice& device, uint64_t serial) {
    std::unique_lock lock(presenterMutex);
    auto window = vulkanWindow();
    std::shared_ptr<CAMetalDrawableActual> lastGL;
    auto finish = [&](bool drop) {
        lock.unlock();
        // Presented handlers can submit another command. Never invoke them
        // while holding presentation or layer-queue locks.
        if (lastGL) drawableDidPresent(lastGL.get());
        if (drop) drawableDidDrop(actual);
    };
    if (presentationSerialIsStale(serial, newestPresentationSerial)) {
        traceStalePresentation("Vulkan",serial);
        return finish(true);
    }
    // Another present can begin fallback after our caller reads the flag.
    // Such an in-flight frame has no GL copy to display instead.
    if (!device.vulkanPresentation.load(std::memory_order_acquire) || presenterUnavailable || !presenter || !window) {
        device.vulkanPresentation = false;
        return finish(true);
    }
    if (glRequired()) {
        // The HUD/cursor is drawn in GL. Keep Vulkan visible until the first
        // queued GL frame has flushed; unmapping here exposes the background.
        device.vulkanPresentation = false;
        window->cancelRetireGL();
        window->deferHide();
        return finish(true);
    }
    lastGL = takeLastGLDrawable(layer);
    if (!window->visible()) {
        // The browser transition already unmapped both child surfaces. Keep
        // Vulkan ownership so a queued GL composite cannot map SDL underneath.
        return finish(true);
    }
    int width = 0, height = 0;
    window->size(&width, &height);
    VkExtent2D extent {uint32_t(width), uint32_t(height)};
    if (!width || !height) extent = {uint32_t(texture.width()), uint32_t(texture.height())};
    auto hooks = presentHooks.load();
    const uint64_t begin = hooks ? hooks->begin() : 0;
    // Stop old GL swaps and cancel their pending compositor acknowledgment
    // before Vulkan queues its replacement.
    if (glSurfaceAttached) { window->ownership(1); window->takeover(); }
    const auto result = presenter->present(texture, extent, presentSync(layer), [drawable, actual] { drawableDidPresent(actual); });
    using Result = Indium::SwapchainPresenter::Result;
    if (result == Result::Presented) {
        newestPresentationSerial = serial;
        if (glSurfaceAttached) window->retireGL();
        glSurfaceAttached = false;
        if (hooks) hooks->end(presenter, begin);
    }
    if (result == Result::Failed || result == Result::Lost) {
        std::fprintf(stderr, "Metal presentation: swapchain lost, back to GL\n");
        device.vulkanPresentation = false;
        window->cancelRetireGL();
        window->deferHide();
        delete presenter;
        presenter = nullptr;
        presenterUnavailable = true;
    }
    finish(result == Result::Dropped || result == Result::Failed);
}
}

uint64_t trackb_next_present_serial() {
    static std::atomic<uint64_t> serial{0};
    return serial.fetch_add(1, std::memory_order_relaxed) + 1;
}

// Preserve submit-thread bookkeeping and the synchronous callback that retains
// the Objective-C drawable. Only GL export/import moves to prepareRender.
void trackb_present_drawable(std::shared_ptr<Indium::Drawable> drawable, uint64_t serial = 0) {
    if (!drawable) return;
    if (!serial) serial = trackb_next_present_serial();
    auto* actual = static_cast<CAMetalDrawableActual*>(drawable.get());
    if (actual->_queued) {
        if (auto callback = std::exchange(actual->_wantsToPresentCallback, nullptr)) callback();
        if (auto callback = std::exchange(actual->_didPresentCallback, nullptr)) callback();
        return;
    }
    actual->_queued = true;
    if (auto callback = std::exchange(actual->_wantsToPresentCallback, nullptr)) callback();
    @autoreleasepool {
        id layer = objc_loadWeak(&actual->_layer);
        if (!layer) {
            actual->_presentedTime = 0;
            if (auto callback = std::exchange(actual->_didPresentCallback, nullptr)) callback();
            return;
        }
        auto devicePointer = std::static_pointer_cast<Indium::PrivateDevice>(actual->_texture->device());
        auto& device = *devicePointer;
        // A commit made while Vulkan presented has neither a GL semaphore nor
        // a fresh GL image, even if another frame has since begun fallback.
        auto glSemaphore = actual->_texture->synchronizePresentation();
        const auto route = presentationRoute(bool(glSemaphore), device.vulkanPresentation.load(std::memory_order_acquire));
        if (route == PresentationRoute::Vulkan) {
            presentVulkan(drawable, actual, *actual->_texture, layer, device, serial);
            return;
        }
        if (route == PresentationRoute::Drop) {
            drawableDidDrop(actual);
            return;
        }
        {
            std::unique_lock lock(presenterMutex);
            if (presentationSerialIsStale(serial, newestPresentationSerial)) {
                traceStalePresentation("GL enqueue",serial);
                lock.unlock();
                drawableDidDrop(actual);
                return;
            }
            std::scoped_lock serialLock(queuedPresentSerialsMutex);
            queuedPresentSerials[actual] = serial;
        }
        // Publish before queuePresent's condition unlock makes this drawable
        // visible to the main-thread render callback. No FD exists yet.
        actual->_semaphore = std::move(glSemaphore);
        if (Indium::commandTimingEnabled()) {
            std::scoped_lock lock(presentTimesMutex);
            if(Indium::commandTimingEnabled()){presentTimes[actual] = Indium::CommandClock::now();presentTimesPending=true;}
        }
        [layer queuePresent:actual->_drawableID];
        if (!device.vulkanPresentation.load(std::memory_order_acquire)) startVulkanPresentation(device);
    }
}
static void drawable_present_deferred_import(id self, SEL) {
    trackb_present_drawable([(CAMetalDrawableInternal*)self drawable]);
}

namespace {
template<class T> T glFunction(const char* name) {
    static void* library = dlopen("/System/Library/Frameworks/OpenGL.framework/Libraries/libGL.dylib", RTLD_LAZY);
    auto result = reinterpret_cast<T>(dlsym(library, name));
    if (!result) std::abort();
    return result;
}
void semaphoreDiagnostic(int operation, GLsizei count, const GLuint* objects) {
    if (!std::getenv("RBX_PRESENT_RESOURCE_TRACE")) return;
    static std::mutex mutex;
    static std::map<std::pair<CGLContextObj,GLuint>,bool> live;
    static uint64_t generated = 0, imported = 0, deleted = 0, unknownDeletes = 0;
    std::scoped_lock lock(mutex);
    for (GLsizei i = 0; i < count; ++i) {
        auto key = std::make_pair(CGLGetCurrentContext(), objects[i]);
        if (operation == 0) { live[key] = false; ++generated; }
        if (operation == 1) { live[key] = true; ++imported; }
        if (operation == 2) { if (!live.erase(key)) ++unknownDeletes; ++deleted; }
    }
    if ((operation == 0 && generated == 1) || (operation == 1 && (imported == 1 || imported % 256 == 0)) ||
        (operation == 2 && (deleted == 1 || deleted % 256 == 0)))
        std::fprintf(stderr, "PRESENT resources GLgen=%llu GLimport=%llu GLdelete=%llu live=%zu unknown-context-delete=%llu\n",
            (unsigned long long)generated, (unsigned long long)imported, (unsigned long long)deleted, live.size(), (unsigned long long)unknownDeletes);
}
}

extern "C" void glGenSemaphoresEXT(GLsizei count, GLuint* semaphores) {
    static auto original = glFunction<decltype(&glGenSemaphoresEXT)>("glGenSemaphoresEXT");
    original(count, semaphores);
    semaphoreDiagnostic(0, count, semaphores);
}
extern "C" void glImportSemaphoreFdEXT(GLuint semaphore, GLenum type, GLint fd) {
    static auto original = glFunction<decltype(&glImportSemaphoreFdEXT)>("glImportSemaphoreFdEXT");
    // Isolate errors from this import before deciding who owns the exported FD.
    for (unsigned i = 0; glGetError() != GL_NO_ERROR; ++i) if (i == 31) std::abort();
    original(semaphore, type, fd);
    auto error = glGetError();
    if (error != GL_NO_ERROR) {
        // EXT_external_objects_fd transfers ownership only on SUCCESS.
        // Never inspect/close fd after a successful import (GL owns it).
        if (fd >= 0) close(fd);
        std::fprintf(stderr, "PRESENT semaphore import failed GLerror=0x%x\n", error);
        std::abort();
    }
    semaphoreDiagnostic(1, 1, &semaphore);
}
extern "C" void glDeleteSemaphoresEXT(GLsizei count, const GLuint* semaphores) {
    static auto original = glFunction<decltype(&glDeleteSemaphoresEXT)>("glDeleteSemaphoresEXT");
    original(count, semaphores);
    semaphoreDiagnostic(2, count, semaphores);
}
// Access the existing Objective-C ivars through runtime offsets. This avoids
// a QuartzCore link dependency and keeps its non-fragile ivar layout intact.
void trackb_prepare_render(id self, SEL) {
    std::unique_lock presentationLock(presenterMutex);
    static const std::array<ptrdiff_t,7> offsets = [] {
        std::array<ptrdiff_t,7> result{};
        const char* names[] = {"_drawables", "_queuedDrawables", "_queuedDrawableCount", "_drawableCondition", "_lastPresentedDrawable", "_tex", "_drawableSize"};
        Class cls = objc_getClass("CAMetalLayerInternal");
        for (size_t i=0;i<result.size();++i) {
            Ivar ivar = class_getInstanceVariable(cls,names[i]);
            if (!ivar) std::abort();
            result[i] = ivar_getOffset(ivar);
        }
        return result;
    }();
    auto field = [&](size_t i) { return reinterpret_cast<char*>(self)+offsets[i]; };
    auto& drawables = *reinterpret_cast<std::array<std::shared_ptr<CAMetalDrawableActual>,3>*>(field(0));
    auto& queued = *reinterpret_cast<std::array<NSUInteger,3>*>(field(1));
    auto& count = *reinterpret_cast<NSUInteger*>(field(2));
    NSCondition* condition = *reinterpret_cast<NSCondition**>(field(3));
    auto& last = *reinterpret_cast<std::shared_ptr<CAMetalDrawableActual>*>(field(4));
    GLuint target = *reinterpret_cast<GLuint*>(field(5));
    CGSize size = *reinterpret_cast<CGSize*>(field(6));
    std::shared_ptr<CAMetalDrawableActual> drawable;
    std::vector<std::shared_ptr<CAMetalDrawableActual>> dropped;
    uint64_t serial = 0;
    for (;;) {
        [condition lock];
        if (count) {
            if (count>queued.size() || queued[0]>=drawables.size()) std::abort();
            drawable = drawables[queued[0]];
            --count;
            for (size_t i=0;i<count;++i) queued[i]=queued[i+1];
        }
        [condition unlock];
        if (!drawable) break;
        serial = takeQueuedPresentSerial(drawable.get());
        if (!presentationSerialIsStale(serial, newestPresentationSerial)) break;
        traceStalePresentation("GL dequeue",serial);
        dropped.push_back(std::move(drawable));
    }
    if (drawable) {
        auto cache = importedSemaphores(self);
        for (auto& imported : cache->imports) {
            if (!imported.fence) continue;
            auto status = glClientWaitSync(imported.fence, 0, 0);
            if (status == GL_TIMEOUT_EXPIRED) continue;
            if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) std::abort();
            glDeleteSync(imported.fence);
            imported.fence = nullptr;
            PresentationSemaphores::consumed(imported.entry);
        }
        if (presentTimesPending.load()) {
            std::scoped_lock lock(presentTimesMutex);
            if(!Indium::commandTimingEnabled())presentTimes.clear();
            auto it=presentTimes.find(drawable.get());
            if (it!=presentTimes.end()) {
                Indium::reportCommandTiming(Indium::CommandSpan::PresentDispatch,it->second);
                presentTimes.erase(it);
            }
            presentTimesPending=!presentTimes.empty();
        }
        using TextureName = GLuint (*)(const CAMetalDrawableTexture*);
        static auto textureName = reinterpret_cast<TextureName>(dlsym(RTLD_DEFAULT,"_ZNK22CAMetalDrawableTexture9glTextureEv"));
        if (!textureName) std::abort();
        GLuint texture = textureName(drawable->_texture.get());
        PresentationImport* imported = nullptr;
        for (auto& candidate : cache->imports) {
            if (candidate.entry->semaphore == drawable->_semaphore) {
                imported = &candidate;
                drawable->_glSemaphore = candidate.name;
                if (candidate.fence) std::abort();
                break;
            }
        }
        if (drawable->_semaphore && !drawable->_glSemaphore) {
            // A nonzero generated name proves this thread has a usable GL
            // context before we export a descriptor that GL must consume.
            glGenSemaphoresEXT(1,&drawable->_glSemaphore);
            if (!drawable->_glSemaphore) std::abort();
            static Indium::DynamicVK::DynamicFunction<PFN_vkGetSemaphoreFdKHR> getFd("vkGetSemaphoreFdKHR");
            VkSemaphoreGetFdInfoKHR info{};
            info.sType=VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
            info.semaphore=drawable->_semaphore->semaphore;
            info.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
            int fd=-1;
            if (getFd(drawable->_semaphore->device->device(),&info,&fd)!=VK_SUCCESS) std::abort();
            glImportSemaphoreFdEXT(drawable->_glSemaphore,GL_HANDLE_TYPE_OPAQUE_FD_EXT,fd);
            // Successful import transfers FD ownership to GL. Never close it.
            // ponytail: retain at most 32 imports per layer; exceptional backlog
            // uses ordinary deletion rather than growing an unbounded FD cache.
            if (cache->imports.size() < 32) {
                cache->imports.push_back({PresentationSemaphores::remember(drawable->_semaphore), drawable->_glSemaphore});
                imported = &cache->imports.back();
            }
        }
        if (drawable->_glSemaphore) {
            GLenum layout=GL_LAYOUT_GENERAL_EXT;
            glWaitSemaphoreEXT(drawable->_glSemaphore,0,nullptr,1,&texture,&layout);
        }
        glCopyImageSubData(texture,GL_TEXTURE_2D,0,0,0,0,target,GL_TEXTURE_2D,0,0,0,0,size.width,size.height,1);
        static auto frameReady = reinterpret_cast<void (*)(void)>(dlsym(RTLD_DEFAULT,"rbxWaylandGLFrameReady"));
        if (frameReady) frameReady();
        newestPresentationSerial = serial;
        if (drawable->_glSemaphore) {
            if (imported) {
                imported->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                if (!imported->fence) std::abort();
            } else {
                glDeleteSemaphoresEXT(1, &drawable->_glSemaphore);
            }
            drawable->_glSemaphore=0;
        }
        if (std::getenv("RBX_PRESENT_RESOURCE_TRACE")) {
            static uint64_t prepared=0;
            if (++prepared==1 || prepared%256==0)
                std::fprintf(stderr,"PRESENT prepare completed=%llu GLerror=0x%x\n",(unsigned long long)prepared,glGetError());
        }
    }
    // Preserve Cocotron's one-frame-delayed callback/reuse policy.
    std::shared_ptr<CAMetalDrawableActual> previous;
    @synchronized(self) {
        previous = std::move(last);
        last=drawable;
    }
    presentationLock.unlock();
    for (const auto& stale : dropped) drawableDidDrop(stale.get());
    if (previous) drawableDidPresent(previous.get());
}

// Use the drawable's stored context: a nested layer can have no direct _context.
void CAMetalDrawableActual::reset() {
    takeQueuedPresentSerial(this);
    if (presentTimesPending.load()) {
        std::scoped_lock lock(presentTimesMutex);
        if(!Indium::commandTimingEnabled())presentTimes.clear();else presentTimes.erase(this);
        presentTimesPending=!presentTimes.empty();
    }
    const bool trace = std::getenv("RBX_PRESENT_RESOURCE_TRACE") != nullptr;
    if (trace) {
        static std::atomic<uint64_t> resets{0}, withGL{0};
        auto count = ++resets;
        if (_glSemaphore) ++withGL;
        if (count == 1 || count % 256 == 0)
            std::fprintf(stderr, "PRESENT reset calls=%llu withGL=%llu hasVk=%d\n",
                (unsigned long long)count, (unsigned long long)withGL.load(), bool(_semaphore));
    }
    // Imported names belong to prepareRender's context cache. A queued
    // drawable that is dropped before rendering never exports a descriptor.
    if (_glSemaphore) std::abort();
    // An abandoned drawable may have rendered without being presented. Its
    // prior export must not make a later Vulkan-only frame look GL-ready.
    _texture->synchronizePresentation();
    _semaphore=nullptr;_wantsToPresentCallback=nullptr;_didPresentCallback=nullptr;
    _presentedTime=0;_queued=false;
}
static id (*original_drawable_init)(id,SEL,id,std::shared_ptr<CAMetalDrawableActual>);
static id drawable_init_with_context(id self,SEL selector,id layer,std::shared_ptr<CAMetalDrawableActual> drawable) {
    static std::atomic<bool> first{true};
    if (first.exchange(false) && std::getenv("RBX_PRESENT_RESOURCE_TRACE"))
        std::fprintf(stderr, "PRESENT initializer active trace=%d\n", std::getenv("RBX_PRESENT_RESOURCE_TRACE") != nullptr);
    // Call our implementation directly before the original initializer's local
    // C++ call. Its second reset sees an already-cleared semaphore.
    drawable->reset();
    return original_drawable_init(self,selector,layer,drawable);
}
extern "C" int trackb_install_texture_readback();
extern "C" int trackb_install_drawable_memory();
extern "C" int trackb_install_drawable_cleanup() {
    if (!trackb_install_drawable_memory()) return 0;
    if (std::getenv("RBX_PRESENT_RESOURCE_TRACE")) {
    Dl_info entry{}, resolved{}, hook{};
    dladdr(reinterpret_cast<const void*>(&trackb_install_drawable_cleanup), &entry);
    dladdr(dlsym(RTLD_DEFAULT, "trackb_install_drawable_cleanup"), &resolved);
    dladdr(reinterpret_cast<const void*>(&drawable_init_with_context), &hook);
    std::fprintf(stderr, "PRESENT renderer installer-image=%s default-image=%s hook-image=%s\n",
        entry.dli_fname ? entry.dli_fname : "unknown",
        resolved.dli_fname ? resolved.dli_fname : "unknown",
        hook.dli_fname ? hook.dli_fname : "unknown");
    }
    if (original_drawable_init) return 1;
    if (!trackb_install_texture_readback()) return 0;
    Method method=class_getInstanceMethod(objc_getClass("CAMetalDrawableInternal"),sel_registerName("initWithLayer:drawable:"));
    Method present=class_getInstanceMethod(objc_getClass("CAMetalDrawableInternal"),sel_registerName("present"));
    Method prepare=class_getInstanceMethod(objc_getClass("CAMetalLayerInternal"),sel_registerName("prepareRender"));
    if(!method || !present || !prepare)return 0;
    original_drawable_init=(decltype(original_drawable_init))method_setImplementation(method,(IMP)drawable_init_with_context);
    method_setImplementation(present,(IMP)drawable_present_deferred_import);
    method_setImplementation(prepare,(IMP)trackb_prepare_render);
    if (std::getenv("RBX_PRESENT_RESOURCE_TRACE")) std::fprintf(stderr, "PRESENT cleanup installed\n");
    return 1;
}
