#include <AudioToolbox/AudioComponent.h>
#include <AudioToolbox/AUComponent.h>
#include <AudioToolbox/AudioOutputUnit.h>
#include <CoreFoundation/CFString.h>
#include <CarbonCore/MacErrors.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// No audio hardware: reproduce FMOD's device-change teardown using a plugin
// whose Close releases its interface, then verify a fresh instance still works.
static unsigned opens, closes, starts, stops, initializes, uninitializes;
struct Plugin { AudioComponentPlugInInterface interface; };
static OSStatus open(void*, AudioComponentInstance) { ++opens; return noErr; }
static OSStatus close(void* self) { ++closes; delete static_cast<Plugin*>(self); return noErr; }
static OSStatus start(void*, ...) { ++starts; return noErr; }
static OSStatus stop(void*, ...) { ++stops; return noErr; }
static OSStatus initialize(void*, ...) { ++initializes; return noErr; }
static OSStatus uninitialize(void*, ...) { ++uninitializes; return noErr; }
static AudioComponentMethod lookup(SInt16 selector) {
    switch (selector) {
        case kAudioOutputUnitStartSelect: return start;
        case kAudioOutputUnitStopSelect: return stop;
        case kAudioUnitInitializeSelect: return initialize;
        case kAudioUnitUninitializeSelect: return uninitialize;
        default: return nullptr;
    }
}
static AudioComponentPlugInInterface* factory(const AudioComponentDescription*) {
    return &((new Plugin{{open, close, lookup, nullptr}})->interface);
}
static void expect(const char* operation, OSStatus actual, OSStatus expected = noErr) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL %s: status=%d expected=%d\n", operation, int(actual), int(expected));
        std::exit(1);
    }
}
int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    AudioComponentDescription description = {kAudioUnitType_Output, 'life', 'rbxt', 0, 0};
    auto component = AudioComponentRegister(&description, CFSTR("Audio component lifecycle check"), 1, factory);
    if (!component) return 1;
    AudioUnit previous = nullptr;
    for (unsigned round = 0; round < 32; ++round) {
        AudioUnit unit = nullptr;
        expect("create", AudioComponentInstanceNew(component, &unit));
        if (!unit || unit == previous) return 2;
        expect("initialize", AudioUnitInitialize(unit));
        expect("start", AudioOutputUnitStart(unit));
        expect("unsupported live selector", AudioUnitReset(unit, 0, 0), badComponentSelector);
        expect("stop", AudioOutputUnitStop(unit));
        expect("repeat live stop", AudioOutputUnitStop(unit));
        expect("uninitialize", AudioUnitUninitialize(unit));
        expect("dispose", AudioComponentInstanceDispose(unit));
        // Exact observed ordering: Dispose completes, then the same thread
        // calls Stop/Uninitialize/Dispose again on the same tagged handle.
        expect("stop disposed unit", AudioOutputUnitStop(unit), badComponentInstance);
        expect("uninitialize disposed unit", AudioUnitUninitialize(unit), badComponentInstance);
        expect("dispose again", AudioComponentInstanceDispose(unit), badComponentInstance);
        UInt32 size = 0;
        expect("query disposed unit", AudioUnitGetProperty(unit, 0, 0, 0, nullptr, &size), badComponentInstance);
        expect("set disposed unit", AudioUnitSetProperty(unit, 0, 0, 0, &size, sizeof(size)), badComponentInstance);
        previous = unit;
    }
    auto unknown = reinterpret_cast<AudioUnit>(uintptr_t(0x8fffffff));
    expect("unknown tagged unit", AudioOutputUnitStop(unknown), badComponentInstance);
    if (opens != 32 || closes != 32 || starts != 32 || stops != 64 ||
        initializes != 32 || uninitializes != 32) return 3;
    puts("PASS disposed/unknown units return badComponentInstance without invoking freed plugins");
    puts("PASS repeated teardown, live unsupported selector and 32 successful replacement instances");
}
