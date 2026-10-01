#include <OpenGL/OpenGL.h>
// Metal's property is stored by Cocotron, but its EGL presenter ignored it.
@interface NSObject (TrackBDisplaySync)
- (BOOL)displaySyncEnabled;
@end
static void (*original_prepare_metal)(id,SEL);
static int presentation_interval=-1;
static void prepare_metal_with_sync(id layer,SEL selector) {
    original_prepare_metal(layer,selector);
    CGLContextObj context=CGLGetCurrentContext();
    GLint desired=presentation_interval>=0?presentation_interval:([layer displaySyncEnabled]?1:0),current=-1;
    if(!context || CGLGetParameter(context,kCGLCPSwapInterval,&current))return;
    static CGLContextObj reported;
    if(reported!=context) {
        fprintf(stderr,"Metal presentation: EGL interval %d, requested %d (%s)\n",current,desired,presentation_interval>=0?"launcher override":"Metal displaySyncEnabled");
        reported=context;
    }
    if(current==desired)return;
    CGLError error=CGLSetParameter(context,kCGLCPSwapInterval,&desired);
    // Only log an actual policy change, never every frame.
    fprintf(stderr,"Track B: Metal display sync -> EGL interval %d, status %d\n",desired,error);
}
static int init_metal_presentation(void) {
    const char *interval=getenv("ROBLOX_MAC_SWAP_INTERVAL");
    if(interval && *interval) {
        if(strcmp(interval,"0") && strcmp(interval,"1")) {
            fputs("ROBLOX_MAC_SWAP_INTERVAL must be 0 or 1\n",stderr);return 0;
        }
        presentation_interval=interval[0]-'0';
    }
    int (*cleanup)(void)=dlsym(RTLD_DEFAULT,"trackb_install_drawable_cleanup");
    if(!cleanup || !cleanup())return 0;
    if(getenv("RBX_PRESENT_RESOURCE_TRACE")){Dl_info image={0};if(dladdr((void*)cleanup,&image))fprintf(stderr,"Track B renderer: %s\n",image.dli_fname);}
    Method method=class_getInstanceMethod(objc_getClass("CAMetalLayerInternal"),sel_registerName("prepareRender"));
    if(!method)return 0;
    original_prepare_metal=(void(*)(id,SEL))method_setImplementation(method,(IMP)prepare_metal_with_sync);
    return 1;
}
