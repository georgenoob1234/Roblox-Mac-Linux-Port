#pragma once
#include <stdint.h>
// Plain C ABI between the Mach-O AppKit backend and native GTK/SDL.
struct RbxWaylandEvent {
    uint32_t type,button,key,modifiers,repeat,clicks;
    double x,y,dx,dy;
    char text[256];
};
enum {RBX_WL_MOTION=1,RBX_WL_DOWN,RBX_WL_UP,RBX_WL_SCROLL,RBX_WL_KEY_DOWN,RBX_WL_KEY_UP,RBX_WL_TEXT,RBX_WL_RESIZE,RBX_WL_FOCUS,RBX_WL_BLUR,RBX_WL_CLOSE};
enum {RBX_WL_SHOW=1,RBX_WL_HIDE,RBX_WL_TITLE,RBX_WL_RESIZE_WINDOW,RBX_WL_FULLSCREEN,RBX_WL_LOCK,RBX_WL_WARP,RBX_WL_MINIMIZE,RBX_WL_CURSOR_VISIBLE};
struct RbxWaylandAPI {
    void *(*display)(void);
    void *(*create)(int,int);
    void *(*surface)(void*);
    void (*action)(void*,int,double,double,const char*);
    int (*poll)(struct RbxWaylandEvent*);
    void (*screen)(int*,int*,double*);
    void (*cursor)(const void*,int,int,int,int,int,const char*);
    int (*visible)(void);
    const char *(*clipboard)(const char*);
    const char *(*user_agent)(void);
    const void *(*cursor_image)(int*,int*,int*,int*,int*);
};
