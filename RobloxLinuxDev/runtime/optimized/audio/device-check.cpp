#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFRunLoop.h>
#include <cstdio>
#include <vector>
#include <cassert>
#include <atomic>
#include <unistd.h>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <dlfcn.h>
#include <AudioToolbox/AudioToolbox.h>
#include <thread>
#include <signal.h>
#include <sys/ucontext.h>
static std::atomic<unsigned> outputCalls{0}, inputCalls{0}, signalCalls{0}, changes{0};
static OSStatus changed(AudioObjectID,UInt32,const AudioObjectPropertyAddress*,void*){++changes;return 0;}
static OSStatus audio(AudioObjectID,const AudioTimeStamp*,const AudioBufferList* in,const AudioTimeStamp*,AudioBufferList* out,const AudioTimeStamp*,void*) {
 if(out){for(unsigned b=0;b<out->mNumberBuffers;b++){auto* f=(float*)out->mBuffers[b].mData;for(unsigned i=0;i<out->mBuffers[b].mDataByteSize/4;i++)f[i]=.125f;}++outputCalls;}
 if(in){bool signal=false;for(unsigned b=0;b<in->mNumberBuffers;b++){auto* f=(const float*)in->mBuffers[b].mData;for(unsigned i=0;i<in->mBuffers[b].mDataByteSize/4;i++){assert(std::isfinite(f[i]));if(fabs(f[i]-.125f)<.01f)signal=true;}}++inputCalls;if(signal)++signalCalls;}
 return 0;
}
static AudioDeviceID lookup(const char* name) {
 CFStringRef uid=CFStringCreateWithCString(nullptr,name,kCFStringEncodingUTF8);AudioDeviceID id=0;UInt32 size=4;
 AudioObjectPropertyAddress a={kAudioHardwarePropertyTranslateUIDToDevice,kAudioObjectPropertyScopeGlobal,0};
 assert(!AudioObjectGetPropertyData(1,&a,sizeof(uid),&uid,&size,&id));CFRelease(uid);assert(id);return id;
}
static void signalFile(const char* path) {FILE* f=fopen(path,"w");assert(f);fputs("ready",f);fclose(f);}
static OSStatus render(void*,AudioUnitRenderActionFlags*,const AudioTimeStamp*,UInt32,UInt32 frames,AudioBufferList* out) {
 assert(frames>0);return audio(0,nullptr,nullptr,nullptr,out,nullptr,nullptr);
}
static OSStatus recorded(void*,AudioUnitRenderActionFlags*,const AudioTimeStamp*,UInt32,UInt32 frames,AudioBufferList*) {assert(frames);++inputCalls;return 0;}
static void waitFile(const char* name) {for(int i=0;i<200&&access(name,F_OK);i++)usleep(100000);assert(!access(name,F_OK));}
static AudioUnit unit(const char* factory,UInt32 subtype) {
 void* lib=dlopen(getenv("AUDIO_COMPONENT"),RTLD_NOW|RTLD_LOCAL);if(!lib)printf("AUDIO dlopen %s\n",dlerror());assert(lib);
 auto fn=(AudioComponentFactoryFunction)dlsym(lib,factory);assert(fn);
 AudioComponentDescription d={kAudioUnitType_Output,subtype,'rbxt',0,0};
 auto c=AudioComponentRegister(&d,CFSTR("Track A synthetic audio check"),1,fn);assert(c);
 AudioUnit u=nullptr;assert(!AudioComponentInstanceNew(c,&u));return u;
}
static void checkUnits(AudioDeviceID input,AudioDeviceID output,const AudioStreamBasicDescription& format) {
 AudioUnit u=unit("AUHALFactory",'tahl');UInt32 zero=0,one=1,n=4;AudioDeviceID selected=0;
 assert(!AudioUnitSetProperty(u,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Output,0,&zero,4));
 assert(!AudioUnitSetProperty(u,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Input,1,&one,4));
 assert(!AudioUnitSetProperty(u,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&input,4));
 assert(!AudioUnitGetProperty(u,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&selected,&n));assert(selected==input);
 AURenderCallbackStruct cb={recorded,nullptr};
 assert(!AudioUnitSetProperty(u,kAudioOutputUnitProperty_SetInputCallback,kAudioUnitScope_Global,0,&cb,sizeof(cb)));
 assert(!AudioUnitSetProperty(u,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,1,&format,sizeof(format)));
 assert(!AudioUnitInitialize(u));inputCalls=0;assert(!AudioOutputUnitStart(u));
 signalFile("unit-input-ready");waitFile("unit-input-checked");
 assert(inputCalls);assert(!AudioOutputUnitStop(u));assert(!AudioUnitUninitialize(u));assert(!AudioComponentInstanceDispose(u));
 printf("PASS AudioUnit input selection through global element 0\n");
 u=unit("DefaultOutputAUFactory",'tdft');cb={render,nullptr};
 assert(!AudioUnitSetProperty(u,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&cb,sizeof(cb)));
 assert(!AudioUnitSetProperty(u,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&format,sizeof(format)));
 assert(!AudioUnitInitialize(u));outputCalls=0;assert(!AudioOutputUnitStart(u));
 signalFile("unit-default-ready");waitFile("unit-default-checked");
 assert(outputCalls);assert(!AudioOutputUnitStop(u));assert(!AudioOutputUnitStart(u));usleep(150000);assert(!AudioOutputUnitStop(u));
 assert(!AudioUnitUninitialize(u));assert(!AudioComponentInstanceDispose(u));printf("PASS default AudioUnit output and restart\n");
}
int main(int argc,char** argv) {
 setvbuf(stdout,nullptr,_IOLBF,0);
 struct sigaction action={};action.sa_flags=SA_SIGINFO;action.sa_sigaction=[](int sig,siginfo_t* info,void* ctx){
  auto mc=((ucontext_t*)ctx)->uc_mcontext;Dl_info d={};dladdr((void*)mc->__ss.__rip,&d);
  fprintf(stderr,"AUDIO SIGNAL %d pc=%p %s+%llx fault=%p\n",sig,(void*)mc->__ss.__rip,d.dli_fname?d.dli_fname:"?",mc->__ss.__rip-(unsigned long long)d.dli_fbase,info->si_addr);
  auto* fp=(unsigned long long*)mc->__ss.__rbp;
  for(int j=0;j<10&&fp;j++){dladdr((void*)fp[1],&d);fprintf(stderr,"AUDIO FRAME %s+%llx %s\n",d.dli_fname?d.dli_fname:"?",fp[1]-(unsigned long long)d.dli_fbase,d.dli_sname?d.dli_sname:"?");fp=(unsigned long long*)fp[0];}
  _exit(128+sig);
 };
 for(int sig:{SIGSEGV,SIGBUS,SIGABRT,SIGILL})sigaction(sig,&action,nullptr);
 // Exact FMOD pre-enumeration request at 0.737 PC 0x101f8ce22.
 AudioObjectPropertyAddress runLoop={kAudioHardwarePropertyRunLoop,kAudioObjectPropertyScopeGlobal,0};
 CFRunLoopRef loop=nullptr;Boolean writable=false;UInt32 loopSize=0;
 auto runLoopStatus=AudioObjectSetPropertyData(1,&runLoop,0,nullptr,sizeof(loop),&loop);
 printf("AUDIO FMOD runloop-init status=%d\n",runLoopStatus);assert(!runLoopStatus);
 assert(AudioObjectHasProperty(1,&runLoop));
 assert(!AudioObjectIsPropertySettable(1,&runLoop,&writable)&&writable);
 assert(!AudioObjectGetPropertyDataSize(1,&runLoop,0,nullptr,&loopSize)&&loopSize==sizeof(loop));
 loop=CFRunLoopGetCurrent();assert(AudioObjectSetPropertyData(1,&runLoop,0,nullptr,sizeof(loop),&loop)==kAudioHardwareUnsupportedOperationError);
 assert(!AudioObjectGetPropertyData(1,&runLoop,0,nullptr,&loopSize,&loop)&&loop==nullptr);
 assert(AudioObjectSetPropertyData(1,&runLoop,0,nullptr,1,&loop)==kAudioHardwareBadPropertySizeError);
 printf("PASS FMOD NULL runloop initialization, readback, non-NULL/size validation\n");
 AudioObjectPropertyAddress a={kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,0}; UInt32 n=0;
 OSStatus s=AudioObjectGetPropertyDataSize(1,&a,0,nullptr,&n);
 printf("AUDIO inventory status=%d bytes=%u\n",s,n); if(s) return 1;
 std::vector<AudioDeviceID> ids(n/4); assert(!AudioObjectGetPropertyData(1,&a,0,nullptr,&n,ids.data()));
 for(auto id:ids) {
  a.mSelector=kAudioObjectPropertyName; n=sizeof(CFStringRef); CFStringRef name=nullptr;
  s=AudioObjectGetPropertyData(id,&a,0,nullptr,&n,&name); char text[512]={};
  if(!s&&name) {CFStringGetCString(name,text,sizeof(text),kCFStringEncodingUTF8); CFRelease(name);}
  printf("AUDIO device=%u name=%s",id,text);
  for(auto scope:{kAudioObjectPropertyScopeInput,kAudioObjectPropertyScopeOutput}) {
   a.mSelector=kAudioDevicePropertyStreamConfiguration; a.mScope=scope; n=0;
   s=AudioObjectGetPropertyDataSize(id,&a,0,nullptr,&n); std::vector<unsigned char> b(n);
   unsigned channels=0; if(!s&&n) {s=AudioObjectGetPropertyData(id,&a,0,nullptr,&n,b.data()); auto* list=(AudioBufferList*)b.data(); if(!s) for(unsigned j=0;j<list->mNumberBuffers;j++)channels+=list->mBuffers[j].mNumberChannels;}
   printf(" %s=%u",scope==kAudioObjectPropertyScopeInput?"input":"output",channels);
  } puts(""); a.mScope=kAudioObjectPropertyScopeGlobal;
 }
 // Playback property sequence observed in FMOD's 0.737 CoreAudio backend.
 for(auto id:ids) {
  AudioObjectPropertyAddress a={kAudioDevicePropertyBufferSizeRange,kAudioObjectPropertyScopeOutput,0};
  AudioValueRange range={};UInt32 size=sizeof(range);assert(!AudioObjectGetPropertyData(id,&a,0,nullptr,&size,&range));assert(range.mMaximum>=range.mMinimum&&range.mMinimum>0);
  a.mSelector=kAudioDevicePropertyStreamFormat;AudioStreamBasicDescription format={};size=sizeof(format);
  assert(!AudioObjectGetPropertyData(id,&a,0,nullptr,&size,&format));assert(format.mBytesPerFrame==format.mChannelsPerFrame*format.mBitsPerChannel/8);
  a.mSelector=kAudioDevicePropertyBufferSize;UInt32 bytes=0;size=4;assert(!AudioObjectGetPropertyData(id,&a,0,nullptr,&size,&bytes));
  assert(bytes>=range.mMinimum&&bytes<=range.mMaximum);
  if(argc>1&&!strcmp(argv[1],"--synthetic")) {
   for(UInt32 value:{UInt32(range.mMinimum),UInt32(range.mMaximum)}) {
    assert(!AudioObjectSetPropertyData(id,&a,0,nullptr,4,&value));
    UInt32 readback=0;size=4;assert(!AudioObjectGetPropertyData(id,&a,0,nullptr,&size,&readback)&&readback==value);
   }
   for(UInt32 value:{UInt32(range.mMinimum)-format.mBytesPerFrame,UInt32(range.mMaximum)+format.mBytesPerFrame,UInt32(range.mMinimum)+1})
    assert(AudioObjectSetPropertyData(id,&a,0,nullptr,4,&value)==kAudioHardwareIllegalOperationError);
   assert(AudioObjectSetPropertyData(id,&a,0,nullptr,1,&bytes)==kAudioHardwareBadPropertySizeError);
   assert(!AudioObjectSetPropertyData(id,&a,0,nullptr,4,&bytes));
  }
  a.mSelector=kAudioDevicePropertyLatency;UInt32 latency=0;size=4;assert(!AudioObjectGetPropertyData(id,&a,0,nullptr,&size,&latency));
  for(auto scope:{kAudioObjectPropertyScopeInput,kAudioObjectPropertyScopeOutput}) {
   a.mSelector=kAudioDevicePropertyStreams;a.mScope=scope;size=0;assert(!AudioObjectGetPropertyDataSize(id,&a,0,nullptr,&size));
   if(!size)continue;assert(size==4);AudioObjectID stream=0;assert(!AudioObjectGetPropertyData(id,&a,0,nullptr,&size,&stream)&&stream!=id);
   a={kAudioObjectPropertyOwner,kAudioObjectPropertyScopeGlobal,0};UInt32 owner=0;size=4;assert(!AudioObjectGetPropertyData(stream,&a,0,nullptr,&size,&owner)&&owner==id);
   a.mSelector=kAudioDevicePropertyStreamFormat;size=sizeof(format);assert(!AudioObjectGetPropertyData(stream,&a,0,nullptr,&size,&format));
  }
 }
 printf("PASS FMOD legacy byte-buffer range, device latency and PCM stream queries\n");
 for(auto sel:{kAudioHardwarePropertyDefaultInputDevice,kAudioHardwarePropertyDefaultOutputDevice}) {a.mSelector=sel; n=4; AudioDeviceID id=0; s=AudioObjectGetPropertyData(1,&a,0,nullptr,&n,&id); printf("AUDIO default %s=%u status=%d\n",sel==kAudioHardwarePropertyDefaultInputDevice?"input":"output",id,s);}
 if(argc>1) {
  assert(!strcmp(argv[1],"--synthetic"));assert(argc==3);chdir(argv[2]);
  // Hardcoded private null endpoints: this mode can never select a microphone.
  auto output=lookup("PulseAudio:sink:tracka_test_B"),input=lookup("PulseAudio:source:tracka_test_B.monitor");
  AudioObjectPropertyAddress listener={kAudioHardwarePropertyDefaultOutputDevice,kAudioObjectPropertyScopeGlobal,0};
  assert(!AudioObjectAddPropertyListener(1,&listener,changed,nullptr));
  AudioObjectPropertyAddress format={kAudioDevicePropertyStreamFormat,kAudioObjectPropertyScopeGlobal,0};
  AudioStreamBasicDescription f={48000,kAudioFormatLinearPCM,kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked,8,1,8,2,32,0};
  for(auto id:{output,input})assert(!AudioObjectSetPropertyData(id,&format,0,nullptr,sizeof(f),&f));
  for(int round=0;round<3;round++) {
   outputCalls=0;inputCalls=0;signalCalls=0;AudioDeviceIOProcID op=nullptr,ip=nullptr;
   assert(!AudioDeviceCreateIOProcID(output,audio,nullptr,&op));assert(!AudioDeviceCreateIOProcID(input,audio,nullptr,&ip));
   assert(!AudioDeviceStart(output,op));assert(!AudioDeviceStart(input,ip));
   if(round==0){signalFile("routing-ready");for(int j=0;j<150&&access("routing-checked",F_OK);j++)usleep(100000);assert(!access("routing-checked",F_OK));}
   for(int j=0;j<50&&(!outputCalls||!signalCalls);j++)usleep(100000);
   printf("AUDIO route round=%d output=%u input=%u signal=%u\n",round,outputCalls.load(),inputCalls.load(),signalCalls.load());
   assert(outputCalls&&inputCalls&&signalCalls);
   assert(!AudioDeviceStop(input,ip));assert(!AudioDeviceStop(output,op));
   assert(!AudioDeviceDestroyIOProcID(input,ip));assert(!AudioDeviceDestroyIOProcID(output,op));
  }
  checkUnits(input,output,f);
  AudioObjectPropertyAddress devices={kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,0};
  unsigned char guard[16];memset(guard,0xa5,sizeof(guard));UInt32 shortSize=1;
  assert(AudioObjectGetPropertyData(1,&devices,0,nullptr,&shortSize,guard)==kAudioHardwareBadPropertySizeError);
  for(auto byte:guard)assert(byte==0xa5);
  std::vector<std::thread> readers;
  for(int j=0;j<8;j++)readers.emplace_back([&]{for(int k=0;k<100;k++){
   assert(lookup("PulseAudio:sink:tracka_test_B")==output);
   AudioObjectPropertyAddress a={kAudioObjectPropertyName,kAudioObjectPropertyScopeGlobal,0};CFStringRef name=nullptr;UInt32 size=sizeof(name);
   assert(!AudioObjectGetPropertyData(output,&a,0,nullptr,&size,&name));assert(name);CFRelease(name);
  }});
  for(auto& reader:readers)reader.join();
  auto other=lookup("PulseAudio:sink:tracka_test_A");
  for(int j=0;j<32;j++){AudioDeviceIOProcID proc=nullptr;assert(!AudioDeviceCreateIOProcID(other,audio,nullptr,&proc));assert(!AudioDeviceStart(other,proc));assert(!AudioDeviceStop(other,proc));assert(!AudioDeviceDestroyIOProcID(other,proc));}
  assert(!AudioObjectAddPropertyListener(1,&devices,changed,nullptr));
  signalFile("hotplug-ready");waitFile("hotplug-added");usleep(1200000);
  auto added=lookup("PulseAudio:sink:tracka_test_C");signalFile("hotplug-seen");waitFile("hotplug-removed");usleep(1200000);
  AudioObjectPropertyAddress alive={kAudioDevicePropertyDeviceIsAlive,kAudioObjectPropertyScopeGlobal,0};UInt32 live=1,size=4;
  assert(!AudioObjectGetPropertyData(added,&alive,0,nullptr,&size,&live));assert(!live);
  signalFile("hotplug-gone");waitFile("hotplug-restored");usleep(1200000);
  assert(lookup("PulseAudio:sink:tracka_test_C")==added);
  assert(!AudioObjectRemovePropertyListener(1,&devices,changed,nullptr));
  printf("PASS short-buffer protection, 8 concurrent readers, 32 immediate stop cycles, hotplug and stable UID\n");
  assert(changes);assert(!AudioObjectRemovePropertyListener(1,&listener,changed,nullptr));
  printf("PASS synthetic routing, repeated starts, default notification (%u)\n",changes.load());
 }
 return 0;
}
