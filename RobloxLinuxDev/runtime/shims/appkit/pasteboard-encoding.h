#ifndef RBX_PASTEBOARD_ENCODING_H
#define RBX_PASTEBOARD_ENCODING_H

#include <string.h>

// Every text format the Wayland pasteboard advertises has one encoding. The
// same table is used by dataForType: and setData:forType: so a value written
// through one method is decoded by the other with matching rules.
//
// Cocotron's NSUnicodeStringEncoding encoder emits native-endian UTF-16 while
// its decoder treats those bytes as big-endian (verified under Darling), so
// NSStringPboardType is explicitly little-endian on this x86_64 runtime.
enum RbxPasteboardTextEncoding {
    RBX_PASTEBOARD_NOT_TEXT = 0,
    RBX_PASTEBOARD_UTF16LE,
    RBX_PASTEBOARD_UTF8
};

static inline int rbxPasteboardEncodingForType(const char *type) {
    if (!type) return RBX_PASTEBOARD_NOT_TEXT;
    if (!strcmp(type, "NSStringPboardType")) return RBX_PASTEBOARD_UTF16LE;
    if (!strcmp(type, "public.utf8-plain-text")) return RBX_PASTEBOARD_UTF8;
    return RBX_PASTEBOARD_NOT_TEXT;
}

#endif