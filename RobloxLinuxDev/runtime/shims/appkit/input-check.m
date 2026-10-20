// Keyboard-dispatch coverage for the Wayland editing-shortcut translation.
#import <Foundation/NSObject.h>
#import <Foundation/NSString.h>
#import <Foundation/NSRange.h>
#import <Foundation/NSAutoreleasePool.h>
#include "../../browser/keyboard-shortcut.h"
#include <stdio.h>

extern BOOL rbxWaylandTextEditingResponder(id responder);
extern BOOL rbxWaylandPerformEditAction(id responder,int action);
extern BOOL rbxWaylandCopyUsesEngine(id responder);
extern NSUInteger rbxWaylandSelectedRangeLength(id responder);

static int failures;
static int checks;

static void expect(BOOL condition,const char *message) {
    checks++;
    if(!condition) {
        fprintf(stderr,"FAIL: %s\n",message);
        failures++;
    }
}

static int copy_calls,paste_calls,cut_calls,select_all_calls;

@interface FakeEditor:NSObject {BOOL editable;NSUInteger selectionLength;}
- (NSString*)string;
- (NSRange)selectedRange;
- (BOOL)isEditable;
- (void)setEditable:(BOOL)value;
- (void)setSelectionLength:(NSUInteger)value;
- (void)copy:(id)sender;
- (void)paste:(id)sender;
- (void)cut:(id)sender;
- (void)selectAll:(id)sender;
@end
@implementation FakeEditor
- (id)init{if((self=[super init]))selectionLength=4;return self;}
- (NSString*)string{return @"text";}
- (NSRange)selectedRange{return NSMakeRange(0,selectionLength);}
- (BOOL)isEditable{return editable;}
- (void)setEditable:(BOOL)value{editable=value;}
- (void)setSelectionLength:(NSUInteger)value{selectionLength=value;}
- (void)copy:(id)sender{(void)sender;copy_calls++;}
- (void)paste:(id)sender{(void)sender;paste_calls++;}
- (void)cut:(id)sender{(void)sender;cut_calls++;}
- (void)selectAll:(id)sender{(void)sender;select_all_calls++;}
@end

// An editor-shaped object that lacks a text store must not be treated as one.
@interface PasteOnly:NSObject
- (void)paste:(id)sender;
@end
@implementation PasteOnly
- (void)paste:(id)sender{(void)sender;paste_calls++;}
@end

// Roblox's IME handler exposes inputHandler and owns the visible selection.
@interface FakeEngineEditor:FakeEditor
- (id)inputHandler;
@end
@implementation FakeEngineEditor
- (id)inputHandler{return nil;}
@end

int main(void) {
    NSAutoreleasePool *pool=[NSAutoreleasePool new];
    FakeEditor *editor=[[FakeEditor new] autorelease];
    FakeEngineEditor *engineEditor=[[FakeEngineEditor new] autorelease];
    NSObject *plain=[[NSObject new] autorelease];
    PasteOnly *pasteOnly=[[PasteOnly new] autorelease];

    expect(!rbxWaylandTextEditingResponder(nil),"nil is not a text editor");
    expect(!rbxWaylandTextEditingResponder(plain),"plain object is not a text editor");
    expect(!rbxWaylandTextEditingResponder(pasteOnly),"paste-only object is not a text editor");
    expect(rbxWaylandTextEditingResponder(editor),"editor-shaped responder is recognized");
    expect(rbxWaylandSelectedRangeLength(editor)==4,"editor reports its native selection length");
    expect(!rbxWaylandCopyUsesEngine(editor),"native selection keeps the copy: route");
    [editor setSelectionLength:0];
    expect(rbxWaylandSelectedRangeLength(editor)==0,"empty native selection is detected");
    expect(rbxWaylandCopyUsesEngine(editor),"empty native selection routes copy through the engine");
    expect(!rbxWaylandCopyUsesEngine(plain) && !rbxWaylandCopyUsesEngine(nil),"non-editors never use the engine copy route");
    [engineEditor setSelectionLength:4];
    expect(rbxWaylandTextEditingResponder(engineEditor),"engine editor is recognized");
    expect(rbxWaylandCopyUsesEngine(engineEditor),"Roblox engine editor uses the engine copy route even with a native selection");
    [editor setSelectionLength:4];

    [editor setEditable:YES];
    expect(rbxWaylandPerformEditAction(editor,RBX_WL_EDIT_COPY) && copy_calls==1,"Ctrl/Cmd+C dispatches copy:");
    expect(rbxWaylandPerformEditAction(editor,RBX_WL_EDIT_PASTE) && paste_calls==1,"Ctrl/Cmd+V dispatches paste:");
    expect(rbxWaylandPerformEditAction(editor,RBX_WL_EDIT_CUT) && cut_calls==1,"Ctrl/Cmd+X dispatches cut:");
    expect(rbxWaylandPerformEditAction(editor,RBX_WL_EDIT_SELECT_ALL) && select_all_calls==1,"Ctrl/Cmd+A dispatches selectAll:");

    paste_calls=0;cut_calls=0;
    [editor setEditable:NO];
    expect(rbxWaylandPerformEditAction(editor,RBX_WL_EDIT_PASTE) && paste_calls==0,"read-only editor consumes paste without inserting");
    expect(rbxWaylandPerformEditAction(editor,RBX_WL_EDIT_CUT) && cut_calls==0,"read-only editor consumes cut without deleting");
    expect(rbxWaylandPerformEditAction(editor,RBX_WL_EDIT_COPY),"copy still allowed in read-only editor");

    expect(!rbxWaylandPerformEditAction(plain,RBX_WL_EDIT_COPY),"non-editor is not dispatched");
    expect(!rbxWaylandPerformEditAction(editor,RBX_WL_EDIT_NONE),"no action for unrelated keys");

    // The mapping itself must not hijack gameplay keys or shifted chords.
    expect(rbxWaylandEditAction(6,RBX_WL_MOD_CONTROL)==RBX_WL_EDIT_COPY,"C maps to copy");
    expect(rbxWaylandEditAction(25,RBX_WL_MOD_CONTROL)==RBX_WL_EDIT_PASTE,"V maps to paste");
    expect(rbxWaylandEditAction(27,RBX_WL_MOD_COMMAND)==RBX_WL_EDIT_CUT,"Cmd+X maps to cut");
    expect(rbxWaylandEditAction(4,RBX_WL_MOD_COMMAND)==RBX_WL_EDIT_SELECT_ALL,"Cmd+A maps to selectAll");
    expect(rbxWaylandEditAction(26,RBX_WL_MOD_CONTROL)==RBX_WL_EDIT_NONE,"Ctrl+W stays gameplay");
    expect(rbxWaylandEditAction(6,RBX_WL_MOD_CONTROL|RBX_WL_MOD_SHIFT)==RBX_WL_EDIT_NONE,"Ctrl+Shift+C stays gameplay");
    expect(rbxWaylandEditAction(6,RBX_WL_MOD_CONTROL|RBX_WL_MOD_ALTERNATE)==RBX_WL_EDIT_NONE,"Ctrl+Alt+C stays gameplay");
    expect(rbxWaylandEditAction(6,0)==RBX_WL_EDIT_NONE,"unmodified C is typing");
    expect(rbxWaylandCopyModifiers(RBX_WL_MOD_CONTROL)==RBX_WL_MOD_COMMAND,"Ctrl+C copy reaches the Command engine path");
    expect(rbxWaylandCopyModifiers(RBX_WL_MOD_COMMAND)==RBX_WL_MOD_COMMAND,"Cmd+C keeps Command");
    expect(rbxWaylandCopyModifiers(RBX_WL_MOD_COMMAND|RBX_WL_MOD_SHIFT)==(RBX_WL_MOD_COMMAND|RBX_WL_MOD_SHIFT),"unrelated modifiers are preserved");

    [pool release];
    if(failures) {
        fprintf(stderr,"FAIL: %d of %d input checks failed\n",failures,checks);
        return 1;
    }
    printf("PASS Ctrl/Cmd editing-action dispatch, empty-selection engine-copy routing, read-only handling, gameplay-key isolation, and modifier guards\n");
    return 0;
}