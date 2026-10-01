#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CFString.h>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#include <vector>

static std::atomic<float> level{0};
static OSStatus playback(AudioObjectID, const AudioTimeStamp*, const AudioBufferList*,
                         const AudioTimeStamp*, AudioBufferList* output,
                         const AudioTimeStamp*, void*) {
    for (UInt32 b = 0; b < output->mNumberBuffers; ++b) {
        auto* samples = static_cast<float*>(output->mBuffers[b].mData);
        for (UInt32 i = 0; i < output->mBuffers[b].mDataByteSize / sizeof(float); ++i)
            samples[i] = level.load();
    }
    return noErr;
}

struct Capture {
    AudioUnit unit;
    AudioStreamBasicDescription format;
    std::atomic<unsigned> matching{0}, calls{0};
    UInt32 maxFrames = 0;
    double nextSample = 0;
    UInt64 lastHost = 0;
};
static OSStatus recorded(void* context, AudioUnitRenderActionFlags* flags,
                         const AudioTimeStamp* timestamp, UInt32 bus, UInt32 frames,
                         AudioBufferList*) {
    auto& capture = *static_cast<Capture*>(context);
    if (frames > capture.maxFrames)
        fprintf(stderr, "AUDIO oversized capture callback frames=%u maximum=%u\n", frames, capture.maxFrames);
    assert(bus == 1 && frames > 0 && frames <= capture.maxFrames);
    assert((timestamp->mFlags & kAudioTimeStampSampleHostTimeValid) == kAudioTimeStampSampleHostTimeValid);
    assert(timestamp->mHostTime > capture.lastHost);
    if (capture.calls) assert(timestamp->mSampleTime == capture.nextSample);
    capture.lastHost = timestamp->mHostTime;
    capture.nextSample = timestamp->mSampleTime + frames;
    std::vector<unsigned char> samples(frames * capture.format.mBytesPerFrame, 0xa5);
    AudioBufferList buffers = {1, {{capture.format.mChannelsPerFrame,
                                  UInt32(samples.size()), samples.data()}}};
    assert(!AudioUnitRender(capture.unit, flags, timestamp, bus, frames, &buffers));
    assert(buffers.mBuffers[0].mDataByteSize == samples.size());
    unsigned matches = 0, count = frames * capture.format.mChannelsPerFrame;
    float expected = level.load();
    for (unsigned i = 0; i < count; ++i) {
        float value = capture.format.mBitsPerChannel == 32
            ? reinterpret_cast<const float*>(buffers.mBuffers[0].mData)[i]
            : reinterpret_cast<const short*>(buffers.mBuffers[0].mData)[i] / 32768.0f;
        assert(std::isfinite(value));
        if (std::fabs(value - expected) < 0.002f) ++matches;
    }
    if (matches == count) ++capture.matching;
    ++capture.calls;
    return noErr;
}

static AudioDeviceID device(CFStringRef uid) {
    AudioDeviceID result = 0;
    UInt32 size = sizeof(result);
    AudioObjectPropertyAddress property = {kAudioHardwarePropertyTranslateUIDToDevice,
                                          kAudioObjectPropertyScopeGlobal, 0};
    assert(!AudioObjectGetPropertyData(kAudioObjectSystemObject, &property,
                                      sizeof(uid), &uid, &size, &result));
    assert(result);
    return result;
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    // These endpoints exist only on the harness's private server: no microphone.
    auto input = device(CFSTR("PulseAudio:source:tracka_capture.monitor"));
    auto output = device(CFSTR("PulseAudio:sink:tracka_capture"));
    auto* library = dlopen(getenv("AUDIO_COMPONENT"), RTLD_NOW | RTLD_LOCAL);
    if (!library) fprintf(stderr, "%s\n", dlerror());
    assert(library);
    auto factory = reinterpret_cast<AudioComponentFactoryFunction>(dlsym(library, "AUHALFactory"));
    assert(factory);
    AudioComponentDescription description = {kAudioUnitType_Output, kAudioUnitSubType_HALOutput, 'rbxt', 0, 0};
    auto component = AudioComponentRegister(&description, CFSTR("Synthetic microphone regression"), 1, factory);
    assert(component);
    AudioStreamBasicDescription stereo = {48000, kAudioFormatLinearPCM,
        kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0};
    AudioObjectPropertyAddress property = {kAudioDevicePropertyStreamFormat, kAudioObjectPropertyScopeOutput, 0};
    assert(!AudioObjectSetPropertyData(output, &property, 0, nullptr, sizeof(stereo), &stereo));
    AudioDeviceIOProcID writer = nullptr;
    assert(!AudioDeviceCreateIOProcID(output, playback, nullptr, &writer));
    assert(!AudioDeviceStart(output, writer));

    for (bool mono : {false, true}) {
        Capture capture{};
        capture.format = stereo;
        if (mono) {
            capture.format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
            capture.format.mChannelsPerFrame = 1;
            capture.format.mBitsPerChannel = 16;
            capture.format.mBytesPerFrame = capture.format.mBytesPerPacket = 2;
        }
        assert(!AudioComponentInstanceNew(component, &capture.unit));
        UInt32 off = 0, on = 1;
        assert(!AudioUnitSetProperty(capture.unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &off, sizeof(off)));
        assert(!AudioUnitSetProperty(capture.unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &on, sizeof(on)));
        assert(!AudioUnitSetProperty(capture.unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &input, sizeof(input)));
        AudioStreamBasicDescription hardware{};
        UInt32 size = sizeof(hardware);
        auto status = AudioUnitGetProperty(capture.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &hardware, &size);
        printf("AUDIO capture hardware input bus 1 status=%d channels=%u rate=%.0f\n", status, hardware.mChannelsPerFrame, hardware.mSampleRate);
        assert(!status && size == sizeof(hardware) && hardware.mChannelsPerFrame && hardware.mSampleRate > 0);
        auto inputProperty = property;
        inputProperty.mScope = kAudioObjectPropertyScopeInput;
        AudioStreamBasicDescription reported{};
        size = sizeof(reported);
        assert(!AudioObjectGetPropertyData(input, &inputProperty, 0, nullptr, &size, &reported));
        assert(!std::memcmp(&hardware, &reported, sizeof(hardware)));
        size = sizeof(capture.maxFrames);
        assert(!AudioUnitGetProperty(capture.unit, kAudioUnitProperty_MaximumFramesPerSlice,
                                    kAudioUnitScope_Global, 0, &capture.maxFrames, &size));
        assert(!AudioUnitSetProperty(capture.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &capture.format, sizeof(capture.format)));
        AURenderCallbackStruct callback = {recorded, &capture};
        assert(!AudioUnitSetProperty(capture.unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &callback, sizeof(callback)));
        assert(!AudioUnitInitialize(capture.unit));
        for (unsigned round = 0; round < 3; ++round) {
            level = 0;
            capture.calls = 0;
            capture.lastHost = 0;
            assert(!AudioOutputUnitStart(capture.unit));
            for (float value : {0.0f, 0.125f, -0.25f}) {
                level = value;
                capture.matching = 0;
                for (unsigned i = 0; i < 300 && capture.matching < 3; ++i) usleep(20000);
                printf("AUDIO capture %s round=%u expected=%.3f matching=%u calls=%u\n",
                       mono ? "mono S16" : "stereo float", round, value,
                       capture.matching.load(), capture.calls.load());
                assert(capture.matching >= 3);
            }
            assert(!AudioOutputUnitStop(capture.unit));
            printf("PASS %s capture round=%u: silence, positive and negative PCM, advancing timestamps\n",
                   mono ? "mono S16" : "stereo float", round);
        }
        assert(!AudioUnitUninitialize(capture.unit));
        assert(!AudioComponentInstanceDispose(capture.unit));
    }
    assert(!AudioDeviceStop(output, writer));
    assert(!AudioDeviceDestroyIOProcID(output, writer));
    return 0;
}
