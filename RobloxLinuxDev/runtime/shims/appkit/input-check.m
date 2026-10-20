// Keyboard-dispatch coverage for the Wayland editing-shortcut translation.
#import <Foundation/NSObject.h>
#import <Foundation/NSString.h>
#import <Foundation/NSRange.h>
#import <Foundation/NSGeometry.h>
#import <Foundation/NSAutoreleasePool.h>
#include "../../browser/keyboard-shortcut.h"
#include "../../browser/keyboard-map.h"
#include <stdio.h>

extern BOOL rbxWaylandTextEditingResponder(id responder);
extern BOOL rbxWaylandPerformEditAction(id responder,int action);
extern BOOL rbxWaylandCopyUsesEngine(id responder);
extern BOOL rbxWaylandEditActionUsesEngine(int action,BOOL engineOwned,BOOL nativeSelectionEmpty);
extern NSUInteger rbxWaylandSelectedRangeLength(id responder);
extern void rbxWaylandSendEngineChord(id engine,unsigned short key,NSString *plain,NSUInteger flags,NSUInteger restore,NSPoint location,NSInteger windowNumber);

// Only the accessors the engine chord assertions need. AppKit is linked but its
// umbrella header is not available in this SDK.
@interface NSEvent:NSObject
- (unsigned short)keyCode;
- (NSString*)characters;
- (NSUInteger)modifierFlags;
@end

static int failures;
static int checks;

static void expect(BOOL condition,const char *message) {
    checks++;
    if(!condition) {
        fprintf(stderr,"FAIL: %s\n",message);
        failures++;
    }
}

static int copy_calls,paste_calls,cut_calls,select_all_calls,undo_calls,redo_calls;

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

// A genuine native editor that owns an undo stack. Cocotron's NSTextView does
// not implement undo:/redo:, but the dispatcher must still use them if present.
@interface UndoableEditor:FakeEditor
- (void)undo:(id)sender;
- (void)redo:(id)sender;
@end
@implementation UndoableEditor
- (void)undo:(id)sender{(void)sender;undo_calls++;}
- (void)redo:(id)sender{(void)sender;redo_calls++;}
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

@interface FakeEngine:NSObject {int key_events;int modifier_events;unsigned first_flags;unsigned last_flags;unsigned short last_key;NSString *last_chars;unsigned last_event_flags;}
- (void)handleKeyEvent:(id)event;
- (void)handleModifiers:(unsigned short)keycode newModifierFlags:(unsigned)flags;
- (int)keyEvents;
- (int)modifierEvents;
- (unsigned)firstFlags;
- (unsigned)lastFlags;
- (unsigned short)lastKey;
- (NSString*)lastCharacters;
- (unsigned)lastEventFlags;
@end
@implementation FakeEngine
- (void)dealloc{[last_chars release];[super dealloc];}
- (void)handleKeyEvent:(id)event{
    key_events++;
    last_key=[event keyCode];
    [last_chars release];last_chars=[[event characters] copy];
    last_event_flags=(unsigned)[event modifierFlags];
}
- (void)handleModifiers:(unsigned short)keycode newModifierFlags:(unsigned)flags{(void)keycode;if(!modifier_events)first_flags=flags;last_flags=flags;modifier_events++;}
- (int)keyEvents{return key_events;}
- (int)modifierEvents{return modifier_events;}
- (unsigned)firstFlags{return first_flags;}
- (unsigned)lastFlags{return last_flags;}
- (unsigned short)lastKey{return last_key;}
- (NSString*)lastCharacters{return last_chars?last_chars:@"";}
- (unsigned)lastEventFlags{return last_event_flags;}
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

    FakeEngine *engine=[[FakeEngine new] autorelease];
    rbxWaylandSendEngineChord(engine,8,@"c",RBX_WL_MOD_COMMAND,RBX_WL_MOD_CONTROL,NSMakePoint(0,0),1);
    expect([engine keyEvents]==2,"engine receives Command+C key down and up");
    expect([engine modifierEvents]==2,"engine modifier state is set and restored");
    expect([engine firstFlags]==RBX_WL_MOD_COMMAND,"engine sees Command while copying");
    expect([engine lastFlags]==RBX_WL_MOD_CONTROL,"engine modifier state is restored to Control");

    // The engine must receive the macOS equivalent of each Linux chord, on the
    // right key, so cut/undo/redo act on the text instead of inserting letters.
    struct {int action;unsigned scan;unsigned modifiers;unsigned short expected_key;const char *expected_chars;unsigned expected_shift;} chords[]={
        {RBX_WL_EDIT_CUT,27,RBX_WL_MOD_CONTROL,macKey(27),"x",0},
        {RBX_WL_EDIT_UNDO,29,RBX_WL_MOD_CONTROL,macKey(29),"z",0},
        {RBX_WL_EDIT_REDO,28,RBX_WL_MOD_CONTROL,macKey(29),"z",1},
        {RBX_WL_EDIT_REDO,29,RBX_WL_MOD_CONTROL|RBX_WL_MOD_SHIFT,macKey(29),"z",1},
        {RBX_WL_EDIT_REDO,29,RBX_WL_MOD_COMMAND|RBX_WL_MOD_SHIFT,macKey(29),"z",1}};
    for(unsigned i=0;i<sizeof(chords)/sizeof(chords[0]);i++) {
        FakeEngine *target=[[FakeEngine new] autorelease];
        unsigned scan=rbxWaylandEngineScancode(chords[i].action,chords[i].scan);
        unsigned flags=rbxWaylandEngineModifiers(chords[i].action,chords[i].modifiers);
        NSString *plain=[NSString stringWithFormat:@"%c",rbxWaylandShortcutLetter(scan)];
        rbxWaylandSendEngineChord(target,macKey(scan),plain,flags,chords[i].modifiers,NSMakePoint(0,0),1);
        expect([target keyEvents]==2,"engine receives a key down and up for each editing chord");
        expect([target lastKey]==chords[i].expected_key,"engine chord uses the expected key code");
        expect([[target lastCharacters] isEqual:[NSString stringWithUTF8String:chords[i].expected_chars]],"engine chord carries the expected letter");
        expect([target firstFlags]==flags,"engine sees the translated modifier flags");
        expect([target lastFlags]==chords[i].modifiers,"engine modifier state is restored after the chord");
        expect(!(([target lastEventFlags]&RBX_WL_MOD_CONTROL) && (flags&RBX_WL_MOD_COMMAND)),"engine never sees a Control+Command chord");
        expect(!!([target firstFlags]&RBX_WL_MOD_SHIFT)==!!chords[i].expected_shift,"Shift is set only for redo");
    }

    // Routing: the engine owns the TextBox state, so cut/undo/redo must not be
    // handed to the native text view when a handler is present.
    expect(rbxWaylandEditActionUsesEngine(RBX_WL_EDIT_CUT,YES,YES),"engine-owned cut routes to the engine");
    expect(rbxWaylandEditActionUsesEngine(RBX_WL_EDIT_UNDO,YES,YES),"engine-owned undo routes to the engine");
    expect(rbxWaylandEditActionUsesEngine(RBX_WL_EDIT_REDO,YES,YES),"engine-owned redo routes to the engine");
    expect(rbxWaylandEditActionUsesEngine(RBX_WL_EDIT_SELECT_ALL,YES,YES),"engine-owned select-all routes to the engine");
    expect(!rbxWaylandEditActionUsesEngine(RBX_WL_EDIT_CUT,NO,NO),"native cut with a selection uses cut:");
    expect(!rbxWaylandEditActionUsesEngine(RBX_WL_EDIT_UNDO,NO,NO),"native editor keeps its own undo:");
    expect(!rbxWaylandEditActionUsesEngine(RBX_WL_EDIT_PASTE,YES,YES),"paste keeps the verified native route");
    expect(rbxWaylandEditActionUsesEngine(RBX_WL_EDIT_COPY,NO,YES),"empty native selection still sends copy to the engine");
    expect(rbxWaylandEditActionUsesEngine(RBX_WL_EDIT_COPY,YES,NO),"engine-owned copy uses the engine");
    expect(!rbxWaylandEditActionUsesEngine(RBX_WL_EDIT_NONE,YES,YES),"unmapped chords are never routed");

    [editor setEditable:YES];
    expect(rbxWaylandPerformEditAction(editor,RBX_WL_EDIT_COPY) && copy_calls==1,"Ctrl/Cmd+C dispatches copy:");
    expect(rbxWaylandPerformEditAction(editor,RBX_WL_EDIT_PASTE) && paste_calls==1,"Ctrl/Cmd+V dispatches paste:");
    expect(rbxWaylandPerformEditAction(editor,RBX_WL_EDIT_CUT) && cut_calls==1,"Ctrl/Cmd+X dispatches cut:");
    expect(rbxWaylandPerformEditAction(editor,RBX_WL_EDIT_SELECT_ALL) && select_all_calls==1,"Ctrl/Cmd+A dispatches selectAll:");
    // Cocotron's NSTextView has no undo:/redo:. The chord is still consumed so
    // 'z' cannot leak, but no edit is simulated.
    expect(rbxWaylandPerformEditAction(editor,RBX_WL_EDIT_UNDO),"editor without undo: consumes the undo chord");
    expect(rbxWaylandPerformEditAction(editor,RBX_WL_EDIT_REDO),"editor without redo: consumes the redo chord");

    UndoableEditor *undoable=[[UndoableEditor new] autorelease];
    [undoable setEditable:YES];
    expect(rbxWaylandPerformEditAction(undoable,RBX_WL_EDIT_UNDO) && undo_calls==1,"native editor with undo: receives the undo action");
    expect(rbxWaylandPerformEditAction(undoable,RBX_WL_EDIT_REDO) && redo_calls==1,"native editor with redo: receives the redo action");

    paste_calls=0;cut_calls=0;undo_calls=0;redo_calls=0;
    [editor setEditable:NO];
    expect(rbxWaylandPerformEditAction(editor,RBX_WL_EDIT_PASTE) && paste_calls==0,"read-only editor consumes paste without inserting");
    expect(rbxWaylandPerformEditAction(editor,RBX_WL_EDIT_CUT) && cut_calls==0,"read-only editor consumes cut without deleting");
    expect(rbxWaylandPerformEditAction(editor,RBX_WL_EDIT_COPY),"copy still allowed in read-only editor");
    [undoable setEditable:NO];
    expect(rbxWaylandPerformEditAction(undoable,RBX_WL_EDIT_UNDO) && undo_calls==0,"read-only editor consumes undo without editing");
    expect(rbxWaylandPerformEditAction(undoable,RBX_WL_EDIT_REDO) && redo_calls==0,"read-only editor consumes redo without editing");

    expect(!rbxWaylandPerformEditAction(plain,RBX_WL_EDIT_COPY),"non-editor is not dispatched");
    expect(!rbxWaylandPerformEditAction(editor,RBX_WL_EDIT_NONE),"no action for unrelated keys");

    // The mapping itself must not hijack gameplay keys or shifted chords.
    expect(rbxWaylandEditAction(6,RBX_WL_MOD_CONTROL)==RBX_WL_EDIT_COPY,"C maps to copy");
    expect(rbxWaylandEditAction(25,RBX_WL_MOD_CONTROL)==RBX_WL_EDIT_PASTE,"V maps to paste");
    expect(rbxWaylandEditAction(27,RBX_WL_MOD_COMMAND)==RBX_WL_EDIT_CUT,"Cmd+X maps to cut");
    expect(rbxWaylandEditAction(4,RBX_WL_MOD_COMMAND)==RBX_WL_EDIT_SELECT_ALL,"Cmd+A maps to selectAll");
    expect(rbxWaylandEditAction(29,RBX_WL_MOD_CONTROL)==RBX_WL_EDIT_UNDO,"Ctrl+Z maps to undo");
    expect(rbxWaylandEditAction(29,RBX_WL_MOD_COMMAND)==RBX_WL_EDIT_UNDO,"Cmd+Z maps to undo");
    expect(rbxWaylandEditAction(28,RBX_WL_MOD_CONTROL)==RBX_WL_EDIT_REDO,"Ctrl+Y maps to redo");
    expect(rbxWaylandEditAction(29,RBX_WL_MOD_CONTROL|RBX_WL_MOD_SHIFT)==RBX_WL_EDIT_REDO,"Ctrl+Shift+Z maps to redo");
    expect(rbxWaylandEditAction(29,RBX_WL_MOD_COMMAND|RBX_WL_MOD_SHIFT)==RBX_WL_EDIT_REDO,"Cmd+Shift+Z maps to redo");
    expect(rbxWaylandEditAction(26,RBX_WL_MOD_CONTROL)==RBX_WL_EDIT_NONE,"Ctrl+W stays gameplay");
    expect(rbxWaylandEditAction(6,RBX_WL_MOD_CONTROL|RBX_WL_MOD_SHIFT)==RBX_WL_EDIT_NONE,"Ctrl+Shift+C stays gameplay");
    expect(rbxWaylandEditAction(27,RBX_WL_MOD_CONTROL|RBX_WL_MOD_SHIFT)==RBX_WL_EDIT_NONE,"Ctrl+Shift+X stays gameplay");
    expect(rbxWaylandEditAction(28,RBX_WL_MOD_CONTROL|RBX_WL_MOD_SHIFT)==RBX_WL_EDIT_NONE,"Ctrl+Shift+Y stays gameplay");
    expect(rbxWaylandEditAction(6,RBX_WL_MOD_CONTROL|RBX_WL_MOD_ALTERNATE)==RBX_WL_EDIT_NONE,"Ctrl+Alt+C stays gameplay");
    expect(rbxWaylandEditAction(29,RBX_WL_MOD_CONTROL|RBX_WL_MOD_ALTERNATE)==RBX_WL_EDIT_NONE,"Ctrl+Alt+Z stays gameplay");
    expect(rbxWaylandEditAction(6,0)==RBX_WL_EDIT_NONE,"unmodified C is typing");
    expect(rbxWaylandEditAction(29,0)==RBX_WL_EDIT_NONE,"unmodified Z is typing");

    // The engine expects macOS Command chords, not Linux Control chords.
    expect(rbxWaylandEngineModifiers(RBX_WL_EDIT_COPY,RBX_WL_MOD_CONTROL)==RBX_WL_MOD_COMMAND,"Ctrl+C copy reaches the Command engine path");
    expect(rbxWaylandEngineModifiers(RBX_WL_EDIT_COPY,RBX_WL_MOD_COMMAND)==RBX_WL_MOD_COMMAND,"Cmd+C keeps Command");
    expect(rbxWaylandEngineModifiers(RBX_WL_EDIT_CUT,RBX_WL_MOD_CONTROL)==RBX_WL_MOD_COMMAND,"Ctrl+X cut becomes Command+X");
    expect(rbxWaylandEngineModifiers(RBX_WL_EDIT_UNDO,RBX_WL_MOD_CONTROL)==RBX_WL_MOD_COMMAND,"Ctrl+Z undo becomes Command+Z");
    expect(rbxWaylandEngineModifiers(RBX_WL_EDIT_UNDO,RBX_WL_MOD_COMMAND)==RBX_WL_MOD_COMMAND,"Cmd+Z undo keeps Command");
    expect(rbxWaylandEngineModifiers(RBX_WL_EDIT_REDO,RBX_WL_MOD_CONTROL)==(RBX_WL_MOD_COMMAND|RBX_WL_MOD_SHIFT),"Ctrl+Y redo becomes Command+Shift+Z");
    expect(rbxWaylandEngineModifiers(RBX_WL_EDIT_REDO,RBX_WL_MOD_CONTROL|RBX_WL_MOD_SHIFT)==(RBX_WL_MOD_COMMAND|RBX_WL_MOD_SHIFT),"Ctrl+Shift+Z redo becomes Command+Shift+Z");
    expect(rbxWaylandEngineModifiers(RBX_WL_EDIT_REDO,RBX_WL_MOD_COMMAND|RBX_WL_MOD_SHIFT)==(RBX_WL_MOD_COMMAND|RBX_WL_MOD_SHIFT),"Cmd+Shift+Z redo keeps Command+Shift");
    expect(rbxWaylandEngineModifiers(RBX_WL_EDIT_CUT,RBX_WL_MOD_COMMAND|RBX_WL_MOD_SHIFT)==RBX_WL_MOD_COMMAND,"cut never keeps Shift");

    // Redo is Shift+Z on macOS even when the user pressed Ctrl+Y.
    expect(rbxWaylandEngineScancode(RBX_WL_EDIT_UNDO,29)==29,"undo uses the Z key");
    expect(rbxWaylandEngineScancode(RBX_WL_EDIT_REDO,28)==29,"Ctrl+Y redo uses the Z key");
    expect(rbxWaylandEngineScancode(RBX_WL_EDIT_REDO,29)==29,"Shift+Z redo uses the Z key");
    expect(rbxWaylandEngineScancode(RBX_WL_EDIT_CUT,27)==27,"cut keeps the X key");
    expect(rbxWaylandShortcutLetter(27)=='x' && rbxWaylandShortcutLetter(29)=='z' && rbxWaylandShortcutLetter(28)=='y',"shortcut letters are available for the engine event");

    [pool release];
    if(failures) {
        fprintf(stderr,"FAIL: %d of %d input checks failed\n",failures,checks);
        return 1;
    }
    printf("PASS Ctrl/Cmd editing-action dispatch, direct engine copy/select-all chord, empty-selection routing, read-only handling, gameplay-key isolation, and modifier guards\n");
    return 0;
}