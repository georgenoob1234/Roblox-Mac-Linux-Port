// Cocotron chooses its X11 visual through GLX, independently of CGL's EGLConfig.
// Use the visual advertised by CGL's EGL configuration for Cocotron windows.
#include <pthread.h>
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern char *getenv(const char *);
static int wayland(void){const char *v=getenv("ROBLOX_MAC_WAYLAND");return v && v[0]=='1' && !v[1];}
extern int fprintf(void *,const char *,...);
extern void *__stderrp;
extern int rbxWaylandCanPresent(void);
extern void rbxWaylandHideVulkan(void);
@interface NSObject + (id)class; @end
@interface NSDisplay : NSObject + (id)currentDisplay; - (void *)display; @end
static unsigned long cgl_visual;
struct VisualInfo {
    void *visual; unsigned long visualid; int screen, depth, visual_class;
    unsigned long red_mask, green_mask, blue_mask; int colormap_size, bits_per_rgb;
};
static void *symbol(const char *library, const char *name) {
    return dlsym(dlopen(library, 1), name);
}
// Resolve the frame-path entry points once. Reopening these already-loaded
// images on every frame still makes dyld resolve paths and query the filesystem.
static pthread_once_t frame_symbols_once = PTHREAD_ONCE_INIT;
static int (*flush_drawable)(void *);
static int (*attach_context)(void *, void *);
static void *(*current_context)(void);
static void *(*current_display)(void);
static void *(*current_surface)(int);
static unsigned (*swap_buffers)(void *, void *);
static unsigned (*query_surface)(void *, void *, int, int *);
static void (*hide_vulkan)(void);
static int hide_vulkan_pending;
static int gl_frame_ready;
// A Vulkan takeover waits for an in-flight EGL swap, then blocks later GL
// swaps until fallback has a fresh drawable to replace Vulkan atomically.
static pthread_rwlock_t presentation_lock = PTHREAD_RWLOCK_INITIALIZER;
static int vulkan_owns_presentation;
static __thread unsigned long long gl_swap_serial;
unsigned long long rbxWaylandGLSwapSerial(void) { return gl_swap_serial; }
void rbxWaylandSetVulkanOwnership(int owns) {
    pthread_rwlock_wrlock(&presentation_lock);
    vulkan_owns_presentation=owns;
    if(owns){__atomic_store_n(&hide_vulkan_pending,0,__ATOMIC_RELEASE);gl_frame_ready=0;}
    pthread_rwlock_unlock(&presentation_lock);
}
// Let the first queued GL frame reach SDL before the native helper waits for
// its compositor acknowledgment and unmaps Vulkan.
void rbxWaylandDeferVulkanHide(void) {
    pthread_rwlock_wrlock(&presentation_lock);
    vulkan_owns_presentation=0;
    __atomic_store_n(&hide_vulkan_pending,1,__ATOMIC_RELEASE);
    gl_frame_ready=0;
    pthread_rwlock_unlock(&presentation_lock);
}
// Cocoa can redraw for a cursor change before a new Metal drawable arrives.
// Only a copied drawable may expose GL's texture, which is stale during Vulkan.
void rbxWaylandGLFrameReady(void) {
    if(!__atomic_load_n(&hide_vulkan_pending,__ATOMIC_ACQUIRE))return;
    pthread_rwlock_wrlock(&presentation_lock);
    gl_frame_ready=1;
    pthread_rwlock_unlock(&presentation_lock);
}
static void load_frame_symbols(void) {
    void *cgl = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", 1);
    void *egl = dlopen("/usr/lib/native/libEGL.dylib", 1);
    if (cgl) {
        flush_drawable = dlsym(cgl, "CGLFlushDrawable");
        attach_context = dlsym(cgl, "CGLContextMakeCurrentAndAttachToWindow");
        current_context = dlsym(cgl, "CGLGetCurrentContext");
    }
    if (egl) {
        current_display = dlsym(egl, "eglGetCurrentDisplay");
        current_surface = dlsym(egl, "eglGetCurrentSurface");
        swap_buffers = dlsym(egl, "eglSwapBuffers");
        query_surface = dlsym(egl, "eglQuerySurface");
    }
    hide_vulkan = rbxWaylandHideVulkan;
    // Keep the two handles open for the lifetime of these function pointers.
}
extern void glGetIntegerv(unsigned,int *);
extern void glGetBooleanv(unsigned,unsigned char *);
extern void glDrawBuffer(unsigned);
extern void glReadBuffer(unsigned);
extern unsigned glGetError(void);
extern const unsigned char *glGetString(unsigned);
extern void glReadPixels(int,int,int,int,unsigned,unsigned,void*);
int CGLFlushDrawable(void *context) {
    pthread_rwlock_rdlock(&presentation_lock);
    if(vulkan_owns_presentation || (__atomic_load_n(&hide_vulkan_pending,__ATOMIC_ACQUIRE) && !gl_frame_ready) || !rbxWaylandCanPresent()){pthread_rwlock_unlock(&presentation_lock);return 0;}
    pthread_once(&frame_symbols_once, load_frame_symbols);
    int result=10004;
    if (__atomic_load_n(&hide_vulkan_pending,__ATOMIC_ACQUIRE)) {
        // Darling's CGL flush discards EGLBoolean. During handoff, validate the
        // actual current draw surface's swap before retiring the visible frame.
        if (current_context && current_context()==context && current_display && current_surface && swap_buffers) {
            void *display=current_display(), *surface=current_surface(0x3059 /* EGL_DRAW */);
            if (display && surface && swap_buffers(display,surface)) result=0;
        }
    } else result=flush_drawable?flush_drawable(context):10004;
    // Failure keeps Vulkan visible; ownership serializes callback registration
    // with a newer Vulkan takeover, which cancels any pending native hide.
    if (!result && __atomic_exchange_n(&hide_vulkan_pending,0,__ATOMIC_ACQ_REL) && hide_vulkan) hide_vulkan();
    if (!result) ++gl_swap_serial;
    pthread_rwlock_unlock(&presentation_lock);
    return result;
}
int CGLContextMakeCurrentAndAttachToWindow(void *context,void *window) {
    pthread_once(&frame_symbols_once, load_frame_symbols);
    if(!attach_context)return 10004;
    int result=attach_context(context,window);
    if(result || !window)return result;
    // A context first made current without a surface starts with GL_NONE read/
    // draw buffers. EGL keeps that state when attaching the real window.
    int framebuffer=0,buffer=0;unsigned char double_buffered=0;
    glGetBooleanv(0x0c32 /* GL_DOUBLEBUFFER */,&double_buffered);
    unsigned target=double_buffered ? 0x0405 /* GL_BACK */ : 0x0404 /* GL_FRONT */;
    // NVIDIA can keep GL_DOUBLEBUFFER=false after a surfaceless context gets
    // a Wayland window. EGL knows the actual surface mode; GL_FRONT remains
    // unusable in this case even though the GL query suggests otherwise.
    int render_buffer=0;
    if(current_display && query_surface && query_surface(current_display(),window,0x3086 /* EGL_RENDER_BUFFER */,&render_buffer))
        target=render_buffer==0x3084 /* EGL_BACK_BUFFER */?0x0405:0x0404;
    glGetIntegerv(0x8ca6 /* GL_DRAW_FRAMEBUFFER_BINDING */,&framebuffer);
    glGetIntegerv(0x0c01 /* GL_DRAW_BUFFER */,&buffer);
    if(!framebuffer && !buffer)glDrawBuffer(target);
    glGetIntegerv(0x8caa /* GL_READ_FRAMEBUFFER_BINDING */,&framebuffer);
    glGetIntegerv(0x0c02 /* GL_READ_BUFFER */,&buffer);
    if(!framebuffer && !buffer)glReadBuffer(target);
    return result;
}
void *CGLGetWindow(void *window) {
    void *(*get)(void *)=symbol("/System/Library/Frameworks/OpenGL.framework/OpenGL","CGLGetWindow");
    if(wayland()) {
        void *surface=get?get(window):0;
        return surface;
    }
    // The window was created on a thread's X connection; EGL uses the persistent one.
    // Publish queued XCreateWindow requests before EGL queries that window's visual.
    void *native_display = [[NSDisplay currentDisplay] display];
    int (*sync)(void *, int) = symbol("/usr/lib/native/libX11.dylib", "XSync");
    void *(*original)(void *) = symbol("/System/Library/Frameworks/OpenGL.framework/OpenGL", "CGLGetWindow");
    if (!window || !native_display || !sync || !original) return 0;
    sync(native_display, 0);
    return original(window);
}
int CGLCreateContext(void *format, void *share, void **result) {
    unsigned (*bind)(unsigned) = symbol("/usr/lib/native/libEGL.dylib", "eglBindAPI");
    int (*original)(void *,void *,void **) = symbol("/System/Library/Frameworks/OpenGL.framework/OpenGL", "CGLCreateContext");
    if (!bind || !original || !bind(0x30a2)) return 10004;
    return original(format, share, result);
}
int CGLRegisterNativeDisplay(void *native_display) {
    if(wayland()) {
        int (*original)(void *)=symbol("/System/Library/Frameworks/OpenGL.framework/OpenGL","CGLRegisterNativeDisplay");
        return original?original(native_display):10017;
    }
    (void)native_display;
    unsigned (*bind)(unsigned) = symbol("/usr/lib/native/libEGL.dylib", "eglBindAPI");
    if (!bind || !bind(0x30a2 /* EGL_OPENGL_API */)) return 10017;
    @synchronized([NSObject class]) {
    if (cgl_visual) return 0;
    // CGL stores one process-wide display. Give it its own connection, which survives
    // NSDisplay worker-thread teardown, instead of replacing it on every thread.
    void *(*open_display)(const char *) = symbol("/usr/lib/native/libX11.dylib", "XOpenDisplay");
    static void *connection;
    if (!connection && open_display) connection = open_display(0);
    if (!connection) return 10017;
    int (*original)(void *) = symbol("/System/Library/Frameworks/OpenGL.framework/OpenGL", "CGLRegisterNativeDisplay");
    void *(*display_for)(void *) = symbol("/usr/lib/native/libEGL.dylib", "eglGetDisplay");
    unsigned (*choose)(void *, const int *, void **, int, int *) = symbol("/usr/lib/native/libEGL.dylib", "eglChooseConfig");
    unsigned (*get)(void *, void *, int, int *) = symbol("/usr/lib/native/libEGL.dylib", "eglGetConfigAttrib");
    if (!original || !display_for || !choose || !get) return 10017;
    int result = original(connection);
    if (result) return result;
    void *display = display_for(connection), *config = 0;
    int attributes[] = {0x3024,1,0x3023,1,0x3022,1,0x3038};
    int count = 0, visual = 0;
    if (!choose(display,attributes,&config,1,&count) || !count || !get(display,config,0x302e,&visual)) return 10017;
    cgl_visual = (unsigned long)visual;
    return 0;
    }
}
struct VisualInfo *glXChooseVisual(void *display, int screen, int *attributes) {
    // Only replace Cocotron's fixed window request; preserve other callers' requirements.
    static const int window_attributes[] = {4,5,8,4,9,4,10,4,12,4,0};
    int matches = attributes != 0;
    for (int i=0; matches && i<11; ++i) matches = attributes[i] == window_attributes[i];
    if (matches && cgl_visual) {
        struct VisualInfo *(*get)(void *, long, struct VisualInfo *, int *) = symbol("/usr/lib/native/libX11.dylib", "XGetVisualInfo");
        struct VisualInfo wanted = {.visualid=cgl_visual, .screen=screen};
        int count=0;
        if (get) return get(display,3 /* VisualIDMask | VisualScreenMask */,&wanted,&count);
        return 0;
    }
    struct VisualInfo *(*original)(void *,int,int *) = symbol("/System/Library/Frameworks/OpenGL.framework/Libraries/libGL.dylib","glXChooseVisual");
    return original ? original(display,screen,attributes) : 0;
}
