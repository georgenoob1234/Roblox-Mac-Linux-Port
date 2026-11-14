// Track A: real PulseAudio endpoints. Included by AudioHardware.cpp so all
// legacy and modern CoreAudio entry points use the same object registry.
#include <pulse/pulseaudio.h>
#include <CoreFoundation/CFRunLoop.h>
#include <chrono>
#include <atomic>
#include <vector>
#include <string>
#include <algorithm>
#include <unistd.h>

struct HostEndpoint {
 std::string name, description;
 bool input;
 uint32_t channels, rate, latencyFrames;
 pa_channel_map channelMap;
};
struct HostSnapshot {
 std::vector<HostEndpoint> devices;
 std::string input, output;
 bool failed=false;
};
// No host thread calls Darwin code: this private native mainloop is pumped on
// the caller's Darwin thread. Never creates a playback or recording stream.
//
// The connection is kept open and subscribed to sink, source and server
// events. Reconnecting for every poll cost one ~23 ms round of connection
// setup per second (and one libpulse cookie warning each time); now an
// unchanged server costs one non-blocking mainloop pass per poll, and the
// inventory is only read again after the server reports a change.
// All of this state is guarded by g_inventoryMutex.
struct HostInventory {
 pa_mainloop* loop=nullptr;
 pa_context* context=nullptr;
 bool dirty=true;
};
static HostInventory g_inventory;
static void hostInventoryClose() {
 auto& v=g_inventory;
 if(v.context){pa_context_set_subscribe_callback(v.context,nullptr,nullptr);pa_context_disconnect(v.context);pa_context_unref(v.context);}
 if(v.loop)pa_mainloop_free(v.loop);
 v=HostInventory();
}
enum class HostRefresh {Failed,Unchanged,Fresh};
static HostRefresh hostSnapshot(HostSnapshot& s) {
 auto& v=g_inventory;
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 auto pump=[&] {
  if(std::chrono::steady_clock::now()>=deadline || !PA_CONTEXT_IS_GOOD(pa_context_get_state(v.context)))return false;
  if(pa_mainloop_iterate(v.loop,0,nullptr)<0)return false;
  usleep(1000); return true;
 };
 auto finish=[&](pa_operation* op) {
  if(!op)return false;
  bool done=true;
  while(pa_operation_get_state(op)==PA_OPERATION_RUNNING)if(!pump()){done=false;pa_operation_cancel(op);break;}
  pa_operation_unref(op); return done&&!s.failed;
 };
 if(v.context) {
  // Deliver queued change events without blocking. The bound only stops a
  // server that never goes quiet from holding the caller here.
  int dispatched=1;
  for(int pass=0;pass<64&&dispatched>0;++pass)dispatched=pa_mainloop_iterate(v.loop,0,nullptr);
  if(dispatched<0||!PA_CONTEXT_IS_GOOD(pa_context_get_state(v.context)))hostInventoryClose(); // server went away: reconnect below
 }
 if(!v.context) {
  v.loop=pa_mainloop_new(); if(!v.loop)return HostRefresh::Failed;
  v.context=pa_context_new(pa_mainloop_get_api(v.loop),"CoreAudio device inventory");
  bool ok=v.context && pa_context_connect(v.context,nullptr,PA_CONTEXT_NOAUTOSPAWN,nullptr)>=0;
  if(ok)while(pa_context_get_state(v.context)!=PA_CONTEXT_READY)if(!pump()){ok=false;break;}
  if(ok) {
   pa_context_set_subscribe_callback(v.context,[](pa_context*,pa_subscription_event_type_t,uint32_t,void* p){*(bool*)p=true;},&v.dirty);
   ok=finish(pa_context_subscribe(v.context,pa_subscription_mask_t(PA_SUBSCRIPTION_MASK_SINK|PA_SUBSCRIPTION_MASK_SOURCE|PA_SUBSCRIPTION_MASK_SERVER),nullptr,nullptr));
  }
  if(!ok){hostInventoryClose();return HostRefresh::Failed;}
  v.dirty=true;
 }
 if(!v.dirty)return HostRefresh::Unchanged;
 // Cleared before the queries: an event that arrives while they run marks the
 // inventory dirty again and is picked up by the next poll.
 v.dirty=false;
 bool ok=finish(pa_context_get_server_info(v.context,[](pa_context*,const pa_server_info* i,void* p){
  auto& s=*(HostSnapshot*)p;if(!i){s.failed=true;return;}
  s.input=i->default_source_name?i->default_source_name:"";s.output=i->default_sink_name?i->default_sink_name:"";
 },&s));
 if(ok)ok=finish(pa_context_get_sink_info_list(v.context,[](pa_context*,const pa_sink_info* i,int end,void* p){
  auto& s=*(HostSnapshot*)p;if(end<0)s.failed=true;if(end||!i)return;
  s.devices.push_back({i->name,i->description?i->description:i->name,false,i->sample_spec.channels,i->sample_spec.rate,uint32_t(std::min(4294967295.0,double(i->latency)*i->sample_spec.rate/1000000.0+0.999999)),i->channel_map});
 },&s));
 if(ok)ok=finish(pa_context_get_source_info_list(v.context,[](pa_context*,const pa_source_info* i,int end,void* p){
  auto& s=*(HostSnapshot*)p;if(end<0)s.failed=true;if(end||!i)return;
  // Include monitor sources as real, explicitly labelled host input endpoints.
  s.devices.push_back({i->name,i->description?i->description:i->name,true,i->sample_spec.channels,i->sample_spec.rate,uint32_t(std::min(4294967295.0,double(i->latency)*i->sample_spec.rate/1000000.0+0.999999)),i->channel_map});
 },&s));
 // A failed read leaves no trustworthy state behind; the next poll reconnects
 // and reads everything again.
 if(!ok){hostInventoryClose();return HostRefresh::Failed;}
 return HostRefresh::Fresh;
}
static OSStatus hostCopy(UInt32* size,void* out,const void* value,UInt32 required) {
 if(!size)return kAudioHardwareIllegalOperationError;
 UInt32 capacity=*size;*size=required;
 if(out&&capacity<required)return kAudioHardwareBadPropertySizeError;
 if(out&&required)memcpy(out,value,required);
 return noErr;
}
class HostDevice : public AudioHardwareImplPA {
public:
 HostEndpoint endpoint;
 std::atomic<bool> alive{true};
 HostDevice(AudioDeviceID id,const HostEndpoint& e):AudioHardwareImplPA(id),endpoint(e) {
  m_name=CFStringCreateWithCString(nullptr,e.description.c_str(),kCFStringEncodingUTF8);
  std::string uid=std::string("PulseAudio:")+(e.input?"source:":"sink:")+e.name;
  m_uid=CFStringCreateWithCString(nullptr,uid.c_str(),kCFStringEncodingUTF8);
  m_asbd.mChannelsPerFrame=e.channels;m_asbd.mSampleRate=e.rate;
  m_asbd.mBytesPerPacket=m_asbd.mBytesPerFrame=e.channels*4;
 }
 const char* deviceName() const override {return endpoint.name.c_str();}
 OSStatus getPropertyData(const AudioObjectPropertyAddress* a,UInt32 qs,const void* q,UInt32* size,void* out) override {
  bool matches=a->mScope==(endpoint.input?kAudioObjectPropertyScopeInput:kAudioObjectPropertyScopeOutput);
  switch(a->mSelector) {
   case kAudioDevicePropertyPreferredChannelLayout: {
    if(!matches)return kAudioHardwareUnknownPropertyError;
    const auto& map=endpoint.channelMap;AudioChannelLayout layout={};
    if(map.channels==1&&map.map[0]==PA_CHANNEL_POSITION_MONO)layout.mChannelLayoutTag=kAudioChannelLayoutTag_Mono;
    else if(map.channels==2&&map.map[0]==PA_CHANNEL_POSITION_FRONT_LEFT&&map.map[1]==PA_CHANNEL_POSITION_FRONT_RIGHT)layout.mChannelLayoutTag=kAudioChannelLayoutTag_Stereo;
    else return kAudioHardwareUnknownPropertyError;
    return hostCopy(size,out,&layout,offsetof(AudioChannelLayout,mChannelDescriptions));
   }
   case kAudioDevicePropertyStreamConfiguration: {
    AudioBufferList list={}; list.mNumberBuffers=matches?1:0;
    list.mBuffers[0].mNumberChannels=endpoint.channels;
    return hostCopy(size,out,&list,offsetof(AudioBufferList,mBuffers)+(matches?sizeof(AudioBuffer):0));
   }
   case kAudioDevicePropertyDeviceIsAlive:{UInt32 v=alive;return hostCopy(size,out,&v,sizeof(v));}
   case kAudioDevicePropertyDeviceCanBeDefaultDevice:
   case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:{UInt32 v=matches;return hostCopy(size,out,&v,sizeof(v));}
   case kAudioDevicePropertyNominalSampleRate:{Float64 v=endpoint.rate;return hostCopy(size,out,&v,sizeof(v));}
   case kAudioDevicePropertyAvailableNominalSampleRates:{AudioValueRange v={double(endpoint.rate),double(endpoint.rate)};return hostCopy(size,out,&v,sizeof(v));}
   case kAudioDevicePropertyBufferSize:{UInt32 v=bufferSize();return hostCopy(size,out,&v,sizeof(v));}
   case kAudioDevicePropertyBufferSizeRange:{auto frame=asbd().mBytesPerFrame;AudioValueRange v={double(32*frame),double(8192*frame)};return hostCopy(size,out,&v,sizeof(v));}
   case kAudioDevicePropertyLatency:{UInt32 v=endpoint.latencyFrames;return hostCopy(size,out,&v,sizeof(v));}
   case kAudioDevicePropertyStreams:{AudioObjectID stream=id()+1;return hostCopy(size,out,&stream,matches?sizeof(stream):0);}
   case kAudioDevicePropertyBufferFrameSize:{UInt32 v=bufferSize()/asbd().mBytesPerFrame;return hostCopy(size,out,&v,sizeof(v));}
   case kAudioDevicePropertyBufferFrameSizeRange:{AudioValueRange v={32,8192};return hostCopy(size,out,&v,sizeof(v));}
  }
  return AudioHardwareImplPA::getPropertyData(a,qs,q,size,out);
 }
 OSStatus isPropertySettable(const AudioObjectPropertyAddress* a,Boolean* out) override {
  if(a->mSelector==kAudioDevicePropertyBufferFrameSize){*out=true;return noErr;}
  return AudioHardwareImplPA::isPropertySettable(a,out);
 }
 OSStatus setPropertyData(const AudioObjectPropertyAddress* a,UInt32 qs,const void* q,UInt32 size,const void* in) override {
  if(a->mSelector==kAudioDevicePropertyBufferSize){
   if(size!=4)return kAudioHardwareBadPropertySizeError;
   auto bytes=*(const UInt32*)in,frame=asbd().mBytesPerFrame;
   if(bytes<32*frame||bytes>8192*frame||bytes%frame)return kAudioHardwareIllegalOperationError;
   return setBufferSize(bytes);
  }
  if(a->mSelector==kAudioDevicePropertyBufferFrameSize){
   if(size!=4)return kAudioHardwareBadPropertySizeError;
   auto frames=*(const UInt32*)in;if(frames<32||frames>8192)return kAudioHardwareIllegalOperationError;
   return setBufferSize(frames*asbd().mBytesPerFrame);
  }
  return AudioHardwareImplPA::setPropertyData(a,qs,q,size,in);
 }
 OSStatus start(AudioDeviceIOProcID proc,AudioTimeStamp* time,UInt32 flags) override {
  if(!alive)return kAudioHardwareBadDeviceError;
  return AudioHardwareImplPA::start(proc,time,flags);
 }
protected:
 AudioHardwareStream* createStream(AudioDeviceIOProc cb,void* data) override {
  if(endpoint.input)return new AudioHardwareStreamPAInput(this,cb,data);
  return new AudioHardwareStreamPAOutput(this,cb,data);
 }
};
// Each Pulse endpoint has one interleaved PCM stream; expose a real stream
// object instead of reusing a device ID. There is no additional stream DSP delay.
class HostDeviceStream : public AudioHardwareImpl {
 HostDevice* device;
public:
 HostDeviceStream(HostDevice* d):AudioHardwareImpl(d->id()+1),device(d){}
 OSStatus getPropertyData(const AudioObjectPropertyAddress* a,UInt32 qs,const void* q,UInt32* size,void* out) override {
  switch(a->mSelector){
   case kAudioObjectPropertyClass:{UInt32 v='astr';return hostCopy(size,out,&v,4);}
   case kAudioObjectPropertyOwner:{UInt32 v=device->id();return hostCopy(size,out,&v,4);}
   case 'sdir':{UInt32 v=device->endpoint.input;return hostCopy(size,out,&v,4);}
   case 'schn':{UInt32 v=1;return hostCopy(size,out,&v,4);}
   case kAudioDevicePropertyLatency:{UInt32 v=0;return hostCopy(size,out,&v,4);}
   case 'pft ':
   case kAudioDevicePropertyStreamFormat:{auto f=device->asbd();return hostCopy(size,out,&f,sizeof(f));}
  }
  return AudioHardwareImpl::getPropertyData(a,qs,q,size,out);
 }
protected:
 AudioHardwareStream* createStream(AudioDeviceIOProc,void*) override {return nullptr;}
};
// Never destroyed, like g_objects: see the note there.
static auto& g_objectsMutex=*new std::recursive_mutex;
static auto& g_hostIDs=*new std::vector<AudioDeviceID>;
static AudioDeviceID g_defaultInput=0,g_defaultOutput=0;
static std::chrono::steady_clock::time_point g_refresh;
struct HostListener {AudioObjectID id;AudioObjectPropertyAddress address;AudioObjectPropertyListenerProc proc;void* data;};
static auto& g_listenerMutex=*new std::recursive_mutex;
static auto& g_listeners=*new std::vector<HostListener>;
static bool sameAddress(const AudioObjectPropertyAddress& a,const AudioObjectPropertyAddress& b) {
 return a.mSelector==b.mSelector&&a.mScope==b.mScope&&a.mElement==b.mElement;
}
// Serializes the inventory connection and g_refresh. Taken before
// g_objectsMutex, never while holding it. Recursive because device
// construction below may re-enter through the public CoreAudio entry points.
static auto& g_inventoryMutex=*new std::recursive_mutex;
static std::atomic<bool> g_inventoryLoaded{false};
static void refreshHostDevices() {
 std::vector<std::pair<AudioObjectID,AudioObjectPropertySelector>> changes;
 {
  // The sound server is queried without g_objectsMutex, so property reads on
  // other threads no longer wait behind it. Until the first inventory has been
  // applied every caller waits for it, because an empty device list would be
  // wrong; afterwards a caller that finds a refresh already running skips it
  // and reads the registry as it is.
  std::unique_lock<std::recursive_mutex> inventory(g_inventoryMutex,std::defer_lock);
  if(!g_inventoryLoaded.load(std::memory_order_acquire))inventory.lock();
  else if(!inventory.try_lock())return;
  auto now=std::chrono::steady_clock::now();if(now<g_refresh)return;
  g_refresh=now+std::chrono::seconds(1);
  HostSnapshot snapshot;if(hostSnapshot(snapshot)!=HostRefresh::Fresh)return; // unchanged, or preserve last good snapshot on temporary server failure
  std::lock_guard<std::recursive_mutex> lock(g_objectsMutex);
  std::vector<AudioDeviceID> ids;AudioDeviceID input=0,output=0;
  for(const auto& e:snapshot.devices) {
   HostDevice* device=nullptr;
   for(auto& item:g_objects)if(auto* d=dynamic_cast<HostDevice*>(item.second.get()))
    if(d->endpoint.input==e.input&&d->endpoint.name==e.name){device=d;break;}
   if(!device) {AudioDeviceID id=16+g_objects.size();auto d=std::make_unique<HostDevice>(id,e);device=d.get();g_objects.emplace(id,std::move(d));g_objects.emplace(id+1,std::make_unique<HostDeviceStream>(device));}
   ids.push_back(device->id());
   if(e.input&&e.name==snapshot.input)input=device->id();
   if(!e.input&&e.name==snapshot.output)output=device->id();
  }
  std::sort(ids.begin(),ids.end());
  for(auto& item:g_objects)if(auto* d=dynamic_cast<HostDevice*>(item.second.get())) {
   bool alive=std::binary_search(ids.begin(),ids.end(),d->id());
   if(d->alive.exchange(alive)!=alive)changes.push_back({d->id(),kAudioDevicePropertyDeviceIsAlive});
  }
  if(ids!=g_hostIDs)changes.push_back({1,kAudioHardwarePropertyDevices});
  if(input!=g_defaultInput)changes.push_back({1,kAudioHardwarePropertyDefaultInputDevice});
  if(output!=g_defaultOutput){changes.push_back({1,kAudioHardwarePropertyDefaultOutputDevice});changes.push_back({1,kAudioHardwarePropertyDefaultSystemOutputDevice});}
  g_hostIDs=std::move(ids);g_defaultInput=input;g_defaultOutput=output;
  g_inventoryLoaded.store(true,std::memory_order_release);
 }
 // Serialize removal with callback execution, without holding the registry lock.
 std::lock_guard<std::recursive_mutex> lock(g_listenerMutex);
 auto listeners=g_listeners;
 for(auto change:changes)for(auto listener:listeners) {
  auto found=std::find_if(g_listeners.begin(),g_listeners.end(),[&](const HostListener& l){return l.id==listener.id&&l.proc==listener.proc&&l.data==listener.data&&sameAddress(l.address,listener.address);});
  if(found!=g_listeners.end()&&listener.id==change.first&&(listener.address.mSelector==change.second||listener.address.mSelector==AudioObjectPropertySelector('****'))) {
   auto address=listener.address;address.mSelector=change.second;listener.proc(listener.id,1,&address,listener.data);
  }
 }
}
static OSStatus hostSystemProperty(const AudioObjectPropertyAddress* a,UInt32 qs,const void* q,UInt32* size,void* out) {
 std::lock_guard<std::recursive_mutex> lock(g_objectsMutex);
 switch(a->mSelector) {
  case kAudioHardwarePropertyRunLoop:{CFRunLoopRef loop=nullptr;return hostCopy(size,out,&loop,sizeof(loop));}
  case kAudioHardwarePropertyDevices:return hostCopy(size,out,g_hostIDs.data(),g_hostIDs.size()*sizeof(AudioDeviceID));
  case kAudioHardwarePropertyDefaultInputDevice:return hostCopy(size,out,&g_defaultInput,4);
  case kAudioHardwarePropertyDefaultOutputDevice:
  case kAudioHardwarePropertyDefaultSystemOutputDevice:return hostCopy(size,out,&g_defaultOutput,4);
  case kAudioHardwarePropertyTranslateUIDToDevice: {
   if(qs!=sizeof(CFStringRef)||!q)return kAudioHardwareBadPropertySizeError;
   AudioDeviceID id=0;for(auto candidate:g_hostIDs){CFStringRef uid=nullptr;UInt32 n=sizeof(uid);AudioObjectPropertyAddress address={kAudioDevicePropertyDeviceUID,kAudioObjectPropertyScopeGlobal,0};
    g_objects.at(candidate)->getPropertyData(&address,0,nullptr,&n,&uid);if(uid){if(CFEqual(uid,*(CFStringRef*)q))id=candidate;CFRelease(uid);}}
   return hostCopy(size,out,&id,4);
  }
 }
 return kAudioHardwareUnknownPropertyError;
}
