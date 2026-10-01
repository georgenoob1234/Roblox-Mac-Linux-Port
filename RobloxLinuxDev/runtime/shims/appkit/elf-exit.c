// Darling's sys_exit calls native_exit, whose host hook is libc exit(), not _exit().
// A forked Crashpad intermediate therefore runs inherited NVIDIA destructors, corrupting
// the parent's GPU resources. POSIX _exit must not invoke those destructors.
// Prefix from darling/src/startup/mldr/elfcalls/elfcalls.h; validated before changing it.
struct ElfCalls {
    void *(*dlopen)(const char *);
    int (*dlclose)(void *);
    void *(*dlsym)(void *, const char *);
    void *remaining_loader_thread_posix_calls[16];
    void (*exit)(int);
};
extern struct ElfCalls *_elfcalls;
extern long write(int, const void *, unsigned long);

__attribute__((constructor)) static void correct_native_exit(void) {
    if (!_elfcalls) return;
    void *libc = _elfcalls->dlopen("libc.so.6");
    if (!libc) return;
    void (*normal_exit)(int) = _elfcalls->dlsym(libc, "exit");
    void (*immediate_exit)(int) = _elfcalls->dlsym(libc, "_exit");
    if (immediate_exit && _elfcalls->exit == normal_exit) {
        _elfcalls->exit = immediate_exit;
    } else {
        static const char message[] = "roblox-mac: unrecognized Darling exit hook; leaving it unchanged\n";
        write(2,message,sizeof(message)-1);
    }
    // Keep libc loaded for the lifetime of its installed function pointer.
}
