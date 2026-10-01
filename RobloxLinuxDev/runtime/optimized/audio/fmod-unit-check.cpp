#include <CoreAudio/AudioHardware.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CFString.h>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>
#include <atomic>
#include <pthread.h>
#include <mach/mach.h>
#include <cassert>
#include <cstring>
static std::atomic<unsigned> callbacks{0};
static std::atomic<unsigned> interruption{0}, interruptedCalls{0};
static pthread_key_t callbackKey;
static OSStatus render(void*,AudioUnitRenderActionFlags*,const AudioTimeStamp*,UInt32,UInt32,AudioBufferList* b) {
 auto mode=interruption.load();
 if(mode) {
  ++interruptedCalls;
  if(mode==1)return kAudioUnitErr_NoConnection;
  b->mBuffers[0].mDataByteSize=mode==2?0:b->mBuffers[0].mDataByteSize+4;
  return noErr;
 }
 auto self=pthread_self();assert(self);
 assert(!pthread_setspecific(callbackKey,self)&&pthread_getspecific(callbackKey)==self);
 auto thread=mach_thread_self();assert(thread!=MACH_PORT_NULL);
 thread_basic_info_data_t info={};mach_msg_type_number_t count=THREAD_BASIC_INFO_COUNT;
 assert(thread_info(thread,THREAD_BASIC_INFO,reinterpret_cast<thread_info_t>(&info),&count)==KERN_SUCCESS);
 assert(mach_port_deallocate(mach_task_self(),thread)==KERN_SUCCESS);
 auto text=CFStringCreateWithCString(nullptr,"audio callback registration check",kCFStringEncodingUTF8);assert(text);CFRelease(text);
 for(UInt32 i=0;i<b->mNumberBuffers;i++)for(UInt32 j=0;j<b->mBuffers[i].mDataByteSize/4;j++)((float*)b->mBuffers[i].mData)[j]=.125f;
 ++callbacks;return 0;
}
#define CHECK(stage,call) do { auto s=(call);printf("AUDIO FMOD-unit %s status=%d\n",stage,int(s));if(s)return 1; } while(0)
int main(int argc,char** argv) {
 bool sustain=argc==2&&!std::strcmp(argv[1],"--sustain");
 assert(argc==1||sustain);
 setvbuf(stdout,nullptr,_IOLBF,0);
 assert(!pthread_key_create(&callbackKey,nullptr));
 // The harness supplies only a private server with synthetic monitor sources.
 CFStringRef uid=CFSTR("PulseAudio:sink:tracka_test_B");AudioDeviceID device=0;UInt32 size=4;
 AudioObjectPropertyAddress a={kAudioHardwarePropertyTranslateUIDToDevice,kAudioObjectPropertyScopeGlobal,0};
 CHECK("private device",AudioObjectGetPropertyData(1,&a,sizeof(uid),&uid,&size,&device));if(!device)return 2;
 void* lib=dlopen(getenv("AUDIO_COMPONENT"),RTLD_NOW|RTLD_LOCAL);if(!lib)return 3;
 auto factory=(AudioComponentFactoryFunction)dlsym(lib,"AUHALFactory");if(!factory)return 4;
 AudioComponentDescription desc={kAudioUnitType_Output,kAudioUnitSubType_HALOutput,'rbxt',0,0};
 auto component=AudioComponentRegister(&desc,CFSTR("Isolated FMOD HAL sequence"),1,factory);if(!component)return 5;
 AudioUnit u=nullptr;CHECK("instance",AudioComponentInstanceNew(component,&u));
 UInt32 on=1,off=0;
 CHECK("enable output",AudioUnitSetProperty(u,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Output,0,&on,4));
 CHECK("disable input",AudioUnitSetProperty(u,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Input,1,&off,4));
 CHECK("select device",AudioUnitSetProperty(u,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&device,4));
 AudioStreamBasicDescription hw={};size=sizeof(hw);
 CHECK("get hardware format",AudioUnitGetProperty(u,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,0,&hw,&size));
 AudioStreamBasicDescription format={48000,kAudioFormatLinearPCM,kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked,8,1,8,2,32,0};
 CHECK("set client format",AudioUnitSetProperty(u,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&format,sizeof(format)));
 AudioChannelLayout layout={};layout.mChannelLayoutTag=kAudioChannelLayoutTag_Stereo;
 CHECK("set client channel layout",AudioUnitSetProperty(u,kAudioUnitProperty_AudioChannelLayout,kAudioUnitScope_Input,0,&layout,sizeof(layout)));
 a={kAudioDevicePropertyPreferredChannelLayout,kAudioObjectPropertyScopeOutput,0};
 AudioChannelLayout reported={};size=sizeof(reported);
 CHECK("host channel layout",AudioObjectGetPropertyData(device,&a,0,nullptr,&size,&reported));
 if(reported.mChannelLayoutTag!=kAudioChannelLayoutTag_Stereo||size!=12)return 7;
 size=sizeof(reported);CHECK("unit layout readback",AudioUnitGetProperty(u,kAudioUnitProperty_AudioChannelLayout,kAudioUnitScope_Input,0,&reported,&size));
 if(reported.mChannelLayoutTag!=kAudioChannelLayoutTag_Stereo||size!=12)return 8;
 layout.mChannelLayoutTag=kAudioChannelLayoutTag_Mono;
 if(AudioUnitSetProperty(u,kAudioUnitProperty_AudioChannelLayout,kAudioUnitScope_Input,0,&layout,sizeof(layout))!=kAudioUnitErr_InvalidPropertyValue)return 9;
 auto mono=format;mono.mChannelsPerFrame=1;mono.mBytesPerFrame=mono.mBytesPerPacket=4;
 CHECK("mono format",AudioUnitSetProperty(u,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&mono,sizeof(mono)));
 CHECK("mono layout",AudioUnitSetProperty(u,kAudioUnitProperty_AudioChannelLayout,kAudioUnitScope_Input,0,&layout,sizeof(layout)));
 CHECK("restore stereo format",AudioUnitSetProperty(u,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&format,sizeof(format)));
 layout.mChannelLayoutTag=kAudioChannelLayoutTag_Stereo;layout.mNumberChannelDescriptions=1;
 if(AudioUnitSetProperty(u,kAudioUnitProperty_AudioChannelLayout,kAudioUnitScope_Input,0,&layout,sizeof(layout))!=kAudioUnitErr_InvalidPropertyValue)return 10;
 layout.mNumberChannelDescriptions=0;
 CHECK("restore stereo layout",AudioUnitSetProperty(u,kAudioUnitProperty_AudioChannelLayout,kAudioUnitScope_Input,0,&layout,sizeof(layout)));
 puts("PASS native layout readback, mono/stereo selection, incompatible and malformed layout rejection");
 AURenderCallbackStruct cb={render,nullptr};
 CHECK("render callback",AudioUnitSetProperty(u,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&cb,sizeof(cb)));
 CHECK("initialize",AudioUnitInitialize(u));
 for(int round=0;round<20;round++) {
  auto before=callbacks.load();
  CHECK("start",AudioOutputUnitStart(u));
  for(int i=0;i<150&&callbacks<before+3;i++)usleep(20000);
  if(callbacks<before+3)return 11;
  if(round==0)for(unsigned mode=1;mode<=3;++mode) {
   auto interrupted=interruptedCalls.load();interruption=mode;
   for(int i=0;i<150&&interruptedCalls==interrupted;i++)usleep(20000);
   if(interruptedCalls==interrupted)return 12;
   before=callbacks.load();interruption=0;
   for(int i=0;i<150&&callbacks<before+3;i++)usleep(20000);
   if(callbacks<before+3) {
    printf("AUDIO FAIL playback did not recover after callback interruption mode=%u\n",mode);
    return 13;
   }
   printf("PASS playback recovers after callback interruption mode=%u without restarting\n",mode);
  }
  if(round==0&&sustain)for(unsigned second=0;second<240;++second) {
   before=callbacks.load();sleep(1);
   if(callbacks==before) {printf("AUDIO FAIL sustained playback stopped at second=%u\n",second+1);return 14;}
   if((second+1)%30==0)printf("PASS sustained playback seconds=%u callbacks=%u\n",second+1,callbacks.load());
  }
  CHECK("stop",AudioOutputUnitStop(u));
 }
 CHECK("uninitialize",AudioUnitUninitialize(u));
 CHECK("dispose",AudioComponentInstanceDispose(u));
 if(!callbacks)return 6;
 assert(!pthread_key_delete(callbackKey));
 printf("PASS FMOD AudioUnit init sequence, 20 restarts and registered Darwin callbacks=%u (pthread TLS, Mach thread_info, CF allocation)\n",callbacks.load());
}
