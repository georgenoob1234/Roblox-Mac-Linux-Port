#ifndef RBX_KEYBOARD_MODIFIER_STATE_H
#define RBX_KEYBOARD_MODIFIER_STATE_H

// Reconciliation rules for the macOS modifier flags carried by
// RbxWaylandEvent. Wayland is authoritative: a compositor shortcut (Super
// opening the KWin overview, Super+Tab, a Ctrl chord grabbed by the desktop)
// can take keyboard focus before the modifier's release reaches this client.
// SDL's modifier mask then stays latched and the guest keeps receiving
// Command/Control on every later key, so a plain game key such as 1 turns
// into Roblox's Command+1 Capture chord.
//
// Keep this file free of SDL, Wayland and AppKit headers: the native harness
// compiled on the host and the AppKit harness compiled in the guest both
// exercise the same rules.
#include "keyboard-shortcut.h"

// Linux evdev keycodes (linux/input-event-codes.h). wl_keyboard.key and the
// keys array in wl_keyboard.enter use these, not SDL scancodes.
enum {
    RBX_EVDEV_KEY_LEFTCTRL = 29,
    RBX_EVDEV_KEY_RIGHTCTRL = 97,
    RBX_EVDEV_KEY_LEFTSHIFT = 42,
    RBX_EVDEV_KEY_RIGHTSHIFT = 54,
    RBX_EVDEV_KEY_LEFTALT = 56,
    RBX_EVDEV_KEY_RIGHTALT = 100,
    RBX_EVDEV_KEY_LEFTMETA = 125,
    RBX_EVDEV_KEY_RIGHTMETA = 126
};

// SDL scancodes 224..231 (SDL_SCANCODE_LCTRL .. SDL_SCANCODE_RGUI) are USB HID
// usages, not evdev codes, so they need their own mapping.
static inline unsigned rbxWaylandModifierBitForKey(unsigned key) {
    switch (key) {
        case RBX_EVDEV_KEY_LEFTCTRL:
        case RBX_EVDEV_KEY_RIGHTCTRL: return RBX_WL_MOD_CONTROL;
        case RBX_EVDEV_KEY_LEFTSHIFT:
        case RBX_EVDEV_KEY_RIGHTSHIFT: return RBX_WL_MOD_SHIFT;
        case RBX_EVDEV_KEY_LEFTALT:
        case RBX_EVDEV_KEY_RIGHTALT: return RBX_WL_MOD_ALTERNATE;
        case RBX_EVDEV_KEY_LEFTMETA:
        case RBX_EVDEV_KEY_RIGHTMETA: return RBX_WL_MOD_COMMAND;
        default: return 0;
    }
}

static inline unsigned rbxWaylandModifierBitForScancode(unsigned scan) {
    switch (scan) {
        case 224: case 228: return RBX_WL_MOD_CONTROL;   // SDL_SCANCODE_LCTRL/RCTRL
        case 225: case 229: return RBX_WL_MOD_SHIFT;     // SDL_SCANCODE_LSHIFT/RSHIFT
        case 226: case 230: return RBX_WL_MOD_ALTERNATE; // SDL_SCANCODE_LALT/RALT
        case 227: case 231: return RBX_WL_MOD_COMMAND;   // SDL_SCANCODE_LGUI/RGUI
        default: return 0;
    }
}

// xkb modifier indices as used by wl_keyboard.modifiers: Shift, Control,
// Mod1 (Alt), Mod4 (Super) and Mod5 (ISO_Level3_Shift, i.e. AltGr on most
// non-US layouts). Lock (Caps) is a lock, not a held modifier.
static inline unsigned rbxWaylandModifiersFromDepressed(unsigned depressed) {
    unsigned bits = 0;
    if (depressed & (1u << 0)) bits |= RBX_WL_MOD_SHIFT;
    if (depressed & (1u << 2)) bits |= RBX_WL_MOD_CONTROL;
    if (depressed & ((1u << 3) | (1u << 7))) bits |= RBX_WL_MOD_ALTERNATE;
    if (depressed & (1u << 6)) bits |= RBX_WL_MOD_COMMAND;
    return bits;
}

// Drop reported modifiers that the compositor has not confirmed as depressed.
// `physical` is the RBX_WL_MOD_* set, not the raw xkb mask. Caps Lock survives
// because it is a lock state, not a held key.
static inline unsigned rbxWaylandReconcileModifiers(unsigned reported, unsigned physical, int known) {
    if (!known) return reported;
    return (reported & RBX_WL_MOD_CAPS) | (reported & physical);
}

// Modifier bits that were reported before an event but are gone from it: the
// guest still needs an explicit release for each one.
static inline unsigned rbxWaylandDroppedModifiers(unsigned previous, unsigned current) {
    return previous & ~current &
        (RBX_WL_MOD_SHIFT | RBX_WL_MOD_CONTROL | RBX_WL_MOD_ALTERNATE | RBX_WL_MOD_COMMAND);
}

// Key-path bookkeeping for the AppKit shim. `engineFlags` holds the modifier
// flags the engine last saw on a *key* event; mouse events must not touch it.
// Returns the modifiers to release before a non-modifier key, then records
// `current` as what the engine now believes.
static inline unsigned rbxWaylandKeyEventDropped(unsigned *engineFlags, unsigned current, int isModifierKey) {
    unsigned dropped = isModifierKey ? 0 : rbxWaylandDroppedModifiers(*engineFlags, current);
    *engineFlags = current;
    return dropped;
}

#endif