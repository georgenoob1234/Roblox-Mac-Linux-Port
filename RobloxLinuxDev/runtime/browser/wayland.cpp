// GTK owns the toplevel and WebKit. SDL owns only a Wayland subsurface, so
// rendering/input stay native without XEmbed or a CPU framebuffer copy.
#define RBX_WAYLAND_LIBRARY 1
#define main browser_init
#include "host.cpp"
#undef main
#include "wayland-api.h"
#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#include <gdk/gdkwayland.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include "protocols/relative-pointer.h"
#include "protocols/pointer-constraints.h"
#include <X11/Xcursor/Xcursor.h>
#include <vector>
#include <algorithm>
#include <future>
#include <thread>
#include <atomic>
#include <cmath>
#include <charconv>
#include <sched.h>
#include <signal.h>
#ifdef RBX_PROFILER
#include "../profiler/api.h"
#include <chrono>
#include <GL/gl.h>
extern "C" const RbxProfilerAPI *rbx_profiler_api();
extern "C" bool rbx_profiler_key(unsigned,bool,bool,bool);
extern "C" bool rbx_profiler_mouse(int,float,float,float,bool);
extern "C" void rbx_profiler_host_in_gtk();
extern "C" unsigned rbx_profiler_refresh_ms();
extern "C" uint64_t rbx_profiler_revision();
#endif

static SDL_Window *game;
static wl_subcompositor *subcompositor;
static wl_compositor *compositor;
static wl_subsurface *subsurface;
static wl_display *display;
static wl_event_queue *pointer_queue;
static wl_egl_window *egl_window;
// Vulkan presents the game to its own subsurface, stacked above SDL's EGL
// surface. GL still owns SDL's surface and stays usable as the fallback.
static wl_surface *vulkan_surface;
static wl_subsurface *vulkan_subsurface;
static std::atomic<int> vulkan_width{0},vulkan_height{0};
static bool browser_visible;
static int content_width=1100,content_height=800;
static bool resize_report_pending=false;
static SDL_Cursor *game_cursor;
static GdkCursor *gtk_cursor;
static float pointer_x,pointer_y;
static bool pointer_position_known;
static std::thread::id ui_thread;
static std::atomic<bool> can_present{true};
static std::vector<unsigned char> cursor_pixels;
static int cursor_width,cursor_height,cursor_pitch,cursor_x,cursor_y;
static bool pointer_closing;
#if defined(RBX_PROFILER) && !defined(RBX_PROFILER_GL_HUD)
static GtkWidget *profiler_area;
static guint profiler_refresh;
static void update_profiler_overlay() {
    if(!profiler_area)return;
    bool shown=game && !current && !pointer_closing && rbx_profiler_api()->shown();
    gtk_widget_set_visible(profiler_area,shown);
    static uint64_t revision=0;
    auto latest=rbx_profiler_revision();
    if(shown && revision!=latest){gtk_gl_area_queue_render(GTK_GL_AREA(profiler_area));revision=latest;}
    if(shown && !profiler_refresh)profiler_refresh=g_timeout_add(rbx_profiler_refresh_ms(),[](gpointer)->gboolean{
        gtk_gl_area_queue_render(GTK_GL_AREA(profiler_area));return G_SOURCE_CONTINUE;
    },nullptr);
    if(!shown && profiler_refresh){g_source_remove(profiler_refresh);profiler_refresh=0;}
}
static void initialize_profiler_overlay() {
    // Keep the game in its native subsurface. GTK composites only the HUD
    // above it, without forcing a Vulkan -> GL game copy or EGL swap.
    auto overlay=gtk_overlay_new();
    g_object_ref(stack);gtk_container_remove(GTK_CONTAINER(window),stack);
    gtk_container_add(GTK_CONTAINER(overlay),stack);g_object_unref(stack);
    gtk_container_add(GTK_CONTAINER(window),overlay);
    profiler_area=gtk_gl_area_new();
    gtk_gl_area_set_required_version(GTK_GL_AREA(profiler_area),3,3);
    gtk_gl_area_set_has_alpha(GTK_GL_AREA(profiler_area),TRUE);
    gtk_gl_area_set_auto_render(GTK_GL_AREA(profiler_area),FALSE);
    gtk_widget_set_no_show_all(profiler_area,TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay),profiler_area);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay),profiler_area,TRUE);
    g_signal_connect(overlay,"get-child-position",G_CALLBACK(+[](GtkOverlay *overlay,GtkWidget *child,GdkRectangle *rect,gpointer)->gboolean{
        if(child!=profiler_area)return FALSE;
        gtk_widget_translate_coordinates(socket_view,GTK_WIDGET(overlay),0,0,&rect->x,&rect->y);
        rect->width=gtk_widget_get_allocated_width(socket_view);rect->height=gtk_widget_get_allocated_height(socket_view);
        return TRUE;
    }),nullptr);
    g_signal_connect(profiler_area,"render",G_CALLBACK(+[](GtkGLArea*,GdkGLContext*,gpointer)->gboolean{
        glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
        rbx_profiler_api()->draw();return TRUE;
    }),nullptr);
    // The parent must expose its below-parent game surfaces through the game
    // rectangle. Leave the titlebar and browser widgets normally painted.
    auto css=gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,"window { background-color: transparent; }",-1,nullptr);
    gtk_style_context_add_provider(gtk_widget_get_style_context(window),GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    g_signal_connect(socket_view,"draw",G_CALLBACK(+[](GtkWidget*,cairo_t *cr,gpointer)->gboolean{
        cairo_save(cr);cairo_set_operator(cr,CAIRO_OPERATOR_CLEAR);cairo_paint(cr);cairo_restore(cr);return FALSE;
    }),nullptr);
    rbx_profiler_host_in_gtk();
    gtk_widget_show(overlay);
    g_signal_connect(stack,"notify::visible-child",G_CALLBACK(+[](GObject*,GParamSpec*,gpointer){update_profiler_overlay();}),nullptr);
}
#else
static void update_profiler_overlay() {}
#endif

// Engine requests own capture (including Shift Lock); button-up alone does not.
struct PointerCapture {
    bool focused=false,requested=false;
    bool focus(bool value) {
        bool changed=focused!=value;focused=value;
        if(!focused)requested=false;
        return changed;
    }
    void request(bool value) { requested=value && focused; }
};
static PointerCapture pointer_capture;
static std::deque<int> focus_changes;
static bool pointer_trace() {
    static bool enabled=g_strcmp0(g_getenv("ROBLOX_MAC_POINTER_TRACE"),"1")==0;
    return enabled;
}
// GTK owns keyboard focus while SDL owns the child pointer surface. SDL's
// relative mode requires both on the same SDL window; use Wayland's protocols
// directly and enqueue raw deltas alongside SDL's buttons instead.
static zwp_relative_pointer_manager_v1 *relative_manager;
static zwp_pointer_constraints_v1 *pointer_constraints;
static zwp_relative_pointer_v1 *relative_pointer;
static zwp_locked_pointer_v1 *locked_pointer;
static bool capture_active;
static bool queued_capture_active,cursor_visible_requested=true;
static Uint32 raw_motion_event,capture_state_event;
static float capture_x,capture_y;
// Commit the last anchor only while releasing the lock. Re-centering warps
// during capture must stay local; committing every one stalls the compositor.
template<class Hint,class Commit> static bool capture_release_hint(double x,double y,int offset_x,int offset_y,Hint hint,Commit commit) {
    if(!locked_pointer || !pointer_capture.focused)return false;
    double sx=x+offset_x,sy=y+offset_y;
    if(!std::isfinite(sx) || !std::isfinite(sy) ||
       sx<INT32_MIN/256.0 || sx>INT32_MAX/256.0 ||
       sy<INT32_MIN/256.0 || sy>INT32_MAX/256.0)return false;
    hint(wl_fixed_from_double(sx),wl_fixed_from_double(sy));
    // The constraint belongs to the GTK parent, not SDL's child surface.
    // Its double-buffered hint must be committed before destroying the lock.
    commit();
    capture_x=pointer_x=x;capture_y=pointer_y=y;
    // Hints are advisory. Rebase on the compositor's next absolute position,
    // whether it honors the hint or chooses a different release position.
    pointer_position_known=false;
    return true;
}
static void commit_capture_hint() {
    if(!locked_pointer || !pointer_capture.focused)return;
    int offset_x=0,offset_y=0;
    if(!gtk_widget_translate_coordinates(socket_view,window,0,0,&offset_x,&offset_y))return;
    auto native=gtk_widget_get_window(window);
    auto surface=native?gdk_wayland_window_get_wl_surface(native):nullptr;
    if(!surface)return;
    capture_release_hint(capture_x,capture_y,offset_x,offset_y,
        [](wl_fixed_t sx,wl_fixed_t sy){zwp_locked_pointer_v1_set_cursor_position_hint(locked_pointer,sx,sy);},
        [surface]{wl_surface_commit(surface);});
}
static bool capture_cursor_visible(){return cursor_visible_requested && !capture_active;}
static void update_capture_cursor(){
    auto target=gtk_widget_get_window(socket_view);
    if(!target)return;
    auto blank=gdk_cursor_new_for_display(gdk_display_get_default(),GDK_BLANK_CURSOR);
    gdk_window_set_cursor(target,capture_cursor_visible()?gtk_cursor:blank);
    g_object_unref(blank);
}
// Acknowledgments share the motion/button FIFO; the producer may already have
// changed state again by the time an older event reaches poll_event.
static void queue_capture_state(bool active) {
    if(capture_active==active)return;
    capture_active=active;
    SDL_Event event{};event.type=capture_state_event;event.user.code=active;
    SDL_PushEvent(&event);
}
static bool discard_pointer_event(const SDL_Event &event) {
    if(event.type==capture_state_event){queued_capture_active=event.user.code!=0;return true;}
    return event.type==SDL_EVENT_MOUSE_MOTION && queued_capture_active;
}
static void raw_motion(void*,zwp_relative_pointer_v1*,uint32_t,uint32_t,
                       wl_fixed_t,wl_fixed_t,wl_fixed_t dx,wl_fixed_t dy) {
    if(!capture_active || !pointer_capture.focused)return;
    if(pointer_trace())g_printerr("POINTER raw dx=%.1f dy=%.1f\n",wl_fixed_to_double(dx),wl_fixed_to_double(dy));
    SDL_Event event{};event.type=raw_motion_event;
    event.motion.windowID=SDL_GetWindowID(game);
    event.motion.x=capture_x;event.motion.y=capture_y;
    event.motion.xrel=wl_fixed_to_double(dx);event.motion.yrel=wl_fixed_to_double(dy);
    SDL_PushEvent(&event);
}
static void apply_capture(const char *reason) {
    if(!game || (locked_pointer!=nullptr)==pointer_capture.requested)return;
    if(pointer_capture.requested) {
        auto device=gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
        auto pointer=device?gdk_wayland_device_get_wl_pointer(device):nullptr;
        if(!pointer || !relative_manager || !pointer_constraints) {
            g_printerr("Wayland pointer capture unavailable\n");return;
        }
        capture_x=pointer_x;capture_y=pointer_y;
        relative_pointer=zwp_relative_pointer_manager_v1_get_relative_pointer(relative_manager,pointer);
        static const zwp_relative_pointer_v1_listener motion_listener={raw_motion};
        zwp_relative_pointer_v1_add_listener(relative_pointer,&motion_listener,nullptr);
        auto surface=gdk_wayland_window_get_wl_surface(gtk_widget_get_window(window));
        locked_pointer=zwp_pointer_constraints_v1_lock_pointer(pointer_constraints,surface,pointer,nullptr,ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        static const zwp_locked_pointer_v1_listener lock_listener={
            [](void*,zwp_locked_pointer_v1*){queue_capture_state(true);update_capture_cursor();if(pointer_trace())g_printerr("POINTER native locked\n");},
            [](void*,zwp_locked_pointer_v1*){queue_capture_state(false);update_capture_cursor();if(pointer_trace())g_printerr("POINTER native unlocked\n");}
        };
        zwp_locked_pointer_v1_add_listener(locked_pointer,&lock_listener,nullptr);
        // Commit the parent surface after installing its pointer constraint.
        wl_surface_commit(surface);
    } else {
        zwp_locked_pointer_v1_destroy(locked_pointer);locked_pointer=nullptr;
        zwp_relative_pointer_v1_destroy(relative_pointer);relative_pointer=nullptr;
        queue_capture_state(false);update_capture_cursor();SDL_CaptureMouse(false);
    }
    wl_display_flush(display);
    if(pointer_trace())g_printerr("POINTER helper %s requested=%d focused=%d pending=%d actual=%d\n",
        reason,pointer_capture.requested,pointer_capture.focused,locked_pointer!=nullptr,capture_active);
}
static void refresh_pointer_focus() {
    bool focused=game && !current && !pointer_closing && gtk_window_is_active(GTK_WINDOW(window)) && gtk_widget_get_visible(window);
    if(pointer_capture.focus(focused)) {
        focus_changes.push_back(focused?RBX_WL_FOCUS:RBX_WL_BLUR);
        if(!focused)SDL_FlushEvent(raw_motion_event);
#ifdef RBX_PROFILER
        if(!focused)rbx_profiler_mouse(4,0,0,0,false);
#endif
        if(pointer_trace())g_printerr("POINTER helper focus=%d browser=%d\n",focused,current!=nullptr);
        apply_capture("focus");
    }
}

static void registry_global(void*,wl_registry *registry,uint32_t name,const char *interface,uint32_t) {
    if(!strcmp(interface,wl_compositor_interface.name))
        compositor=(wl_compositor*)wl_registry_bind(registry,name,&wl_compositor_interface,1);
    else if(!strcmp(interface,wl_subcompositor_interface.name))
        subcompositor=(wl_subcompositor*)wl_registry_bind(registry,name,&wl_subcompositor_interface,1);
    else if(!strcmp(interface,zwp_relative_pointer_manager_v1_interface.name))
        relative_manager=(zwp_relative_pointer_manager_v1*)wl_registry_bind(registry,name,&zwp_relative_pointer_manager_v1_interface,1);
    else if(!strcmp(interface,zwp_pointer_constraints_v1_interface.name))
        pointer_constraints=(zwp_pointer_constraints_v1*)wl_registry_bind(registry,name,&zwp_pointer_constraints_v1_interface,1);
}
static void registry_removed(void*,wl_registry*,uint32_t) {}
static void layout(GtkWidget*,GtkAllocation *allocation,gpointer) {
    content_width=std::max(1,allocation->width);content_height=std::max(1,allocation->height);
    if(!game)return;
    int x=0,y=0;gtk_widget_translate_coordinates(socket_view,window,0,0,&x,&y);
    wl_subsurface_set_position(subsurface,x,y);
    if(vulkan_subsurface)wl_subsurface_set_position(vulkan_subsurface,x,y);
    vulkan_width=content_width;vulkan_height=content_height;
    SDL_SetWindowSize(game,content_width,content_height);
    if(egl_window)wl_egl_window_resize(egl_window,content_width,content_height,0,0);
}
static wl_callback *gl_handoff_callback;
static wl_callback *vulkan_handoff_callback;
static void cancel_vulkan_handoff() {
    if(vulkan_handoff_callback)wl_callback_destroy(vulkan_handoff_callback);
    vulkan_handoff_callback=nullptr;
}
static void cancel_gl_handoff() {
    if(gl_handoff_callback)wl_callback_destroy(gl_handoff_callback);
    gl_handoff_callback=nullptr;
}
static void pump() {
    // Drain currently ready events only; never block the guest render thread.
    for(int i=0;i<64 && g_main_context_pending(nullptr);i++)g_main_context_iteration(nullptr,FALSE);
    if(!game)return;
    wl_display_dispatch_queue_pending(display,pointer_queue);
    refresh_pointer_focus();
    update_profiler_overlay();
    if((current!=nullptr)!=browser_visible) {
        browser_visible=current!=nullptr;
        can_present=!browser_visible;
        if(browser_visible) {
            cancel_gl_handoff();
            cancel_vulkan_handoff();
            // Browser visibility must not leave a NULL buffer cached behind
            // the synchronization used for Vulkan-to-GL handoff.
            wl_subsurface_set_desync(subsurface);
            if(vulkan_subsurface)wl_subsurface_set_desync(vulkan_subsurface);
            auto surface=(wl_surface*)SDL_GetPointerProperty(SDL_GetWindowProperties(game),SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER,nullptr);
            wl_surface_attach(surface,nullptr,0,0);wl_surface_commit(surface);
            if(vulkan_surface){wl_surface_attach(vulkan_surface,nullptr,0,0);wl_surface_commit(vulkan_surface);}
            wl_display_flush(display);
        }
    }
}
static Uint8 gtk_click_counts[256];
static bool translate_gtk_button(const GdkEventButton &b,const GdkEvent *next,SDL_MouseButtonEvent &out) {
    if(b.button==0 || b.button>=256 || (b.type!=GDK_BUTTON_PRESS && b.type!=GDK_BUTTON_RELEASE))return false;
    out.down=b.type==GDK_BUTTON_PRESS;
    out.type=out.down?SDL_EVENT_MOUSE_BUTTON_DOWN:SDL_EVENT_MOUSE_BUTTON_UP;
    out.button=b.button;out.x=b.x;out.y=b.y;
    if(out.down) {
        // GDK has already applied the user's time/distance thresholds. Label
        // this press from its queued notification; never emit another down.
        Uint8 clicks=1;
        if(next && (next->type==GDK_2BUTTON_PRESS || next->type==GDK_3BUTTON_PRESS) &&
           next->button.button==b.button && next->button.time==b.time &&
           next->button.window==b.window && next->button.device==b.device)
            clicks=next->type==GDK_2BUTTON_PRESS?2:3;
        gtk_click_counts[b.button]=clicks;
    }
    out.clicks=gtk_click_counts[b.button]?gtk_click_counts[b.button]:1;
    return true;
}
static void translate_gtk_scroll(const GdkEventScroll &s,SDL_MouseWheelEvent &out) {
    out.type=SDL_EVENT_MOUSE_WHEEL;out.mouse_x=s.x;out.mouse_y=s.y;
    if(s.direction==GDK_SCROLL_SMOOTH){out.x=s.delta_x;out.y=-s.delta_y;}
    else {out.x=(s.direction==GDK_SCROLL_RIGHT)-(s.direction==GDK_SCROLL_LEFT);out.y=(s.direction==GDK_SCROLL_UP)-(s.direction==GDK_SCROLL_DOWN);}
}
static void connect_pointer_input() {
    gtk_widget_add_events(socket_view,GDK_POINTER_MOTION_MASK|GDK_BUTTON_PRESS_MASK|GDK_BUTTON_RELEASE_MASK|GDK_SCROLL_MASK|GDK_SMOOTH_SCROLL_MASK|GDK_ENTER_NOTIFY_MASK|GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(socket_view,"motion-notify-event",G_CALLBACK(+[](GtkWidget*,GdkEventMotion *m,gpointer)->gboolean{
        if(current || !game)return FALSE;
        SDL_Event e{};e.type=SDL_EVENT_MOUSE_MOTION;e.motion.windowID=SDL_GetWindowID(game);
        e.motion.x=m->x;e.motion.y=m->y;
        if(pointer_position_known){e.motion.xrel=m->x-pointer_x;e.motion.yrel=m->y-pointer_y;}
        pointer_x=m->x;pointer_y=m->y;pointer_position_known=true;SDL_PushEvent(&e);return TRUE;
    }),nullptr);
    auto button=+[](GtkWidget *widget,GdkEventButton *b,gpointer)->gboolean{
        if(current || !game)return FALSE;
        auto next=b->type==GDK_BUTTON_PRESS?gdk_display_peek_event(gtk_widget_get_display(widget)):nullptr;
        SDL_Event e{};bool send=translate_gtk_button(*b,next,e.button);
        if(next)gdk_event_free(next);
        if(!send)return TRUE;
        e.button.windowID=SDL_GetWindowID(game);
        // Wayland buttons reuse GTK's last motion coordinates; they cannot
        // validate an absolute baseline invalidated by a capture release hint.
        pointer_x=b->x;pointer_y=b->y;
        SDL_PushEvent(&e);return TRUE;
    };
    g_signal_connect(socket_view,"button-press-event",G_CALLBACK(button),nullptr);
    g_signal_connect(socket_view,"button-release-event",G_CALLBACK(button),nullptr);
    g_signal_connect(socket_view,"scroll-event",G_CALLBACK(+[](GtkWidget*,GdkEventScroll *s,gpointer)->gboolean{
        if(current || !game)return FALSE;
        SDL_Event e{};translate_gtk_scroll(*s,e.wheel);e.wheel.windowID=SDL_GetWindowID(game);
        SDL_PushEvent(&e);return TRUE;
    }),nullptr);
    g_signal_connect(socket_view,"leave-notify-event",G_CALLBACK(+[](GtkWidget*,GdkEventCrossing*,gpointer)->gboolean{pointer_position_known=false;return FALSE;}),nullptr);
}
// Keep the game widget at an exact pixel size; GTK can still enlarge the
// surrounding window. Input coordinates remain local to this same widget.
static bool parse_render_size(const char *text,int &width,int &height) {
    if(!text || !*text)return false;
    const char *end=text+strlen(text);int w=0,h=0;
    auto a=std::from_chars(text,end,w);
    if(a.ec!=std::errc{} || a.ptr==end || *a.ptr!='x')return false;
    auto b=std::from_chars(a.ptr+1,end,h);
    if(b.ec!=std::errc{} || b.ptr!=end || w<64 || h<64 || w>8192 || h>8192)return false;
    width=w;height=h;return true;
}
static void *native_display(){return display;}
static bool initialize_sdl_video() {
    if(SDL_WasInit(SDL_INIT_VIDEO))return true;
    SDL_SetMainReady();SDL_SetHint(SDL_HINT_VIDEO_DRIVER,"wayland");
    // CGAssociate owns relative mode. Hidden cursor warps must not silently
    // re-enable it behind AppKit's state, nor synthesize camera motion.
    SDL_SetHint(SDL_HINT_MOUSE_EMULATE_WARP_WITH_RELATIVE,"0");
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_WARP_MOTION,"0");
    SDL_SetPointerProperty(SDL_GetGlobalProperties(),SDL_PROP_GLOBAL_VIDEO_WAYLAND_WL_DISPLAY_POINTER,display);
    if(!SDL_Init(SDL_INIT_VIDEO)){g_printerr("Wayland SDL init failed: %s\n",SDL_GetError());return false;}
    g_printerr("WAYLAND SDL initialized\n");
    return true;
}
static void *create_window(int width,int height) {
    g_printerr("WAYLAND creating SDL surface\n");
    if(game)return game;
    if(const char *size=g_getenv("ROBLOX_MAC_RENDER_SIZE");size && *size) {
        if(!parse_render_size(size,width,height)) {
            g_printerr("ROBLOX_MAC_RENDER_SIZE must be WIDTHxHEIGHT (64..8192 pixels)\n");return nullptr;
        }
        gtk_widget_set_size_request(socket_view,width,height);
        gtk_widget_set_halign(socket_view,GTK_ALIGN_CENTER);
        gtk_widget_set_valign(socket_view,GTK_ALIGN_CENTER);
        g_printerr("WAYLAND fixed game render size %dx%d\n",width,height);
    }
    gtk_window_resize(GTK_WINDOW(window),std::max(1,width),std::max(1,height));pump();
    if(!initialize_sdl_video())return nullptr;
    raw_motion_event=SDL_RegisterEvents(2);capture_state_event=raw_motion_event+1;
    if(!raw_motion_event){g_printerr("Wayland input event registration failed: %s\n",SDL_GetError());return nullptr;}
    auto properties=SDL_CreateProperties();
    SDL_SetStringProperty(properties,SDL_PROP_WINDOW_CREATE_TITLE_STRING,"Roblox");
    SDL_SetNumberProperty(properties,SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER,content_width);
    SDL_SetNumberProperty(properties,SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER,content_height);
    SDL_SetBooleanProperty(properties,SDL_PROP_WINDOW_CREATE_WAYLAND_SURFACE_ROLE_CUSTOM_BOOLEAN,true);
    game=SDL_CreateWindowWithProperties(properties);SDL_DestroyProperties(properties);
    if(!game){g_printerr("Wayland surface creation failed: %s\n",SDL_GetError());return nullptr;}
    auto surface=(wl_surface*)SDL_GetPointerProperty(SDL_GetWindowProperties(game),SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER,nullptr);
    // The GTK toplevel owns pointer focus before, during and after capture.
    // A focus change from this child would synthesize button-up while held.
    auto empty=wl_compositor_create_region(compositor);
    wl_surface_set_input_region(surface,empty);wl_region_destroy(empty);
    connect_pointer_input();
    auto parent=gdk_wayland_window_get_wl_surface(gtk_widget_get_window(window));
    subsurface=wl_subcompositor_get_subsurface(subcompositor,surface,parent);
    wl_subsurface_set_desync(subsurface);
#if defined(RBX_PROFILER) && !defined(RBX_PROFILER_GL_HUD)
    wl_subsurface_place_below(subsurface,parent);
#endif
    egl_window=wl_egl_window_create(surface,content_width,content_height);
    GtkAllocation allocation;gtk_widget_get_allocation(socket_view,&allocation);layout(socket_view,&allocation,nullptr);
    SDL_StartTextInput(game);wl_display_flush(display);refresh_pointer_focus();
    return game;
}
static void *native_surface(void*){return egl_window;}
static bool create_vulkan_surface() {
    if(vulkan_surface)return true;
    if(!game)return false;
    auto sdl=(wl_surface*)SDL_GetPointerProperty(SDL_GetWindowProperties(game),SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER,nullptr);
    auto parent=gdk_wayland_window_get_wl_surface(gtk_widget_get_window(window));
    if(!sdl || !parent)return false;
    vulkan_surface=wl_compositor_create_surface(compositor);
    // Input stays with the GTK toplevel, as for SDL's surface.
    auto empty=wl_compositor_create_region(compositor);
    wl_surface_set_input_region(vulkan_surface,empty);wl_region_destroy(empty);
    vulkan_subsurface=wl_subcompositor_get_subsurface(subcompositor,vulkan_surface,parent);
    wl_subsurface_set_desync(vulkan_subsurface);
    wl_subsurface_place_above(vulkan_subsurface,sdl);
    GtkAllocation allocation;gtk_widget_get_allocation(socket_view,&allocation);layout(socket_view,&allocation,nullptr);
    // Stacking and position are parent state: have GTK commit the parent.
    gtk_widget_queue_draw(window);
    wl_surface_commit(vulkan_surface);wl_display_flush(display);
    return true;
}
static void action(void*,int op,double x,double y,const char *text) {
    if(!game)return;
    switch(op) {
        case RBX_WL_SHOW:gtk_widget_show(window);break;
        case RBX_WL_HIDE:
            cancel_vulkan_handoff();
            gtk_widget_hide(window);
            if(gl_handoff_callback) {
                // The hidden parent lets us finish fallback without exposing
                // an unfinished GL buffer or stranding Vulkan on the next show.
                cancel_gl_handoff();
                wl_subsurface_set_desync(vulkan_subsurface);
                wl_surface_attach(vulkan_surface,nullptr,0,0);wl_surface_commit(vulkan_surface);
                wl_display_flush(display);
            }
            break;
        case RBX_WL_TITLE:gtk_header_bar_set_title(GTK_HEADER_BAR(header),text?text:"Roblox");break;
        case RBX_WL_RESIZE_WINDOW:
            gtk_window_resize(GTK_WINDOW(window),std::max(1,(int)x),std::max(1,(int)y));
            // Tiled compositors can reject a requested size without a new GTK
            // allocation. Correct the guest's optimistic size on the next poll.
            resize_report_pending=(std::max(1,(int)x)!=content_width || std::max(1,(int)y)!=content_height);break;
        case RBX_WL_FULLSCREEN:
            if(x){gtk_widget_hide(header);gtk_window_fullscreen(GTK_WINDOW(window));}
            else {gtk_window_unfullscreen(GTK_WINDOW(window));gtk_widget_show(header);}break;
        case RBX_WL_LOCK:
            refresh_pointer_focus();pointer_capture.request(x!=0);
            if(!x)commit_capture_hint();
            apply_capture("engine");
            if(pointer_trace())g_printerr("POINTER helper engine=%d accepted=%d pending=%d actual=%d\n",x!=0,pointer_capture.requested,locked_pointer!=nullptr,capture_active);
            break;
        case RBX_WL_WARP: {
            refresh_pointer_focus();
            if(!pointer_capture.focused)break; // Never move another workspace's pointer.
            if(locked_pointer) {
                capture_x=pointer_x=x;capture_y=pointer_y=y;
                pointer_position_known=false;
            } else SDL_WarpMouseInWindow(game,x,y);
            break;
        }
        case RBX_WL_MINIMIZE:gtk_window_iconify(GTK_WINDOW(window));break;
        case RBX_WL_CURSOR_VISIBLE:cursor_visible_requested=x!=0;update_capture_cursor();break;
    }
}
static unsigned modifiers(SDL_Keymod m) {
    return ((m&SDL_KMOD_SHIFT)?1u<<17:0)|((m&SDL_KMOD_CTRL)?1u<<18:0)|
        ((m&SDL_KMOD_ALT)?1u<<19:0)|((m&SDL_KMOD_GUI)?1u<<20:0)|((m&SDL_KMOD_CAPS)?1u<<16:0);
}
static int poll_event(RbxWaylandEvent *out) {
    // Caller drains GTK once before consuming this queue (or a whole batch).
    memset(out,0,sizeof(*out));
    if(!focus_changes.empty()){out->type=focus_changes.front();focus_changes.pop_front();return 1;}
    if(close_requested){close_requested=false;out->type=RBX_WL_CLOSE;return 1;}
    static int width,height;
    if(game && (resize_report_pending || width!=content_width || height!=content_height)) {
        resize_report_pending=false;
        width=content_width;height=content_height;out->type=RBX_WL_RESIZE;out->x=width;out->y=height;return 1;
    }
    SDL_Event e;
    while(game && SDL_PollEvent(&e)) {
        if(discard_pointer_event(e))continue;
#ifdef RBX_PROFILER
        int hud_kind=-1;float hud_x=0,hud_y=0,hud_wheel=0;
        if(e.type==SDL_EVENT_MOUSE_MOTION){hud_kind=0;hud_x=e.motion.x;hud_y=e.motion.y;}
        if((e.type==SDL_EVENT_MOUSE_BUTTON_DOWN || e.type==SDL_EVENT_MOUSE_BUTTON_UP) && e.button.button==SDL_BUTTON_LEFT){
            hud_kind=e.type==SDL_EVENT_MOUSE_BUTTON_DOWN?1:2;hud_x=e.button.x;hud_y=e.button.y;
        }
        if(e.type==SDL_EVENT_MOUSE_WHEEL){hud_kind=3;hud_x=e.wheel.mouse_x;hud_y=e.wheel.mouse_y;hud_wheel=e.wheel.y*(e.wheel.direction==SDL_MOUSEWHEEL_FLIPPED?-1:1);}
        if(hud_kind>=0){int w=0,h=0;SDL_GetWindowSize(game,&w,&h);
            if(w>0 && h>0 && rbx_profiler_mouse(hud_kind,hud_x/w,hud_y/h,hud_wheel,!current && pointer_capture.focused && !capture_active))continue;
        }
#endif
        out->modifiers=modifiers(SDL_GetModState());
        if(e.type==raw_motion_event) {
            out->type=RBX_WL_MOTION;out->x=e.motion.x;out->y=e.motion.y;
            out->dx=e.motion.xrel;out->dy=e.motion.yrel;return 1;
        }
        switch(e.type) {
            case SDL_EVENT_MOUSE_MOTION:out->type=RBX_WL_MOTION;out->x=e.motion.x;out->y=e.motion.y;out->dx=e.motion.xrel;out->dy=e.motion.yrel;break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:case SDL_EVENT_MOUSE_BUTTON_UP:
                if(pointer_trace())g_printerr("POINTER helper button=%u down=%d focused=%d relative=%d\n",e.button.button,e.type==SDL_EVENT_MOUSE_BUTTON_DOWN,pointer_capture.focused,capture_active);
                out->type=e.type==SDL_EVENT_MOUSE_BUTTON_DOWN?RBX_WL_DOWN:RBX_WL_UP;out->button=e.button.button;out->clicks=e.button.clicks;out->x=e.button.x;out->y=e.button.y;break;
            case SDL_EVENT_MOUSE_WHEEL:out->type=RBX_WL_SCROLL;out->dx=e.wheel.x;out->dy=e.wheel.y;out->x=e.wheel.mouse_x;out->y=e.wheel.mouse_y;
                if(e.wheel.direction==SDL_MOUSEWHEEL_FLIPPED){out->dx=-out->dx;out->dy=-out->dy;}
                if(pointer_trace())g_printerr("POINTER helper wheel dx=%.2f dy=%.2f at=%.0f,%.0f relative=%d\n",out->dx,out->dy,out->x,out->y,capture_active);
                break;
            case SDL_EVENT_KEY_DOWN:case SDL_EVENT_KEY_UP:
#ifdef RBX_PROFILER
                if(rbx_profiler_key(e.key.scancode,e.type==SDL_EVENT_KEY_DOWN,e.key.mod&SDL_KMOD_SHIFT,e.key.repeat))continue;
#endif
                if(e.type==SDL_EVENT_KEY_DOWN && e.key.scancode==SDL_SCANCODE_F4 && (e.key.mod&SDL_KMOD_ALT)) {
                    request_close();close_requested=false;out->type=RBX_WL_CLOSE;return 1;
                }
                out->type=e.type==SDL_EVENT_KEY_DOWN?RBX_WL_KEY_DOWN:RBX_WL_KEY_UP;out->key=e.key.scancode;out->repeat=e.key.repeat;out->modifiers=modifiers(e.key.mod);
                if(!(e.key.key & SDLK_SCANCODE_MASK) && e.key.key<128){out->text[0]=(char)e.key.key;out->text[1]=0;}
                if(e.type==SDL_EVENT_KEY_DOWN) {
                    SDL_Event next;
                    if(SDL_PeepEvents(&next,1,SDL_PEEKEVENT,SDL_EVENT_FIRST,SDL_EVENT_LAST)==1 && next.type==SDL_EVENT_TEXT_INPUT) {
                        g_strlcpy(out->text,next.text.text,sizeof(out->text));SDL_PollEvent(&next);
                    } else if(e.key.key>=32 && e.key.key<127 && !(e.key.mod&(SDL_KMOD_CTRL|SDL_KMOD_GUI)))out->text[0]=0;
                }
                break;
            case SDL_EVENT_TEXT_INPUT:out->type=RBX_WL_TEXT;g_strlcpy(out->text,e.text.text,sizeof(out->text));break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:case SDL_EVENT_WINDOW_FOCUS_LOST:
                // SDL is a child surface; GTK owns toplevel activation. Keep
                // one source of application focus in both batched and direct APIs.
                refresh_pointer_focus();
                if(!focus_changes.empty()){out->type=focus_changes.front();focus_changes.pop_front();return 1;}
                continue;
            default:continue;
        }
        return 1;
    }
    return 0;
}
static void screen(int *w,int *h,double *hz) {
    auto monitor=gdk_display_get_primary_monitor(gdk_display_get_default());
    if(!monitor)monitor=gdk_display_get_monitor(gdk_display_get_default(),0);
    GdkRectangle area;gdk_monitor_get_geometry(monitor,&area);*w=area.width;*h=area.height;*hz=gdk_monitor_get_refresh_rate(monitor)/1000.;
}
static void cursor(const void *pixels,int w,int h,int pitch,int x,int y,const char *name) {
    XcursorImage *theme=nullptr;
    if(!pixels) {
        const char *size=g_getenv("XCURSOR_SIZE");
        theme=XcursorLibraryLoadImage(name && !strcmp(name,"IBeam")?"text":"left_ptr",g_getenv("XCURSOR_THEME"),size?std::clamp(atoi(size),16,128):24);
        if(theme){pixels=theme->pixels;w=theme->width;h=theme->height;pitch=w*4;x=theme->xhot;y=theme->yhot;}
    }
    SDL_Cursor *next=nullptr;
    if(pixels && w>0 && h>0 && w<=1024 && h<=1024) {
        auto surface=SDL_CreateSurfaceFrom(w,h,SDL_PIXELFORMAT_BGRA32,const_cast<void*>(pixels),pitch);
        if(surface){next=SDL_CreateColorCursor(surface,x,y);SDL_DestroySurface(surface);}
    } else next=SDL_CreateSystemCursor(name && !strcmp(name,"IBeam")?SDL_SYSTEM_CURSOR_TEXT:SDL_SYSTEM_CURSOR_DEFAULT);
    if(next){SDL_SetCursor(next);if(game_cursor)SDL_DestroyCursor(game_cursor);game_cursor=next;
        if(pixels){cursor_pixels.assign((const unsigned char*)pixels,(const unsigned char*)pixels+pitch*h);cursor_width=w;cursor_height=h;cursor_pitch=pitch;cursor_x=x;cursor_y=y;}}
    if(pixels && w>0 && h>0 && w<=1024 && h<=1024) {
        auto rgba=gdk_pixbuf_new(GDK_COLORSPACE_RGB,TRUE,8,w,h);
        auto dest=gdk_pixbuf_get_pixels(rgba);int stride=gdk_pixbuf_get_rowstride(rgba);
        for(int row=0;row<h;++row)for(int col=0;col<w;++col){
            const auto src=(const unsigned char*)pixels+row*pitch+col*4;auto d=dest+row*stride+col*4;
            for(int ch=0;ch<3;++ch)d[ch]=src[3]?std::min(255,int(src[2-ch])*255/src[3]):0;
            d[3]=src[3];
        }
        auto next=gdk_cursor_new_from_pixbuf(gdk_display_get_default(),rgba,x,y);g_object_unref(rgba);
        if(next){if(gtk_cursor)g_object_unref(gtk_cursor);gtk_cursor=next;update_capture_cursor();}
    }
    if(theme)XcursorImageDestroy(theme);
}
static const void *cursor_image(int *w,int *h,int *pitch,int *x,int *y) {
    if(cursor_pixels.empty())cursor(nullptr,0,0,0,0,0,nullptr);
    *w=cursor_width;*h=cursor_height;*pitch=cursor_pitch;*x=cursor_x;*y=cursor_y;
    return cursor_pixels.empty()?nullptr:cursor_pixels.data();
}
static const char *clipboard(const char *value) {
    static std::string result;
    if(value){if(!SDL_SetClipboardText(value))return nullptr;return value;}
    char *text=SDL_GetClipboardText();if(!text)return nullptr;
    result=text;SDL_free(text);return result.c_str();
}
static const char *user_agent(){return g_getenv("ROBLOX_MAC_WEBKIT_USER_AGENT");}
// Presentation ownership is gated by CGLFlushDrawable. Visibility must stay
// independent so the first GL frame can replace Vulkan when a cursor appears.
static int visible(){return can_present.load();}
static bool initialize_ui() {
    // WebKit's bundled subprocess path is relative to browser/. Give only the
    // host UI thread its own cwd; Roblox keeps its existing Darwin cwd.
    if(const char *root=g_getenv("ROBLOX_MAC_BROWSER_ROOT")) {
        if(unshare(CLONE_FS) || chdir(root)){g_printerr("Wayland browser directory setup failed\n");return false;}
    }
    g_printerr("WAYLAND helper init\n");
    char name[]="roblox-mac",arg[]="native";char *argv[]={name,arg,nullptr};
    if(browser_init(2,argv))return false;
#if defined(RBX_PROFILER) && !defined(RBX_PROFILER_GL_HUD)
    initialize_profiler_overlay();
#endif
    close_input=[]{cancel_gl_handoff();pointer_closing=true;refresh_pointer_focus();apply_capture("close");};
    g_printerr("WAYLAND browser ready\n");
    display=gdk_wayland_display_get_wl_display(gdk_display_get_default());
    pointer_queue=wl_display_create_queue(display);
    auto registry=wl_display_get_registry(display);
    wl_proxy_set_queue((wl_proxy*)registry,pointer_queue);
    static const wl_registry_listener listener={registry_global,registry_removed};
    wl_registry_add_listener(registry,&listener,nullptr);wl_display_roundtrip_queue(display,pointer_queue);wl_registry_destroy(registry);g_printerr("WAYLAND registry ready\n");
    if(!subcompositor || !compositor)return false;
    g_signal_connect(socket_view,"size-allocate",G_CALLBACK(layout),nullptr);
    // Release on the host UI thread even if the guest is stalled or has a
    // prefetched input batch. Browser transitions need no SDL keyboard event.
    auto focus=+[](GObject*,GParamSpec*,gpointer){refresh_pointer_focus();};
    g_signal_connect(window,"notify::is-active",G_CALLBACK(focus),nullptr);
    g_signal_connect(window,"notify::visible",G_CALLBACK(focus),nullptr);
    g_signal_connect(stack,"notify::visible-child",G_CALLBACK(focus),nullptr);pump();
    g_printerr("WAYLAND helper ready\n");return true;
}
// GTK/JSC must run on a Linux pthread stack. Darling switches its guest main
// thread to another stack, which makes JSC's native stack scrubbing cross an
// unmapped gap. Marshal UI calls to a real host thread; EGL stays on the guest
// rendering thread and no pixels cross this boundary.
#ifdef RBX_PROFILER
struct ProfileUICpu {
 const RbxProfilerAPI *api=rbx_profiler_api();unsigned parent=api->version>=2?api->cpu_begin(RBX_PROF_UI_ROUNDTRIP):UINT32_MAX;
 ~ProfileUICpu(){if(api->version>=2)api->cpu_end(parent);}
};
struct ProfileUIRoundTrip {
 ProfileUICpu cpu;
 const RbxProfilerAPI *api=rbx_profiler_api();
 bool active=api->enabled();
 std::chrono::steady_clock::time_point start=active?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
 ~ProfileUIRoundTrip(){if(active)api->metric(RBX_PROF_UI_ROUNDTRIP,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());}
};
#endif
template<class F> static auto on_ui(F function)->decltype(function()) {
#ifdef RBX_PROFILER
    ProfileUIRoundTrip timing;
#endif
    if(std::this_thread::get_id()==ui_thread)return function();
    std::packaged_task<decltype(function())()> task([function=std::move(function)]() mutable {
#ifdef RBX_PROFILER
        ProfileUICpu cpu;
#endif
        return function();
    });auto result=task.get_future();
    g_main_context_invoke(nullptr,[](gpointer p)->gboolean{(*static_cast<decltype(task)*>(p))();return G_SOURCE_REMOVE;},&task);
    return result.get();
}
struct AsyncAction { void *window; int op; double x,y; };
static gboolean run_async_action(gpointer data) {
    auto request=static_cast<AsyncAction*>(data);
    action(request->window,request->op,request->x,request->y,nullptr);
    delete request;
    return G_SOURCE_REMOVE;
}
static void action_async(void *window,int op,double x,double y) {
    if(std::this_thread::get_id()==ui_thread){action(window,op,x,y,nullptr);return;}
    // Preserve warp/unlock order. Captured warps already stay local in AppKit.
    g_main_context_invoke(nullptr,run_async_action,new AsyncAction{window,op,x,y});
}
// Plain C entry points for the Vulkan presenter in the guest renderer.
extern "C" bool rbx_wayland_vulkan_surface(void **display_out,void **surface_out) {
    if(!on_ui([]{return create_vulkan_surface();}))return false;
    *display_out=display;*surface_out=vulkan_surface;return true;
}
extern "C" void rbx_wayland_vulkan_size(int *w,int *h){*w=vulkan_width.load();*h=vulkan_height.load();}
extern "C" int rbx_wayland_vulkan_visible(){return can_present.load();}
static void complete_gl_handoff(void *,wl_callback *callback,uint32_t) {
    if(callback!=gl_handoff_callback)return;
    cancel_gl_handoff();
    if(!vulkan_surface || !can_present || current || pointer_closing)return;
    // The callback belongs to a commit after the fresh EGL buffer. Even when
    // occluded, Hyprland sends it only after that buffer's acquire fence clears.
    wl_subsurface_set_desync(vulkan_subsurface);
    wl_surface_attach(vulkan_surface,nullptr,0,0);wl_surface_commit(vulkan_surface);
    wl_display_flush(display);
}
extern "C" void rbx_wayland_vulkan_takeover() {
    // Cancel the previous fallback before a newer Vulkan present can be queued.
    on_ui([]{cancel_gl_handoff();cancel_vulkan_handoff();if(subsurface){wl_subsurface_set_sync(subsurface);wl_display_flush(display);}});
}
// Retire the old EGL buffer only once the compositor has accepted the Vulkan
// replacement. Window opacity is applied to each surface, so even an opaque
// swapchain can expose an old, smaller EGL frame underneath it.
static void complete_vulkan_handoff(void *,wl_callback *callback,uint32_t) {
    if(callback!=vulkan_handoff_callback)return;
    cancel_vulkan_handoff();
    if(!game || !can_present || current || pointer_closing)return;
    auto surface=(wl_surface*)SDL_GetPointerProperty(SDL_GetWindowProperties(game),SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER,nullptr);
    wl_surface_attach(surface,nullptr,0,0);wl_surface_commit(surface);
    wl_subsurface_set_desync(subsurface);
    wl_display_flush(display);
}
extern "C" void rbx_wayland_vulkan_retire_gl() {
    on_ui([]{
        if(!vulkan_surface || !game || !can_present || current || pointer_closing || vulkan_handoff_callback)return;
        vulkan_handoff_callback=wl_surface_frame(vulkan_surface);
        if(!vulkan_handoff_callback)return;
        wl_proxy_set_queue((wl_proxy*)vulkan_handoff_callback,pointer_queue);
        static const wl_callback_listener listener={complete_vulkan_handoff};
        wl_callback_add_listener(vulkan_handoff_callback,&listener,nullptr);
        wl_surface_commit(vulkan_surface);wl_display_flush(display);
    });
}
extern "C" void rbx_wayland_vulkan_cancel_retire_gl() {
    on_ui([]{cancel_vulkan_handoff();});
}
// CGL calls this after a successful fresh EGL swap, serialized with other swaps.
// Keep Vulkan visible until the compositor acknowledges the newer SDL state;
// synchronized siblings alone do not propagate acquire fences on Hyprland.
extern "C" void rbx_wayland_vulkan_hide() {
    on_ui([]{
        cancel_vulkan_handoff();
        if(!vulkan_surface || !game || current || !can_present || pointer_closing || gl_handoff_callback)return;
        auto surface=(wl_surface*)SDL_GetPointerProperty(SDL_GetWindowProperties(game),SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER,nullptr);
        gl_handoff_callback=wl_surface_frame(surface);
        if(!gl_handoff_callback)return;
        wl_proxy_set_queue((wl_proxy*)gl_handoff_callback,pointer_queue);
        static const wl_callback_listener listener={complete_gl_handoff};
        wl_callback_add_listener(gl_handoff_callback,&listener,nullptr);
        wl_subsurface_set_desync(subsurface);
        wl_surface_commit(surface);
        wl_surface_commit(gdk_wayland_window_get_wl_surface(gtk_widget_get_window(window)));
        wl_display_flush(display);
    });
}
static bool ignore_terminal_resize() {
    // Darling installs a process-wide SIGWINCH handler which requires a Darwin
    // thread RPC socket. Native GTK/driver threads have none. Terminal resizing
    // is irrelevant to this GUI; Wayland configure events resize its windows.
    return signal(SIGWINCH,SIG_IGN)!=SIG_ERR;
}
extern "C" const RbxWaylandAPI *rbx_wayland_api() {
    static bool ready=[] {
        // Set the disposition before starting any native helper threads.
        if(!ignore_terminal_resize()){g_printerr("Wayland signal setup failed: %s\n",strerror(errno));return false;}
        std::promise<bool> startup;auto result=startup.get_future();
        std::thread([&startup] {
            ui_thread=std::this_thread::get_id();
            auto main=g_main_context_default();g_main_context_acquire(main);
            bool ok=initialize_ui();startup.set_value(ok);
            if(ok)gtk_main();g_main_context_release(main);
        }).detach();return result.get();
    }();
    static RbxWaylandAPI api={native_display,
        [](int w,int h)->void*{return on_ui([=]{return create_window(w,h);});},native_surface,
        [](void *w,int op,double x,double y,const char *text){
            if(op==RBX_WL_LOCK || op==RBX_WL_WARP || op==RBX_WL_CURSOR_VISIBLE)action_async(w,op,x,y);
            else on_ui([=]{action(w,op,x,y,text);});
        },
        [](RbxWaylandEvent *event)->int{return on_ui([=]{pump();return poll_event(event);});},
        [](int *w,int *h,double *hz){on_ui([=]{screen(w,h,hz);});},
        [](const void *p,int w,int h,int pitch,int x,int y,const char *name){on_ui([=]{cursor(p,w,h,pitch,x,y,name);});},
        visible,[](const char *text)->const char*{return on_ui([=]{return clipboard(text);});},user_agent,
        [](int *w,int *h,int *pitch,int *x,int *y)->const void*{return on_ui([=]{return cursor_image(w,h,pitch,x,y);});}};
    return ready?&api:nullptr;
}
