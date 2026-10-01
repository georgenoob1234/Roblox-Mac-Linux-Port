// Text Input Services stubs that return real objects instead of NULL.
//
// Roblox reads the current keyboard layout at startup (Contents/MacOS/RobloxPlayer + 0x50873a2):
//     src  = TISCopyCurrentKeyboardLayoutInputSource();
//     data = TISGetInputSourceProperty(src, kTISPropertyUnicodeKeyLayoutData);
//     CFDataCreateCopy(kCFAllocatorDefault, data);      // <- SIGSEGV when data is NULL
// Darling has no Text Input Services at all, so shims/fill generated stubs returning 0, and
// CoreFoundation dereferenced the NULL. Returning an empty CFData keeps the call chain alive.
//
// UCKeyTranslate uses the native keyboard layout for shortcut labels. Committed
// text still arrives through NSEvent/NSTextView, including the native IME.
typedef const void *CFTypeRef;
typedef const struct __CFData *CFDataRef;
typedef const struct __CFString *CFStringRef;
typedef const struct __CFAllocator *CFAllocatorRef;
typedef unsigned short UniChar;
typedef unsigned long UniCharCount;

extern CFDataRef CFDataCreate(CFAllocatorRef, const unsigned char *, long);
extern CFStringRef CFStringCreateWithCString(CFAllocatorRef, const char *, unsigned int);
extern unsigned char CFStringGetCString(CFStringRef,char *,long,unsigned int);
extern CFTypeRef CFArrayCreate(CFAllocatorRef,const void **,long,const void *);
extern int strcmp(const char *,const char *);
extern char *getenv(const char *);
extern void dispatch_once_f(long *,void *,void (*)(void *));
struct InputElfCalls {void *(*open)(const char *);int (*close)(void *);void *(*symbol)(void *,const char *);};
extern struct InputElfCalls *_elfcalls;
static unsigned int (*keyCharacter)(unsigned int,unsigned int);
static void loadKeyboard(void *unused) {
    (void)unused;
    const char *path=getenv("ROBLOX_MAC_WAYLAND_HELPER");
    if(!_elfcalls || !path)return;
    void *library=_elfcalls->open(path);
    if(library)keyCharacter=_elfcalls->symbol(library,"rbx_wayland_key_character");
}
static CFDataRef emptyLayout;
static CFTypeRef languages;
static void initProperties(void *unused) {
    (void)unused;
    emptyLayout=CFDataCreate(0,(const unsigned char *)"",0);
    // No macOS IME is connected. Ordinary committed text comes from NSEvent.
    languages=CFArrayCreate(0,0,0,0);
}

/* Copy: the caller owns the result and releases it, so hand out a fresh object each time. */
CFTypeRef TISCopyCurrentKeyboardLayoutInputSource(void) {
    return CFStringCreateWithCString(0, "roblox-mac.stub.keyboard-layout", 0x08000100 /* UTF-8 */);
}
CFTypeRef TISCopyCurrentKeyboardInputSource(void) {
    return TISCopyCurrentKeyboardLayoutInputSource();
}

/* Get properties have distinct types and remain owned by the source. */
CFTypeRef TISGetInputSourceProperty(CFTypeRef source, CFStringRef key) {
    static long once;
    char name[128];
    if(!source || !key || !CFStringGetCString(key,name,sizeof(name),0x08000100))return 0;
    dispatch_once_f(&once,0,initProperties);
    if(!strcmp(name,"kTISPropertyUnicodeKeyLayoutData"))return emptyLayout;
    if(!strcmp(name,"kTISPropertyInputSourceLanguages"))return languages;
    return 0;
}

/* The client requests kUCKeyActionDisplay, then uppercases the returned text. */
int UCKeyTranslate(const void *layout, unsigned short key, unsigned short action,
                   unsigned int modifiers, unsigned int kbdType, unsigned int options,
                   unsigned int *deadKeyState, UniCharCount maxLen,
                   UniCharCount *actualLen, UniChar *out) {
    (void)layout; (void)kbdType; (void)options;
    if (deadKeyState) *deadKeyState = 0;
    if (actualLen) *actualLen = 0;
    if (out && maxLen) out[0] = 0;
    if (!actualLen || !out || action > 3) return -50; /* paramErr */
    static long once;
    dispatch_once_f(&once,0,loadKeyboard);
    // ponytail: labels only; composition stays in the native IME, not Carbon dead-key state.
    unsigned int character=keyCharacter?keyCharacter(key,modifiers):0;
    if (!character || character > 0x10ffff || (character >= 0xd800 && character <= 0xdfff)) return 0;
    UniCharCount count=character > 0xffff?2:1;
    if (maxLen < count) return -25340; /* kUCOutputBufferTooSmall */
    if (count == 1) out[0]=(UniChar)character;
    else {
        character-=0x10000;
        out[0]=(UniChar)(0xd800+(character>>10));out[1]=(UniChar)(0xdc00+(character&0x3ff));
    }
    *actualLen=count;
    return 0;
}
