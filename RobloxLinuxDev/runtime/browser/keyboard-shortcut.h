#ifndef RBX_KEYBOARD_SHORTCUT_H
#define RBX_KEYBOARD_SHORTCUT_H

// Editing shortcuts are identified from the physical SDL scancode and the
// Cocoa modifier flags carried by RbxWaylandEvent. Keeping this mapping free
// of AppKit lets the native regression harness exercise it directly.
enum RbxWaylandEditAction {
    RBX_WL_EDIT_NONE = 0,
    RBX_WL_EDIT_COPY,
    RBX_WL_EDIT_PASTE,
    RBX_WL_EDIT_CUT,
    RBX_WL_EDIT_SELECT_ALL,
    RBX_WL_EDIT_UNDO,
    RBX_WL_EDIT_REDO
};

enum {
    RBX_WL_MOD_SHIFT = 1u << 17,
    RBX_WL_MOD_CONTROL = 1u << 18,
    RBX_WL_MOD_ALTERNATE = 1u << 19,
    RBX_WL_MOD_COMMAND = 1u << 20
};

static inline int rbxWaylandEditAction(unsigned scan, unsigned modifiers) {
    unsigned command = modifiers & RBX_WL_MOD_COMMAND;
    unsigned control = modifiers & RBX_WL_MOD_CONTROL;
    // Alt combinations stay with the input method or the game. A
    // command+control chord is not a standard text-editing shortcut either.
    if (modifiers & RBX_WL_MOD_ALTERNATE) return RBX_WL_EDIT_NONE;
    if (!command && !control) return RBX_WL_EDIT_NONE;
    if (command && control) return RBX_WL_EDIT_NONE;
    // Shift is only meaningful for redo. Every other shifted chord, including
    // Ctrl+Shift+C, stays with the game or the input method.
    if (modifiers & RBX_WL_MOD_SHIFT)
        return scan == 29 ? RBX_WL_EDIT_REDO : RBX_WL_EDIT_NONE; // SDL_SCANCODE_Z
    switch (scan) {
        case 6: return RBX_WL_EDIT_COPY;       // SDL_SCANCODE_C
        case 25: return RBX_WL_EDIT_PASTE;     // SDL_SCANCODE_V
        case 27: return RBX_WL_EDIT_CUT;       // SDL_SCANCODE_X
        case 4: return RBX_WL_EDIT_SELECT_ALL; // SDL_SCANCODE_A
        case 29: return RBX_WL_EDIT_UNDO;      // SDL_SCANCODE_Z
        case 28: return RBX_WL_EDIT_REDO;      // SDL_SCANCODE_Y
        default: return RBX_WL_EDIT_NONE;
    }
}

// The unmodified character Cocoa needs for charactersIgnoringModifiers.
static inline char rbxWaylandShortcutLetter(unsigned scan) {
    switch (scan) {
        case 6: return 'c';
        case 25: return 'v';
        case 27: return 'x';
        case 4: return 'a';
        case 28: return 'y';
        case 29: return 'z';
        default: return 0;
    }
}

// Undo and redo are Command+Z / Command+Shift+Z in macOS applications. A
// Linux-style Ctrl+Y redo therefore has to reach the engine as Shift+Z, not Y.
//
// Verified 2026-10-20 against the 0.743 Player client: Ctrl+X copies and
// deletes, but neither Command+Z nor Command+Shift+Z changes the text, even
// with FFlag/DFFlag TextBoxUndo, TextBoxRedo and FInt/DFInt
// UndoRedoTextBoxStackSize forced on through ClientAppSettings.json. The
// shipped engine has no working TextBox undo stack, so these two chords are
// consumed without effect. Keep the mapping: a client that implements undo
// picks it up, and consuming the chord stops 'z' being typed into the field.
static inline unsigned rbxWaylandEngineScancode(int action, unsigned scan) {
    if (action == RBX_WL_EDIT_UNDO || action == RBX_WL_EDIT_REDO) return 29; // SDL_SCANCODE_Z
    return scan;
}

// The engine's editing handlers are the Command chords. A Control-only chord
// must reach them as Command, with Shift kept only for redo.
static inline unsigned rbxWaylandEngineModifiers(int action, unsigned modifiers) {
    unsigned flags = modifiers & ~(RBX_WL_MOD_CONTROL | RBX_WL_MOD_ALTERNATE);
    if (!(modifiers & RBX_WL_MOD_COMMAND)) flags |= RBX_WL_MOD_COMMAND;
    if (action == RBX_WL_EDIT_REDO) flags |= RBX_WL_MOD_SHIFT;
    else flags &= ~RBX_WL_MOD_SHIFT;
    return flags;
}

#endif