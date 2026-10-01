// Optional CPU-side swap-return timestamps. These are not GPU/display timings.
#include <time.h>
static FILE *frame_trace;
static pid_t frame_trace_owner;
static void (*original_frame_flush)(id,SEL);
static unsigned long long (*gl_swap_serial)(void);
static uint64_t frame_now(void) {
    struct timespec t;
    if(trackb_clock_gettime(CLOCK_MONOTONIC,&t))abort();
    return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
static void traced_frame_flush(id object,SEL selector) {
    unsigned long long serial=gl_swap_serial?gl_swap_serial():0;
    uint64_t start=frame_now();
    original_frame_flush(object,selector);
    uint64_t end=frame_now();
    // Cocoa also flushes when Vulkan owns the window; those are skipped swaps.
    if((!gl_swap_serial || gl_swap_serial()!=serial) && getpid()==frame_trace_owner)fprintf(frame_trace,"%p,%llu,%llu\n",object,(unsigned long long)end,(unsigned long long)(end-start));
}
static void flush_frame_trace(void) {if(frame_trace && getpid()==frame_trace_owner)fflush(frame_trace);}
static void close_frame_trace(void) {if(getpid()==frame_trace_owner)fclose(frame_trace);}
static int init_frame_trace(void) {
    const char *path=getenv("TRACKB_FRAME_TRACE");
    if(!path)return 1;
    gl_swap_serial=dlsym(RTLD_DEFAULT,"rbxWaylandGLSwapSerial");
    Method method=class_getInstanceMethod(objc_getClass("CALayerContext"),sel_registerName("flush"));
    if(!method){fprintf(stderr,"Frame trace: CALayerContext.flush unavailable\n");return 0;}
    // Exclusive creation preserves earlier measurements. Buffer writes to keep
    // file I/O out of most frames; an abrupt exit can lose the buffered tail.
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);
    if(fd<0){perror("Frame trace");return 0;}
    frame_trace=fdopen(fd,"w");
    if(!frame_trace){close(fd);perror("Frame trace stream");return 0;}
    frame_trace_owner=getpid();
    setvbuf(frame_trace,NULL,_IOFBF,65536);
    fprintf(frame_trace,"context,end_ns,swap_ns\n");
    atexit(close_frame_trace);
    original_frame_flush=(void(*)(id,SEL))method_setImplementation(method,(IMP)traced_frame_flush);
    return 1;
}
