#include <Metal/MTLTextureInternal.h>
#include <Metal/MTLTypesInternal.h>
#include <indium/texture-readback.hpp>
#include <objc/runtime.h>

static void getBytes(id self, SEL, void* bytes, NSUInteger row, MTLRegion region, NSUInteger level) {
    Indium::readTextureBytes([(MTLTextureInternal*)self texture], bytes, row, 0, MTLRegionToIndium(region), level, 0);
}
static void getBytesSlice(id self, SEL, void* bytes, NSUInteger row, NSUInteger image, MTLRegion region, NSUInteger level, NSUInteger slice) {
    Indium::readTextureBytes([(MTLTextureInternal*)self texture], bytes, row, image, MTLRegionToIndium(region), level, slice);
}
// Called by the existing Track B drawable initializer hook. Avoid a new link
// dependency on Metal.framework (which itself depends on libindium).
extern "C" int trackb_install_texture_readback() {
    Class cls = objc_getClass("MTLTextureInternal");
    Method basic = class_getInstanceMethod(cls, sel_registerName("getBytes:bytesPerRow:fromRegion:mipmapLevel:"));
    Method slice = class_getInstanceMethod(cls, sel_registerName("getBytes:bytesPerRow:bytesPerImage:fromRegion:mipmapLevel:slice:"));
    if (!basic || !slice) return 0;
    method_setImplementation(basic, reinterpret_cast<IMP>(&getBytes));
    method_setImplementation(slice, reinterpret_cast<IMP>(&getBytesSlice));
    return 1;
}
