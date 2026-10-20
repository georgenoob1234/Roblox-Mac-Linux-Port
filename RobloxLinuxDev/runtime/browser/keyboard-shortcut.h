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
    RBX_WL_EDIT_SELECT_ALL
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
    // Shift/Alt combinations stay with the input method or the game. A
    // command+control chord is not a standard text-editing shortcut either.
    if (modifiers & (RBX_WL_MOD_SHIFT | RBX_WL_MOD_ALTERNATE)) return RBX_WL_EDIT_NONE;
    if (!command && !control) return RBX_WL_EDIT_NONE;
    if (command && control) return RBX_WL_EDIT_NONE;
    switch (scan) {
        case 6: return RBX_WL_EDIT_COPY;       // SDL_SCANCODE_C
        case 25: return RBX_WL_EDIT_PASTE;     // SDL_SCANCODE_V
        case 27: return RBX_WL_EDIT_CUT;       // SDL_SCANCODE_X
        case 4: return RBX_WL_EDIT_SELECT_ALL; // SDL_SCANCODE_A
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
        default: return 0;
    }
}

#endif