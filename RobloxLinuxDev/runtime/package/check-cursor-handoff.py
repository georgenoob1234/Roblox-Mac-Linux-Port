#!/usr/bin/env python3
"""Execute the production cursor surface code with a recording Wayland peer.

No desktop or account is opened. Real memfd/mmap storage is used; the peer
records the buffer, hotspot, empty input region and synchronized parent commit.
Live compositor appearance still requires gameplay testing.
"""
from pathlib import Path
import subprocess
import tempfile
import resource
import shlex

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "runtime/browser/wayland.cpp").read_text()
surface = source[source.index("// This surface belongs"):source.index("// Commit the last anchor")]
handoff = source[source.index("static bool capture_cursor_wanted"):source.index("// Acknowledgments share")]
appkit = (ROOT / "runtime/shims/appkit/wayland.m").read_text()
capture = appkit[appkit.index("static void applyCapture("):appkit.index("static void captureFocus(")]
assert "anchor=(buttons & (1u<<3))?rightPressPoint:pointer" in capture
assert capture.index("RBX_WL_CURSOR_ANCHOR") < capture.index("RBX_WL_LOCK")
assert capture.index("RBX_WL_WARP") < capture.index("RBX_WL_LOCK")
assert "rbxLockedCursor(" not in appkit and "rbxMoveLockedCursor(" not in appkit
assert "rbxRefreshLockedCursor(" not in appkit
# The existing X11 path still uses its original GL overlay.
assert "rbxLockedCursor(" in (ROOT / "runtime/shims/appkit/appkit-gaps.m").read_text()
assert "op==RBX_WL_CURSOR_ANCHOR)action_async" in source
button = source[source.index("auto button=+[]"):source.index('g_signal_connect(socket_view,"button-press-event"')]
assert button.index("right_button_cursor=true") < button.index("SDL_PushEvent(&e)")
assert "pointer_capture.request(" not in button and "RBX_WL_WARP" not in button
warp = appkit[appkit.index("static void warpPointer("):appkit.index("static void applyCapture(")]
image_cursor = source[source.index("static GdkCursor *native_image_cursor("):source.index("static void cursor(")]

STUBS = r'''
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <sys/mman.h>
#include <algorithm>
#include <cstdio>
static bool pointer_trace(){return false;}
#define g_printerr(...) ((void)0)
struct wl_buffer {std::vector<unsigned char> pixels;};
struct wl_surface {wl_buffer *pending=nullptr,*shown=nullptr;bool empty=false;};
struct wl_subsurface {int x=0,y=0,shown_x=0,shown_y=0;};
struct wl_shm {} shm;
struct wp_viewporter {} viewport_manager;
struct wp_viewport {double x=0,y=0,shown_x=0,shown_y=0;int width=0,height=0;};
static wp_viewport *viewport;
static wp_viewporter *cursor_viewporter=&viewport_manager;
using wl_fixed_t=int32_t;
static wl_fixed_t wl_fixed_from_double(double x){return std::lround(x*256);}
static wl_fixed_t wl_fixed_from_int(int x){return x*256;}
struct wl_shm_pool {std::vector<unsigned char> pixels;};
struct wl_buffer_listener {void (*release)(void*,wl_buffer*);};
struct GdkCursor {bool blank;int width,height,hot_x,hot_y;} native_cursor{false,4,4,2,3};
struct GdkPixbuf {int width,height;bool transparent=false;};
struct Capture {bool requested=false,focused=false;} pointer_capture;
static wl_shm *cursor_shm=&shm;
static GdkCursor *gtk_cursor=&native_cursor;
static std::vector<unsigned char> cursor_pixels;
static int cursor_width=4,cursor_height=4,cursor_pitch=20,cursor_x=2,cursor_y=3;
static bool cursor_visible_requested=true,native_visible=true,right_button_cursor=false,cursor_inside=false;
static unsigned cursor_update_source;
static unsigned idle_calls,native_calls;
static int (*idle_callback)(void*);
static unsigned g_idle_add(int (*callback)(void*),void*){++idle_calls;idle_callback=callback;return 1;}
static int compositor,subcompositor,display,window,socket_view;
static wl_surface parent;
static wl_surface *child;
static wl_subsurface *sub;
static bool child_committed=false;
static unsigned allocations=0,flushes=0;
static unsigned transparent_allocations=0;
static std::vector<wl_buffer*> buffers;
constexpr unsigned WL_SHM_FORMAT_ARGB8888=0,GDK_COLORSPACE_RGB=0;
constexpr bool TRUE=true;
static wl_shm_pool *wl_shm_create_pool(wl_shm*,int fd,int size) {
    auto p=new wl_shm_pool;p->pixels.resize(size);
    assert(pread(fd,p->pixels.data(),size,0)==size);return p;
}
static wl_buffer *wl_shm_pool_create_buffer(wl_shm_pool *pool,int,int w,int h,int stride,unsigned) {
    int size=cursor_viewporter?6:4;
    assert(w==size && h==size && stride==size*4);
    auto b=new wl_buffer{pool->pixels};buffers.push_back(b);++allocations;return b;
}
static void wl_shm_pool_destroy(wl_shm_pool *p){delete p;}
static void wl_buffer_destroy(wl_buffer *b){delete b;}
static void wl_buffer_add_listener(wl_buffer*,const wl_buffer_listener*,void*){}
static wl_surface *wl_compositor_create_surface(int){child=new wl_surface;return child;}
static int wl_compositor_create_region(int){return 1;}
static void wl_region_destroy(int){}
static void wl_surface_set_input_region(wl_surface *s,int){s->empty=true;}
static wl_subsurface *wl_subcompositor_get_subsurface(int,wl_surface *s,wl_surface *p) {
    assert(s==child && p==&parent && s->empty);sub=new wl_subsurface;return sub;
}
static wp_viewport *wp_viewporter_get_viewport(wp_viewporter*,wl_surface*){viewport=new wp_viewport;return viewport;}
static void wp_viewport_set_source(wp_viewport *v,wl_fixed_t x,wl_fixed_t y,wl_fixed_t w,wl_fixed_t h){
    v->x=x/256.0;v->y=y/256.0;
    assert(v->x>=0 && v->x<=1 && v->y>=0 && v->y<=1);
    assert(w==5*256 && h==5*256); // crop is 1:1, with no scaling
}
static void wp_viewport_set_destination(wp_viewport *v,int w,int h){v->width=w;v->height=h;assert(w==5 && h==5);}
static void wl_subsurface_set_position(wl_subsurface *s,int x,int y){s->x=x;s->y=y;}
static void wl_surface_attach(wl_surface *s,wl_buffer *b,int,int){s->pending=b;}
static void wl_surface_damage(wl_surface*,int,int,int,int){}
static wl_surface *gdk_wayland_window_get_wl_surface(int){return &parent;}
static int gtk_widget_get_window(int){return 1;}
static bool gtk_widget_translate_coordinates(int,int,int,int,int *x,int *y){*x=7;*y=32;return true;}
static int gdk_display_get_default(){return 1;}
static GdkPixbuf *gdk_pixbuf_new(unsigned,bool,int,int w,int h){return new GdkPixbuf{w,h};}
static void gdk_pixbuf_fill(GdkPixbuf *image,unsigned rgba){assert(rgba==0);image->transparent=true;}
static GdkCursor *gdk_cursor_new_from_pixbuf(int,GdkPixbuf *image,int x,int y){
    assert(image->transparent);++transparent_allocations;
    return new GdkCursor{true,image->width,image->height,x,y};
}
static void gdk_window_set_cursor(int,GdkCursor *c){
    ++native_calls;
    // GTK applies the hotspot while the previous image is still attached.
    // A 0,0 blank hotspot would shift that old image before hiding it.
    if(native_visible && c->blank) {
        assert(c->hot_x==native_cursor.hot_x && c->hot_y==native_cursor.hot_y);
        assert(c->width==native_cursor.width && c->height==native_cursor.height);
    }
    native_visible=!c->blank;
    // The image must be published, not merely prepared, before native hiding.
    if(c->blank && cursor_visible_requested)assert(child && child->shown);
}
static void g_object_unref(GdkCursor*){}
static void g_object_unref(GdkPixbuf *image){delete image;}
static void wl_surface_commit(wl_surface *s){
    if(s==child){child_committed=true;return;}
    assert(s==&parent && child_committed);
    child->shown=child->pending;sub->shown_x=sub->x;sub->shown_y=sub->y;
    if(viewport){viewport->shown_x=viewport->x;viewport->shown_y=viewport->y;}
    if(cursor_visible_requested)assert(native_visible || child->shown);
    child_committed=false;
}
static void wl_display_flush(int){
    ++flushes;
    assert(!child_committed);
    // At every published state there is exactly one intended cursor.
    if(cursor_visible_requested)assert(native_visible != bool(child && child->shown));
    else assert(!native_visible && (!child || !child->shown));
}
'''

CHECKS = r'''
int main(int argc,char**) {
    if(argc>1)cursor_viewporter=nullptr; // compositor without viewporter support
    int expected_y=cursor_viewporter?79:80;
    cursor_pixels.resize(cursor_pitch*cursor_height);
    for(unsigned i=0;i<cursor_pixels.size();++i)cursor_pixels[i]=i;
    cursor_anchor_x=100.25;cursor_anchor_y=50.5;
    pointer_capture.focused=true;cursor_inside=true;
    // The same cursor is already visible over the focused game before RMB.
    assert(!pointer_capture.requested);
    update_capture_cursor();
    assert(capture_cursor_mapped && !native_visible && child->shown);
    assert(sub->shown_x==105 && sub->shown_y==expected_y && child->empty);
    int pad=cursor_viewporter?1:0,stride=(4+pad*2)*4;
    for(int row=0;row<4;++row)for(int col=0;col<16;++col)
        assert(child->shown->pixels[(row+pad)*stride+pad*4+col]==cursor_pixels[row*20+col]);
    unsigned first=allocations;
    unsigned before_press=flushes,before_native=native_calls;
    right_button_cursor=true;update_capture_cursor();
    assert(flushes==before_press && native_calls==before_native && allocations==first);
    // The delayed engine request must preserve the already visible press anchor.
    pointer_capture.requested=true;right_button_cursor=false;update_capture_cursor();
    assert(allocations==first && sub->shown_x==105 && sub->shown_y==expected_y);
    for(int i=0;i<10;++i)update_capture_cursor();
    assert(allocations==first && sub->shown_x==105 && sub->shown_y==expected_y);
    assert(transparent_allocations==1); // repeated updates reuse the invisible cursor
    // Resume normal tracking with no representation change on release. A burst
    // of 1,000 positions publishes only its final position and allocates no image.
    pointer_capture.requested=false;update_capture_cursor();
    assert(native_calls==before_native && allocations==first);
    for(int i=0;i<1000;++i)track_cursor_position(100+i/256.0,50+i/256.0);
    assert(idle_calls==1 && cursor_update_source && allocations==first);
    idle_callback(nullptr);
    assert(!cursor_update_source && allocations==first && cursor_anchor_x==100+999/256.0);
    double held_x=cursor_anchor_x,held_y=cursor_anchor_y;
    before_native=native_calls;before_press=flushes;
    right_button_cursor=true;update_capture_cursor();
    for(int i=0;i<1000;++i)track_cursor_position(200+i/256.0,100+i/256.0);
    assert(cursor_anchor_x==held_x && cursor_anchor_y==held_y && idle_calls==1);
    right_button_cursor=false;update_capture_cursor();
    for(int i=0;i<250;++i){right_button_cursor=true;update_capture_cursor();right_button_cursor=false;update_capture_cursor();}
    assert(native_calls==before_native && flushes==before_press && allocations==first);
    puts("PASS 1,000 normal motions coalesced without texture allocation; held motion and 250 stationary clicks do not change cursor representation");
    cursor_anchor_x=100.25;cursor_anchor_y=50.5;pointer_capture.requested=true;update_capture_cursor();
    if(viewport) {
        unsigned old_mismatches=0;
        for(int fraction=0;fraction<256;++fraction) {
            cursor_anchor_x=100+fraction/256.0;cursor_anchor_y=50+((fraction*37)%256)/256.0;
            update_capture_cursor();
            double actual_x=sub->shown_x+1-viewport->shown_x+cursor_x-7;
            double actual_y=sub->shown_y+1-viewport->shown_y+cursor_y-32;
            assert(actual_x==cursor_anchor_x && actual_y==cursor_anchor_y);
            assert(std::round(actual_x+947.95)==std::round(cursor_anchor_x+947.95));
            assert(std::round(actual_y+545.15)==std::round(cursor_anchor_y+545.15));
            old_mismatches+=std::round(std::round(cursor_anchor_x)+947.95)!=std::round(cursor_anchor_x+947.95);
            old_mismatches+=std::round(std::round(cursor_anchor_y)+545.15)!=std::round(cursor_anchor_y+545.15);
        }
        assert(old_mismatches>0 && allocations==first);
        printf("PASS all 256 fractional anchor phases at observed fractional window origins; old rounding mismatched %u placements\n",old_mismatches);
        cursor_anchor_x=100.25;cursor_anchor_y=50.5;update_capture_cursor();
    }
    // A cursor tracking refresh uses the same anchor and updates only pixels.
    cursor_pixels[0]=255;capture_cursor_dirty=true;update_capture_cursor();
    assert(allocations==first+1 && child->shown->pixels[pad*stride+pad*4]==255);
    assert(sub->shown_x==105 && sub->shown_y==expected_y);
    // Shift Lock's intentional engine anchor change remains supported.
    cursor_anchor_x=500;cursor_anchor_y=300;update_capture_cursor();
    assert(sub->shown_x==505 && sub->shown_y==329 && allocations==first+1);
    cursor_visible_requested=false;update_capture_cursor();
    assert(!capture_cursor_mapped && !native_visible && !child->shown);
    cursor_visible_requested=true;update_capture_cursor();
    assert(capture_cursor_mapped && !native_visible && child->shown);
    pointer_capture.focused=false;update_capture_cursor();
    assert(!capture_cursor_mapped && native_visible && !child->shown);
    pointer_capture.requested=false;pointer_capture.focused=true;update_capture_cursor();
    assert(capture_cursor_mapped && !native_visible && !pointer_capture.requested);
    cursor_inside=false;update_capture_cursor();
    assert(!capture_cursor_mapped && native_visible && !child->shown);
    // Resource failure must leave the native cursor visible, never blank it.
    pointer_capture.requested=true;cursor_shm=nullptr;update_capture_cursor();
    assert(!capture_cursor_mapped && native_visible && !child->shown);
    cursor_shm=&shm;cursor_pixels.clear();update_capture_cursor();
    assert(!capture_cursor_mapped && native_visible && !child->shown);
    // Replay Roblox's 64x64 cursor with its nonzero 32,32 hotspot.
    cursor_width=cursor_height=64;cursor_x=cursor_y=32;
    auto transparent=transparent_capture_cursor();
    assert(transparent->blank && transparent->width==64 && transparent->height==64);
    assert(transparent->hot_x==32 && transparent->hot_y==32);
    assert(transparent_capture_cursor()==transparent && transparent_allocations==2);
    for(auto b:buffers)wl_buffer_destroy(b);
    delete child;delete sub;delete viewport;
    puts("PASS cursor buffer rows/hotspot, transparent native hotspot/cache, prepare-before-hide, parent commit, fixed anchor, tracking refresh, Shift Lock anchor, intentional hide, release, blur/regain and allocation-failure visibility");
}
'''

ANCHOR_CHECK = r'''
#include <cassert>
#include <cstdio>
struct NSPoint {double x,y;};
static NSPoint pointer,anchor;
static bool locked,captureSuspended,focused;
static RbxRightDragAnchor rightDragAnchor;
static struct Window {void *native;} window;
static Window *gameWindow=&window;
static unsigned updates,tracking;
enum {RBX_WL_CURSOR_ANCHOR=1,RBX_WL_WARP=2};
static void markCursorTrackingDirty(){++tracking;}
static void record_action(void*,int,double,double,const char*){++updates;}
static struct API {decltype(&record_action) action;} api_object{record_action};
static API *api=&api_object;
'''
ANCHOR_TEST = r'''
int main() {
    locked=focused=true;anchor={675.4,239.9};
    rbxRightDragBegin(&rightDragAnchor,anchor.x,anchor.y,true);
    rbxRightDragTrack(&rightDragAnchor,674.45,239.9);
    rbxRightDragTrack(&rightDragAnchor,673.5,239.9);
    warpPointer({673.5,239.9});
    assert(anchor.x==675.4 && anchor.y==239.9 && updates==0 && tracking==0);
    // A genuinely new engine anchor must still reach the host (Shift Lock).
    warpPointer({550,400});
    assert(anchor.x==550 && anchor.y==400 && updates==1 && tracking==1);
    // A late stale restore cannot undo the newly requested center either.
    warpPointer({673.5,239.9});
    assert(anchor.x==550 && anchor.y==400 && updates==1);
    rightDragAnchor.active=false;
    warpPointer({673.5,239.9});
    assert(anchor.x==673.5 && updates==2);
    locked=false;warpPointer({100,150});
    assert(pointer.x==100 && pointer.y==150 && updates==3);
    puts("PASS captured stale-restore replay, original press anchor, new Shift Lock anchor, late restore, reset and ordinary engine warps");
}
'''

PIXEL_STUBS = r'''
#include <cairo/cairo.h>
#include <cassert>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
struct GdkCursor {cairo_surface_t *image;double x,y;};
static int gdk_display_get_default(){return 1;}
static GdkCursor *gdk_cursor_new_from_surface(int,cairo_surface_t *source,double x,double y) {
    // Exercise GTK Wayland's synchronous Cairo paint into its cursor storage.
    auto image=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,cairo_image_surface_get_width(source),cairo_image_surface_get_height(source));
    auto context=cairo_create(image);cairo_set_source_surface(context,source,0,0);
    cairo_paint(context);cairo_destroy(context);cairo_surface_flush(image);
    return new GdkCursor{image,x,y};
}
'''
PIXEL_TEST = r'''
int main() {
    const int width=256,height=256,pitch=1040;
    std::vector<unsigned char> pixels(pitch*height,0xad);
    unsigned old_changed=0;
    for(int a=0;a<height;++a)for(int x=0;x<width;++x) {
        auto p=pixels.data()+a*pitch+x*4;
        p[0]=std::min((x*17)%256,a);p[1]=std::min((x*7)%256,a);p[2]=std::min(x,a);p[3]=a;
        if(a)for(int c=0;c<3;++c)old_changed+=((p[c]*255/a)*a+127)/255!=p[c];
    }
    auto original=pixels;
    auto cursor=native_image_cursor(pixels.data(),width,height,pitch,32,32);
    assert(cursor && cursor->x==32 && cursor->y==32 && pixels==original);
    auto actual=cairo_image_surface_get_data(cursor->image);
    int stride=cairo_image_surface_get_stride(cursor->image);
    for(int row=0;row<height;++row)assert(!memcmp(actual+row*stride,pixels.data()+row*pitch,width*4));
    assert(old_changed>0);
    cairo_surface_destroy(cursor->image);delete cursor;
    printf("PASS native/pinned pixel identity over 65,536 alpha samples and padded rows; old conversion changed %u channels\n",old_changed);
}
'''

with tempfile.TemporaryDirectory(prefix="cursor-handoff-") as directory:
    test = Path(directory) / "check.cpp"
    test.write_text(STUBS + surface + handoff + CHECKS)
    binary = Path(directory) / "check"
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    subprocess.run([str(binary), "--without-viewporter"], check=True)
    test.write_text(PIXEL_STUBS + image_cursor + PIXEL_TEST)
    cairo_flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "cairo"], text=True))
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(binary), *cairo_flags], check=True)
    subprocess.run([str(binary)], check=True)
    # Detect both the old hotspot reset and visibility gaps between requests.
    entry = "    if(mapped)gdk_window_set_cursor(target,blank?blank:gtk_cursor);\n"
    release = "    if(!mapped)gdk_window_set_cursor(target,blank?blank:gtk_cursor);\n"
    mutations = [
        ("RMB-dependent cursor representation", handoff.replace("cursor_inside || ", "")),
        ("zero-hotspot cursor flash", handoff.replace("gdk_display_get_default(),image,x,y)", "gdk_display_get_default(),image,0,0)")),
        ("hide before replacement commit", handoff.replace(entry, "").replace(release, entry + release)),
        ("remove before native restore", handoff.replace(release, "").replace(entry, entry + release)),
    ]
    for label, mutant in mutations:
        assert mutant != handoff
        test.write_text(STUBS + surface + mutant + CHECKS)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        assert result.returncode != 0, label + " regression was not detected"
        print("PASS " + label + " mutation rejected")
    rounded = surface.replace("std::floor(cursor_anchor_x)", "std::round(cursor_anchor_x)")
    assert rounded != surface
    test.write_text(STUBS + rounded + handoff + CHECKS)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert result.returncode != 0, "Fractional-anchor rounding regression was not detected"
    print("PASS fractional-anchor rounding mutation rejected")
    test.write_text('#include "' + str(ROOT / "runtime/browser/right-drag-anchor.h") + '"\n' + ANCHOR_CHECK + warp + ANCHOR_TEST)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
