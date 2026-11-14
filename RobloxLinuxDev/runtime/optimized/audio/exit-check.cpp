// Leaves the process through exit() while the sound-server connection is in one of three states:
//   running  the streams are still playing and recording (a client that quits without stopping)
//   polling  the streams were stopped, and four threads keep reading device properties
//   stopped  the streams were stopped just before (the server's answers are still on their way)
// A correct exit has status 0 and prints nothing after "AUDIO exit". Run by check-device-inventory.py.
#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CFString.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <thread>
static std::atomic<unsigned> outputCalls{0},inputCalls{0};
static OSStatus audio(AudioObjectID,const AudioTimeStamp*,const AudioBufferList* in,const AudioTimeStamp*,AudioBufferList* out,const AudioTimeStamp*,void*) {
 if(out){for(unsigned b=0;b<out->mNumberBuffers;b++)memset(out->mBuffers[b].mData,0,out->mBuffers[b].mDataByteSize);++outputCalls;}
 if(in)++inputCalls;
 return 0;
}
static AudioDeviceID lookup(const char* name) {
 CFStringRef uid=CFStringCreateWithCString(nullptr,name,kCFStringEncodingUTF8);AudioDeviceID id=0;UInt32 size=4;
 AudioObjectPropertyAddress a={kAudioHardwarePropertyTranslateUIDToDevice,kAudioObjectPropertyScopeGlobal,0};
 OSStatus status=AudioObjectGetPropertyData(1,&a,sizeof(uid),&uid,&size,&id);CFRelease(uid);
 return status?0:id;
}
int main(int argc,char** argv) {
 bool polling=argc>1&&!strcmp(argv[1],"polling"),running=argc>1&&!strcmp(argv[1],"running");
 UInt32 size=0;AudioObjectPropertyAddress devices={kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,0};
 AudioObjectGetPropertyDataSize(1,&devices,0,nullptr,&size);
 auto output=lookup("PulseAudio:sink:tracka_test_A"),input=lookup("PulseAudio:source:tracka_test_A.monitor");
 if(!output||!input){printf("AUDIO exit-check: devices not found (%u %u)\n",output,input);return 2;}
 AudioDeviceIOProcID out=nullptr,in=nullptr;
 if(AudioDeviceCreateIOProcID(output,audio,nullptr,&out)||AudioDeviceCreateIOProcID(input,audio,nullptr,&in)||AudioDeviceStart(output,out)||AudioDeviceStart(input,in)){puts("AUDIO exit-check: start failed");return 2;}
 for(int i=0;i<100&&(!outputCalls||!inputCalls);i++)usleep(50000);
 if(!outputCalls||!inputCalls){printf("AUDIO exit-check: no callbacks (%u %u)\n",outputCalls.load(),inputCalls.load());return 2;}
 if(!running&&(AudioDeviceStop(input,in)||AudioDeviceStop(output,out))){puts("AUDIO exit-check: stop failed");return 2;}
 if(polling) {
  for(int t=0;t<4;t++)std::thread([=]{
   for(;;) {
    AudioDeviceID ids[64];UInt32 n=sizeof(ids),alive=0,four=4;
    AudioObjectPropertyAddress all={kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,0},live={kAudioDevicePropertyDeviceIsAlive,kAudioObjectPropertyScopeGlobal,0};
    AudioObjectGetPropertyData(1,&all,0,nullptr,&n,ids);
    AudioObjectGetPropertyData(output,&live,0,nullptr,&four,&alive);
   }
  }).detach();
  usleep(100000);
 }
 printf("AUDIO exit %s\n",argc>1?argv[1]:"stopped");fflush(stdout);
 return 0;
}
