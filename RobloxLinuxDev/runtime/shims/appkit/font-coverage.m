// Missing coverage makes AppKit try every installed font for ordinary text.
#import <Foundation/NSCharacterSet.h>
#import <Foundation/NSRange.h>
#import <objc/runtime.h>

@interface O2Font_freetype : NSObject
- (void *)face;
@end
extern unsigned long FT_Get_First_Char(void *, unsigned *);
extern unsigned long FT_Get_Next_Char(void *, unsigned long, unsigned *);
extern void O2EncodingGetMacRomanUnicode(unichar *);

@implementation O2Font_freetype (RbxFontCoverage)
- (NSCharacterSet *)coveredCharacterSet {
    @synchronized(self) {
        Ivar cache = class_getInstanceVariable(objc_getClass("O2Font"), "_coveredCharSet");
        NSCharacterSet *covered = object_getIvar(self, cache);
        if (covered) return covered;
        // Darling's inherited -init produces an immutable set even for the
        // mutable class; mutableCopy supplies a real mutable CFCharacterSet.
        NSMutableCharacterSet *characters = [[NSCharacterSet characterSetWithRange:NSMakeRange(0, 0)] mutableCopy];
        Ivar encodingIvar = class_getInstanceVariable(objc_getClass("O2Font_freetype"), "_ftEncoding");
        unsigned encoding = *(unsigned *)((char *)self + ivar_getOffset(encodingIvar));
        const unsigned unicode = 0x756e6963, roman = 0x61726d6e; // FreeType tags
        unichar macRoman[256];
        if (encoding == roman) O2EncodingGetMacRomanUnicode(macRoman);
        // Other legacy cmaps do not describe Unicode scalar coverage.
        if (encoding == unicode || encoding == roman) {
            void *face = [self face];
            unsigned glyph;
            NSRange run = NSMakeRange(0, 0);
            for (unsigned long code = FT_Get_First_Char(face, &glyph); glyph;
                 code = FT_Get_Next_Char(face, code, &glyph)) {
                if (encoding == roman && code > 255) continue;
                unsigned long scalar = encoding == roman ? macRoman[code] : code;
                if (scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) continue;
                if (run.length && scalar == NSMaxRange(run)) ++run.length;
                else {
                    if (run.length) [characters addCharactersInRange:run];
                    run = NSMakeRange(scalar, 1);
                }
            }
            if (run.length) [characters addCharactersInRange:run];
        }
        covered = [characters copy];
        [characters release];
        object_setIvar(self, cache, covered); // O2Font's dealloc releases this ivar.
        return covered;
    }
}
@end
