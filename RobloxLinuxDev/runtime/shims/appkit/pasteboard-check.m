// Round-trip coverage for the Wayland pasteboard bridge, run against the real
// RbxWaylandPasteboard class and a fake in-memory Linux clipboard.
#import <Foundation/NSObject.h>
#import <Foundation/NSArray.h>
#import <Foundation/NSString.h>
#import <Foundation/NSData.h>
#import <Foundation/NSAutoreleasePool.h>
#import <Foundation/NSObjCRuntime.h>
#include "../../browser/wayland-api.h"
#include <stdio.h>
#include <string.h>

@interface RbxWaylandPasteboard : NSObject
- (id)initWithName:(NSString*)name;
- (NSInteger)changeCount;
- (NSInteger)clearContents;
- (NSArray*)types;
- (NSData*)dataForType:(NSString*)type;
- (NSString*)stringForType:(NSString*)type;
- (BOOL)setData:(NSData*)data forType:(NSString*)type;
- (BOOL)setString:(NSString*)string forType:(NSString*)type;
- (BOOL)writeObjects:(NSArray*)objects;
@end

extern void rbxWaylandPasteboardSetClipboardForTest(const struct RbxWaylandAPI *test_api);

static char clipboard_text[8192];
static int failures;
static int checks;

static const char *fake_clipboard(const char *value) {
    if(value) {
        size_t length=strlen(value);
        if(length>=sizeof(clipboard_text))length=sizeof(clipboard_text)-1;
        memcpy(clipboard_text,value,length);clipboard_text[length]=0;
        return clipboard_text;
    }
    return clipboard_text;
}

static const struct RbxWaylandAPI test_api = {
    .clipboard=fake_clipboard
};

static void expect(BOOL condition,const char *message) {
    checks++;
    if(!condition) {
        fprintf(stderr,"FAIL: %s (clipboard=%s)\n",message,clipboard_text);
        failures++;
    }
}

static NSStringEncoding encodingForType(NSString *type) {
    return [type isEqual:@"NSStringPboardType"]?NSUTF16LittleEndianStringEncoding:NSUTF8StringEncoding;
}

static void checkText(RbxWaylandPasteboard *board,NSString *type,NSString *text,const char *label) {
    expect([board setString:text forType:type],label);
    expect([[board stringForType:type] isEqual:text],label);
    const char *utf8=[text UTF8String];
    expect(utf8 && !strcmp(clipboard_text,utf8),label);
    NSData *data=[board dataForType:type];
    expect(data!=nil,label);
    if(data) {
        NSString *decoded=[[[NSString alloc] initWithData:data encoding:encodingForType(type)] autorelease];
        expect([decoded isEqual:text],label);
    }
    NSData *encoded=[text dataUsingEncoding:encodingForType(type)];
    expect(encoded!=nil,label);
    if(encoded) {
        expect([board setData:encoded forType:type],label);
        expect([[board stringForType:type] isEqual:text],label);
    }
}

int main(void) {
    NSAutoreleasePool *pool=[NSAutoreleasePool new];
    rbxWaylandPasteboardSetClipboardForTest(&test_api);
    Class cls=NSClassFromString(@"RbxWaylandPasteboard");
    expect(cls!=Nil,"RbxWaylandPasteboard class present");
    if(!cls) {
        [pool release];
        return 1;
    }
    RbxWaylandPasteboard *general=[[cls alloc] initWithName:@"NSGeneralPboard"];
    RbxWaylandPasteboard *named=[[cls alloc] initWithName:@"NSFindPboard"];
    NSArray *types=[general types];
    expect([types containsObject:@"NSStringPboardType"],"general advertises NSStringPboardType");
    expect([types containsObject:@"public.utf8-plain-text"],"general advertises public.utf8-plain-text");

    NSString *utf16=@"NSStringPboardType",*utf8=@"public.utf8-plain-text";
    const char *samples[]={"Hello world 123","caf\xc3\xa9","\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 \xd0\xbc\xd0\xb8\xd1\x80",
        "\xe4\xb8\xad\xe6\x96\x87\xe6\xb5\x8b\xe8\xaf\x95","emoji \xf0\x9f\x99\x82 \xf0\x9f\x9a\x80",
        "line one\nline two\n",""};
    for(unsigned i=0;i<sizeof(samples)/sizeof(samples[0]);i++) {
        NSString *text=[NSString stringWithUTF8String:samples[i]];
        checkText(general,utf16,text,"UTF-16 round trip");
        checkText(general,utf8,text,"UTF-8 round trip");
        // A writer using either declared format must be readable through both.
        [general setString:text forType:utf16];
        expect([[general stringForType:utf8] isEqual:text],"UTF-16 write readable as UTF-8");
        [general setString:text forType:utf8];
        expect([[general stringForType:utf16] isEqual:text],"UTF-8 write readable as UTF-16");
    }

    // Replacement and empty values must clear the previous contents.
    [general setString:@"first" forType:utf16];
    [general setString:@"second" forType:utf8];
    expect(!strcmp(clipboard_text,"second"),"replacement updates Linux clipboard");
    expect([[general stringForType:utf16] isEqual:@"second"],"replacement reflected in UTF-16");
    [general setString:@"" forType:utf16];
    expect(!strcmp(clipboard_text,""),"empty string clears Linux clipboard");
    expect([[general stringForType:utf8] isEqual:@""],"empty string reads back empty");

    // Invalid input must fail without writing or crashing.
    expect(![general setString:nil forType:utf16],"nil string rejected");
    expect(![general setData:nil forType:utf16],"nil data rejected");
    expect([general stringForType:nil]==nil,"nil type reads nil");
    strcpy(clipboard_text,"\xff\xfe\x00");
    expect([general stringForType:utf8]==nil,"invalid UTF-8 rejected");
    strcpy(clipboard_text,"");
    expect([general stringForType:utf8]!=nil,"empty clipboard readable");

    // Named boards keep their own values and never overwrite the desktop.
    [general setString:@"general text" forType:utf16];
    [named setString:@"named text" forType:utf16];
    expect(!strcmp(clipboard_text,"general text"),"named board leaves Linux clipboard alone");
    expect([[named stringForType:utf16] isEqual:@"named text"],"named board stores its own value");
    expect([[general stringForType:utf16] isEqual:@"general text"],"general board keeps its value");
    [named writeObjects:[NSArray arrayWithObject:@"named objects"]];
    expect(!strcmp(clipboard_text,"general text"),"named writeObjects leaves Linux clipboard alone");
    expect([[named stringForType:utf8] isEqual:@"named objects"],"named writeObjects is readable");

    // Roblox's copy path: writeObjects: with an NSString must publish both
    // text formats and must not raise an unknown selector.
    [general clearContents];
    expect([general writeObjects:[NSArray arrayWithObject:@"Hello world 123"]],"writeObjects accepts NSString");
    expect(!strcmp(clipboard_text,"Hello world 123"),"writeObjects updates Linux clipboard");
    expect([[general stringForType:utf16] isEqual:@"Hello world 123"],"writeObjects UTF-16 readable");
    expect([[general stringForType:utf8] isEqual:@"Hello world 123"],"writeObjects UTF-8 readable");
    [general writeObjects:[NSArray arrayWithObject:@"caf\xc3\xa9 \xf0\x9f\x99\x82"]];
    expect([[general stringForType:utf8] isEqual:@"caf\xc3\xa9 \xf0\x9f\x99\x82"],"writeObjects preserves Unicode");

    // The copy path must preserve every text shape the editor can select.
    const char *copied[]={"Hello world 123","\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 \xd0\xbc\xd0\xb8\xd1\x80",
        "\xe4\xb8\xad\xe6\x96\x87\xe6\xb5\x8b\xe8\xaf\x95","emoji \xf0\x9f\x99\x82 \xf0\x9f\x9a\x80",
        "line one\nline two\n"};
    for(unsigned i=0;i<sizeof(copied)/sizeof(copied[0]);i++) {
        NSString *text=[NSString stringWithUTF8String:copied[i]];
        expect([general writeObjects:[NSArray arrayWithObject:text]],"copy writeObjects accepts selected text");
        expect([[general stringForType:utf16] isEqual:text],"selected text reaches UTF-16 clipboard");
        expect([[general stringForType:utf8] isEqual:text],"selected text reaches UTF-8 clipboard");
        expect(!strcmp(clipboard_text,[text UTF8String]),"selected text reaches Linux clipboard");
    }
    // An empty selection must be a safe no-op, not a crash or stale value.
    [general setString:@"stale" forType:utf16];
    expect([general writeObjects:[NSArray arrayWithObject:@""]],"empty selection copy accepted");
    expect(!strcmp(clipboard_text,""),"empty selection clears the Linux clipboard");
    expect([[general stringForType:utf16] isEqual:@""],"empty selection reads back empty");
    expect([general writeObjects:[NSArray array]],"empty object array accepted");

    // Clearing and change counts stay consistent for both boards.
    NSInteger before=[general changeCount];
    expect([general clearContents]>before,"clearContents advances changeCount");
    expect([[general stringForType:utf16] isEqual:@""],"clearContents empties general");
    [named clearContents];
    NSString *clearedNamed=[named stringForType:utf16];
    // A named board with no declared value has no string; only the general
    // board sees the Linux clipboard's empty string.
    expect(clearedNamed==nil || [clearedNamed isEqual:@""],"clearContents empties named");

    [general release];
    [named release];
    [pool release];
    if(failures) {
        fprintf(stderr,"FAIL: %d of %d clipboard checks failed\n",failures,checks);
        return 1;
    }
    printf("PASS clipboard UTF-16/UTF-8 round trips, replacement, empty/nil/invalid input, named isolation, and writeObjects\n");
    return 0;
}