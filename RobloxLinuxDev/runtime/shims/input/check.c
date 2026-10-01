// Run by build.sh on the host; exercise the production shim with a native helper double.
#include "input.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned int character;
static unsigned int lookup(unsigned int key,unsigned int modifiers) {
    assert(key == 16 && modifiers == 0x100); // Roblox's Y display-label call.
    return character;
}
static void *openHelper(const char *path) {assert(!strcmp(path,"keyboard-check"));return (void *)1;}
static void *symbol(void *library,const char *name) {
    assert(library == (void *)1 && !strcmp(name,"rbx_wayland_key_character"));return lookup;
}
static struct InputElfCalls calls={openHelper,0,symbol};
struct InputElfCalls *_elfcalls=&calls;
void dispatch_once_f(long *once,void *context,void (*function)(void *)) {
    if(!*once){*once=1;function(context);}
}

int main(void) {
    assert(!setenv("ROBLOX_MAC_WAYLAND_HELPER","keyboard-check",1));
    UniChar output[3];UniCharCount length;unsigned int dead;
    const unsigned int characters[]={'y','e','/',0x00e9,0x00b2,0x1f600,0,0xd800,0x110000};
    for(unsigned i=0;i<sizeof(characters)/sizeof(*characters);++i) {
        character=characters[i];length=999;dead=42;
        output[0]=output[1]=output[2]=0xbeef;
        assert(!UCKeyTranslate(0,16,3,0x100,0,0,&dead,2,&length,output));
        assert(!dead && output[2] == 0xbeef);
        if(character == 0x1f600)assert(length == 2 && output[0] == 0xd83d && output[1] == 0xde00);
        else if(character && character < 0xd800)assert(length == 1 && output[0] == character && output[1] == 0xbeef);
        else assert(!length && !output[0] && output[1] == 0xbeef);
    }
    character=0x1f600;
    assert(UCKeyTranslate(0,16,3,0x100,0,0,&dead,1,&length,output) == -25340 && !length);
    output[0]=0xbeef;
    assert(UCKeyTranslate(0,16,3,0x100,0,0,&dead,0,&length,output) == -25340 && output[0] == 0xbeef);
    assert(UCKeyTranslate(0,16,3,0x100,0,0,&dead,2,&length,0) == -50);
    assert(UCKeyTranslate(0,16,3,0x100,0,0,&dead,2,0,output) == -50);
    keyCharacter=0;length=999;output[0]=0xbeef;
    assert(!UCKeyTranslate(0,16,3,0x100,0,0,&dead,2,&length,output) && !length && !output[0]);
    puts("PASS keyboard labels, UTF-16, output bounds and unavailable helper");
}
