#include "model.h"
#include "resources.h"
#include "panel.h"
#include "gpu-usage.h"
#include "sampler.h"
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <EGL/egl.h>
#include <atomic>
#include <mutex>
#include <chrono>
#include <ctime>
#include <locale.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sched.h>
#include <cstring>
#include <limits>
#include <fstream>
#include <sstream>
#include <cxxabi.h>
#include <vulkan/vulkan.h>

static uint64_t now_ns(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000+t.tv_nsec;}
static std::atomic<bool> shown{false},paused{false},save_requested{false};
static std::atomic<bool> hosted{false};
extern "C" void rbx_profiler_host_in_gtk(){hosted=true;}
static std::atomic<bool> reset_capture{true};
static std::atomic<bool> snapshot_requested{false};
static std::mutex collectors_mutex;
static std::unique_ptr<ProcessSampler> sampler;
static std::unique_ptr<ProcessGpuUsage> gpu_usage;
static void stop_collectors(){std::lock_guard<std::mutex> lock(collectors_mutex);sampler.reset();gpu_usage.reset();}
static uint64_t capture_duration_ns=0;
// HUD refresh period. The measured numbers came from a 1s tumbling bucket that
// only advanced once a second; they are now a rolling 1s window recomputed at
// this rate. 0 rebuilds every frame, which multiplies ImGui layout work by the
// frame rate: ROBLOX_MAC_PROFILER_HZ picks the tradeoff, default 15.
static uint64_t display_period_ns=1000000000/15;
extern "C" unsigned rbx_profiler_refresh_ms(){return display_period_ns?std::max<uint64_t>(1,(display_period_ns+999999)/1000000):16;}
static bool detailed_scopes=false;
static const char *cpu_categories[]={"guest (native)","runtime","host libraries","unaccounted"};
static std::atomic<uint64_t> panel_revision{1};
extern "C" uint64_t rbx_profiler_revision(){return panel_revision.load(std::memory_order_relaxed);}
static bool capture_has_overlay=false;
static uint64_t layout_builds=0,cached_draws=0,symbol_hits=0,symbol_misses=0,symbol_time_ns=0;
static std::atomic<uint64_t> dropped{0};
static std::atomic<uint64_t> diagnostic_contention_drops{0};
// Exported alongside the model/sampling snapshot so one export describes one
// moment. These counters keep moving while the capture is frozen (the HUD still
// draws, and export_trace itself resolves symbols), so reading them live made
// repeat exports of the same frozen window disagree.
struct WorkSnapshot {uint64_t layouts=0,draws=0,hits=0,misses=0,symbol_ns=0,drops=0;bool overlay=false,had_overlay=false;};
static WorkSnapshot work_now(){return {layout_builds,cached_draws,symbol_hits,symbol_misses,symbol_time_ns,dropped.load(),shown.load(),capture_has_overlay};}
static std::mutex samples_mutex;
static ProfModel model;
static LayerAccounting layer;
static std::atomic<uint64_t> cpu_epoch{1};
static std::mutex panel_mutex;
static ProfPanel panel;
static std::atomic<int(*)(const void*,void*)> guest_symbolizer{nullptr};
static void set_symbolizer(int(*resolve)(const void*,void*)){guest_symbolizer=resolve;}
extern "C" bool rbx_profiler_mouse(int kind,float x,float y,float wheel,bool available){
 if(!shown.load(std::memory_order_relaxed))return false;
 std::lock_guard<std::mutex> lock(panel_mutex);
 if(!available || !shown.load())panel.dragging=panel.moving=false;
 bool consumed=panel.mouse(kind,x,y,wheel,available && shown.load());
 if(consumed)++panel_revision;return consumed;
}
static const char *presentation_policy(){const char *v=getenv("ROBLOX_MAC_SWAP_INTERVAL");return v && *v?v:"Metal displaySyncEnabled";}
static int enabled(){return shown.load(std::memory_order_relaxed) && !paused.load(std::memory_order_relaxed);}
static uint64_t thread_cpu(){timespec t{};clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return uint64_t(t.tv_sec)*1000000000+t.tv_nsec;}
static void span_begin(RbxProfilerSpan *span){
 if(!span)return;
 span->start_ns=span->cpu_ns=span->epoch=0;
 if(!enabled() || reset_capture.load(std::memory_order_relaxed))return;
 const uint64_t epoch=cpu_epoch.load(std::memory_order_relaxed);
 const uint64_t start=now_ns(),cpu=thread_cpu();
 if(!enabled() || cpu_epoch.load(std::memory_order_relaxed)!=epoch)return;
 span->start_ns=start;span->cpu_ns=cpu;span->epoch=epoch;
}
static void span_end(const RbxProfilerSpan *span,unsigned id,uint64_t bytes,uint64_t object,uint64_t detail){
 if(!span || !span->start_ns || id>=RBX_DIAG_COUNT || !enabled() || reset_capture.load(std::memory_order_relaxed))return;
 const uint64_t end=now_ns(),cpu=thread_cpu();
 if(end<span->start_ns || cpu<span->cpu_ns)return;
 std::unique_lock<std::mutex> lock(samples_mutex,std::try_to_lock);
 if(!lock){++diagnostic_contention_drops;return;}
 const uint64_t epoch=cpu_epoch.load(std::memory_order_relaxed);
 if(!enabled() || reset_capture.load(std::memory_order_relaxed) || span->epoch!=epoch)return;
 // The state/epoch transition is serialized by samples_mutex in rbx_profiler_key.
 // A second check keeps an auto-capture transition from admitting a late span.
 if(!enabled() || cpu_epoch.load(std::memory_order_relaxed)!=epoch)return;
 static thread_local uint32_t tid=syscall(SYS_gettid);
 model.diagnostic(span->start_ns,end,cpu-span->cpu_ns,tid,id,bytes,object,detail);
}
struct CpuState {unsigned stage=RBX_PROF_COUNT;uint64_t time=0,epoch=0;int core=-1;};
static thread_local CpuState cpu_state;
static void cpu_switch(unsigned next){
 auto &s=cpu_state;uint64_t time=thread_cpu(),epoch=cpu_epoch.load();int core=sched_getcpu();
 if(enabled() && s.epoch==epoch && time>=s.time)layer.charge(s.stage,s.core,core,time-s.time);
 s.stage=next;s.time=time;s.core=core;s.epoch=epoch;
}
static unsigned cpu_begin(unsigned stage){
 if(stage>=RBX_PROF_COUNT || !detailed_scopes || !enabled())return UINT32_MAX;
 unsigned previous=cpu_state.stage;cpu_switch(stage);return previous;
}
static void cpu_end(unsigned previous){if(previous==UINT32_MAX)return;if(!enabled()){cpu_state.stage=RBX_PROF_COUNT;cpu_state.epoch=0;return;}cpu_switch(previous);}
struct NativeCpuScope {
 unsigned previous;explicit NativeCpuScope(unsigned stage):previous(cpu_begin(stage)){}
 ~NativeCpuScope(){cpu_end(previous);}
};
// Only ImGui allocations are tracked. No allocator callback is installed in Vulkan.
struct Allocation {void *base;size_t size;unsigned stage;};
static void *VKAPI_PTR host_allocate(void *user,size_t size,size_t alignment,VkSystemAllocationScope){
 if(!size)return nullptr;
 alignment=std::max(alignment,alignof(Allocation));
 if((alignment&(alignment-1)) || size>SIZE_MAX-sizeof(Allocation)-(alignment-1))return nullptr;
 void *base=malloc(size+sizeof(Allocation)+alignment-1);if(!base)return nullptr;
 auto address=(reinterpret_cast<uintptr_t>(base)+sizeof(Allocation)+alignment-1)&~(uintptr_t(alignment)-1);
 auto *header=reinterpret_cast<Allocation*>(address)-1;unsigned stage=reinterpret_cast<uintptr_t>(user);
 *header={base,size,stage};layer.allocated(stage,size);return reinterpret_cast<void*>(address);
}
static void VKAPI_PTR host_free(void*,void *pointer){
 if(!pointer)return;auto *header=static_cast<Allocation*>(pointer)-1;
 layer.freed(header->stage,header->size);free(header->base);
}
// Keep Vulkan on its default allocator. Diagnostic callbacks must not remain
// attached to driver objects after the overlay closes. Only ImGui uses our tracker.
static const void *allocator(unsigned){return nullptr;}
static void *imgui_allocate(size_t size,void*){return host_allocate(reinterpret_cast<void*>(uintptr_t(RBX_PROF_HUD)),size,16,VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);}
static void imgui_free(void *p,void*){host_free(nullptr,p);}
// Timestamp pairs do not change GL query state and never wait for the GPU.
// Game and GTK rendering have separate thread-owned EGL contexts/query names.
struct PresenterGpuQuery {GLuint names[2]{};unsigned stage=0;uint64_t epoch=0;bool pending=false,active=false;};
static thread_local std::array<PresenterGpuQuery,64> presenter_queries;
static thread_local EGLContext query_owner=EGL_NO_CONTEXT;
static void collect_presenter_gpu(){
 if(eglGetCurrentContext()!=query_owner || query_owner==EGL_NO_CONTEXT)return;
 for(auto &q:presenter_queries)if(q.pending){
  GLint available=0;glGetQueryObjectiv(q.names[1],GL_QUERY_RESULT_AVAILABLE,&available);if(!available)continue;
  GLuint64 start=0,end=0;glGetQueryObjectui64v(q.names[0],GL_QUERY_RESULT,&start);glGetQueryObjectui64v(q.names[1],GL_QUERY_RESULT,&end);
  if(q.epoch==cpu_epoch.load() && enabled() && end>=start){layer.gpu_tracked[q.stage]=true;layer.gpu[q.stage].fetch_add(end-start,std::memory_order_relaxed);}
  q.pending=false;
 }
}
static unsigned gpu_begin(unsigned stage){
 if(!enabled() || stage>=RBX_PROF_COUNT)return 0;
 EGLContext current=eglGetCurrentContext();if(current==EGL_NO_CONTEXT)return 0;
 if(query_owner==EGL_NO_CONTEXT){
  int major=0,minor=0;const char *version=reinterpret_cast<const char*>(glGetString(GL_VERSION));
  if(!version || sscanf(version,"%d.%d",&major,&minor)!=2 || major<3 || (major==3 && minor<3))return 0;
  query_owner=current;
 }
 if(current!=query_owner)return 0;collect_presenter_gpu();
 for(unsigned i=0;i<presenter_queries.size();++i){auto &q=presenter_queries[i];if(q.pending || q.active)continue;
  if(!q.names[0])glGenQueries(2,q.names);
  q.stage=stage;q.epoch=cpu_epoch.load();q.active=true;glQueryCounter(q.names[0],GL_TIMESTAMP);return i+1;
 }
 return 0;
}
static void gpu_end(unsigned token){
 if(!token || token>presenter_queries.size())return;
 auto &q=presenter_queries[token-1];if(!q.active)return;q.active=false;
 if(!enabled() || eglGetCurrentContext()!=query_owner)return;
 glQueryCounter(q.names[1],GL_TIMESTAMP);q.pending=true;
}
static void metric(unsigned id,double value){
 if(!enabled())return;
 if(id==RBX_PROF_GPU && std::isfinite(value) && value>=0 && value<1e9){layer.gpu_tracked[id]=true;layer.gpu[id].fetch_add(uint64_t(value*1e6),std::memory_order_relaxed);}
 std::unique_lock<std::mutex> lock(samples_mutex,std::try_to_lock);
 if(!lock){++dropped;return;}
 static thread_local uint32_t tid=syscall(SYS_gettid);
 model.record(id,value,now_ns(),tid);
}
static void frame(){
 if(!enabled())return;
 std::unique_lock<std::mutex> lock(samples_mutex,std::try_to_lock);
 if(lock)model.frame(now_ns());else ++dropped;
}
// Hotkeys are consumed by the host before the guest sees F6/F7/F8/F9. No mouse
// interception except the resize grip and scrolling over the visible HUD.
extern "C" bool rbx_profiler_key(unsigned scan,bool down,bool /*shift*/,bool repeat){
 static bool held[4]{};
 if(scan<64 || scan>66)return false; // SDL/USB F7..F9
 unsigned key=scan==65?1:scan==64?2:3; // F8 shows/hides, F7 freezes, F9 exports
 if(!down){bool consumed=held[key];held[key]=false;return consumed;}
 if(key!=1 && !shown.load() && !held[key])return false;
 held[key]=true;
 if(repeat)return true;
 ++panel_revision;
 if(key==1 || key==2){
  // Serialize visibility/epoch changes with span_end so a transition cannot
  // land between its final validity check and the ring write.
  {
   std::lock_guard<std::mutex> lock(samples_mutex);
   ++cpu_epoch;
  if(key==1){shown=!shown.load();if(shown.load())paused=false;}
   else paused=!paused.load();
  }
  snapshot_requested=true;
  if(!enabled())stop_collectors();
  else reset_capture=true;
  {std::lock_guard<std::mutex> lock(panel_mutex);panel.dragging=panel.moving=false;}
  std::lock_guard<std::mutex> lock(samples_mutex);model.last_frame=0;
 }
 if(key==3)save_requested=true;
 return true;
}
struct HotSymbol {std::string module,name,reason;uint64_t offset=0,region=0;unsigned category=3;};
static std::map<uint64_t,HotSymbol> symbol_cache;
static std::vector<CodeMapping> code_mappings;
static const HotSymbol &symbol_for(uint64_t ip){
 auto found=symbol_cache.find(ip);if(found!=symbol_cache.end()){++symbol_hits;return found->second;}
 ++symbol_misses;uint64_t start=now_ns();
 Dl_info info{};auto resolve=guest_symbolizer.load();
 // Resolve ELF/vDSO PCs without first traversing Darwin's loaded-image map.
 bool known=dladdr(reinterpret_cast<const void*>(ip),&info);
 // GTK has no Darwin TLS. Its HUD uses the existing mapped-image fallback;
 // guest dladdr remains available to the legacy game-context HUD.
 if(!known && !hosted.load())known=resolve && resolve(reinterpret_cast<const void*>(ip),&info);
 HotSymbol s;
 if(known && info.dli_fname){
  std::string path=info.dli_fname;auto slash=path.rfind('/');s.module=path.substr(slash==std::string::npos?0:slash+1);
  s.offset=ip-reinterpret_cast<uintptr_t>(info.dli_fbase);
  if(info.dli_sname)s.name=info.dli_sname;
  else s.reason="symbol not exported / stripped";
  if(s.name.rfind("_Z",0)==0){int status=0;char *name=abi::__cxa_demangle(s.name.c_str(),nullptr,nullptr,&status);if(name){if(status==0)s.name=name;free(name);}}
  s.region=info.dli_saddr?reinterpret_cast<uintptr_t>(info.dli_saddr)-reinterpret_cast<uintptr_t>(info.dli_fbase):s.offset&~uint64_t(4095);
  s.category=image_category(path,s.module);
 }else {
  const auto *m=find_mapping(code_mappings,ip);
  if(m){
   s.offset=ip-m->start+m->offset;s.region=s.offset&~uint64_t(4095);
   if(m->path.empty()){s.module="[anonymous "+m->permissions+"]";s.reason="anonymous mapping; no symbol registration";}
   else {auto slash=m->path.rfind('/');s.module=m->path.substr(slash==std::string::npos?0:slash+1);s.reason="mapped image; dladdr has no symbol";if(m->path[0]=='/')s.category=image_category(m->path,s.module);}
  }else {s.module="[unmapped at collection]";s.offset=ip;s.region=ip&~uint64_t(4095);s.reason="mapping absent or unloaded since sample";}
 }
 symbol_time_ns+=now_ns()-start;
 return symbol_cache.emplace(ip,std::move(s)).first->second;
}
static std::string symbol_label(const HotSymbol &s,bool region=false){
 char offset[40];snprintf(offset,sizeof(offset),"+0x%llx",(unsigned long long)(region?s.region:s.offset));
 return s.module+offset+(s.name.empty()?"  ["+s.reason+"]":"  "+s.name);
}
struct ProfileRanks {
 std::array<uint64_t,4> groups{};
 std::vector<std::pair<uint64_t,uint64_t>> guest;
 std::vector<std::pair<uint64_t,std::string>> regions,hosts;
 void update(const SamplingSnapshot &sampling){
  groups={};guest.clear();regions.clear();hosts.clear();std::map<std::string,uint64_t> region_counts,host_counts;
  for(const auto &[ip,v]:sampling.ips){const auto &s=symbol_for(ip);groups[s.category]+=v.count;
   if(s.category==0){guest.emplace_back(v.count,ip);region_counts[symbol_label(s,true)]+=v.count;}
   else host_counts["["+std::string(s.category==1?"runtime":s.category==2?"library":"unknown")+"] "+(s.name.empty()?symbol_label(s):s.module+"  "+s.name)]+=v.count;
  }
  groups[3]+=sampling.overflow;
  for(const auto &[label,count]:region_counts)regions.emplace_back(count,label);
  for(const auto &[label,count]:host_counts)hosts.emplace_back(count,label);
  std::sort(guest.rbegin(),guest.rend());std::sort(regions.rbegin(),regions.rend());std::sort(hosts.rbegin(),hosts.rend());
 }
};
static void json_string(FILE *f,const std::string &s){
 fputc('"',f);for(unsigned char c:s){if(c=='"' || c=='\\'){fputc('\\',f);fputc(c,f);}else if(c<32)fprintf(f,"\\u%04x",c);else fputc(c,f);}fputc('"',f);
}
static std::string export_trace(const ProfModel &data,const ResourceHistory &resources,const SamplingSnapshot &sampling,const WorkSnapshot &work){
 // JSON always uses decimal points. Keep GTK/Roblox's locale unchanged, including
 // on early errors; only this exporting thread switches for the duration.
 locale_t numeric=newlocale(LC_NUMERIC_MASK,"C",nullptr);
 if(!numeric)return "Export failed: numeric locale";
 locale_t previous=uselocale(numeric);
 if(!previous){freelocale(numeric);return "Export failed: thread locale";}
 struct RestoreLocale {locale_t previous,current;~RestoreLocale(){uselocale(previous);freelocale(current);}} restore{previous,numeric};
 const char *root=getenv("ROBLOX_MAC_DATA");if(!root || !*root)return "Export failed: data directory unavailable";
 std::string dir=std::string(root)+"/profiles";if(mkdir(dir.c_str(),0700) && errno!=EEXIST)return "Export failed: mkdir";
 std::string path=dir+"/compat-"+std::to_string(now_ns())+".json";
 int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(fd<0)return "Export failed: open";
 FILE *file=fdopen(fd,"w");if(!file){close(fd);return "Export failed: stream";}
 fprintf(file,"{\"displayTimeUnit\":\"ms\",\"overlay_drawn_during_capture\":%s,\"measurement_warning\":",work.had_overlay?"true":"false");
 json_string(file,work.had_overlay?"HUD was drawn during this capture; frame times include instrumentation and overlay overhead. Do not compare as a profiler-disabled FPS baseline.":"Instrumented diagnostic capture; no HUD drawn. Sampling/accounting overhead remains; not a profiler-disabled FPS baseline.");
 fprintf(file,",\"cpu_scope_timing\":%s",detailed_scopes?"true":"false");
 fputs(",\"traceEvents\":[",file);bool comma=false;
 for(uint64_t i=data.written>ProfModel::capacity?data.written-ProfModel::capacity:0;i<data.written;++i){
  const auto &e=data.events[i%ProfModel::capacity];
  // Values are observed durations at report time, not reconstructed start/end
  // spans. Counter events avoid falsely claiming an exact nested CPU timeline.
  fprintf(file,"%s{\"name\":\"%s\",\"ph\":\"C\",\"pid\":%d,\"tid\":%u,\"ts\":%.3f,\"args\":{\"%s\":%.6f}}",comma?",":"",prof_names[e.id],getpid(),e.tid,e.end/1000.,prof_counter(e.id)?"items":"duration_ms",e.value);comma=true;
 }
 for(uint64_t i=data.diagnostic_written>ProfModel::diagnostic_capacity?data.diagnostic_written-ProfModel::diagnostic_capacity:0;i<data.diagnostic_written;++i){
  const auto &e=data.diagnostic_events[i%ProfModel::diagnostic_capacity];
  fprintf(file,"%s{\"name\":\"%s\",\"ph\":\"X\",\"cat\":\"renderer_diagnostic\",\"pid\":%d,\"tid\":%u,\"ts\":%.3f,\"dur\":%.3f,\"args\":{\"cpu_ms\":%.6f,\"bytes\":%llu,\"object\":\"0x%llx\",\"detail\":%llu}}",
   comma?",":"",diag_names[e.id],getpid(),e.tid,e.start_ns/1000.,(e.end_ns-e.start_ns)/1000.,e.cpu_ns/1e6,
   (unsigned long long)e.bytes,(unsigned long long)e.object,(unsigned long long)e.detail);comma=true;
 }
 auto window=data.frameWindow(sampling.frame_end_ns);
 for(uint64_t i=window.first;i<window.last;++i){
  unsigned index=i%ProfModel::frame_capacity;
  fprintf(file,"%s{\"name\":\"CPU swap-return interval\",\"ph\":\"C\",\"pid\":%d,\"tid\":0,\"ts\":%.3f,\"args\":{\"duration_ms\":%.6f}}",comma?",":"",getpid(),data.frame_ends[index]/1000.,data.frames[index]);comma=true;
 }
 for(const auto &point:resources.points){
  if(std::isfinite(point.gpu)){
   fprintf(file,"%s{\"name\":\"Runner GPU utilization\",\"ph\":\"C\",\"pid\":%d,\"tid\":0,\"ts\":%.3f,\"args\":{\"percent\":%.6f}}",comma?",":"",getpid(),point.time/1000.,point.gpu);comma=true;
  }
  for(const auto &[core,value]:point.cpu)if(core>=0 && std::isfinite(value)){
   fprintf(file,"%s{\"name\":\"Layer CPU %d estimate\",\"ph\":\"C\",\"pid\":%d,\"tid\":0,\"ts\":%.3f,\"args\":{\"percent\":%.6f}}",comma?",":"",core,getpid(),point.time/1000.,value);comma=true;
  }
  fprintf(file,"%s{\"name\":\"Tracked layer host allocations\",\"ph\":\"C\",\"pid\":%d,\"tid\":0,\"ts\":%.3f,\"args\":{\"live_MiB\":%.6f}}",comma?",":"",getpid(),point.time/1000.,point.ram);comma=true;
 }
 fprintf(file,"],\"diagnostics\":{\"schema\":3,\"track\":\"A-native-x86_64\",\"ip_sample_period_ns\":%llu,\"samples\":%llu,\"lost\":%llu,\"ip_capacity_drops\":%llu,\"active_seconds\":%.6f,\"sampler_cpu_ms\":%.6f,\"category_cpu_estimates\":[",
  (unsigned long long)ip_sample_period_ns,(unsigned long long)sampling.samples,(unsigned long long)sampling.lost,(unsigned long long)sampling.overflow,sampling.seconds,sampling.worker_cpu_ms);
 std::array<uint64_t,4> category_samples{};
 for(const auto &[ip,v]:sampling.ips)category_samples[symbol_for(ip).category]+=v.count;
 category_samples[3]+=sampling.overflow;
 for(unsigned i=0;i<4;++i){
  fprintf(file,"%s{\"category\":%u,\"name\":",i?",":"",i);json_string(file,cpu_categories[i]);
  fprintf(file,",\"samples\":%llu,\"share_percent\":",(unsigned long long)category_samples[i]);
  if(sampling.samples)fprintf(file,"%.6f",100.*category_samples[i]/sampling.samples);else fputs("null",file);
  fputs(",\"estimated_cpu_ms\":",file);
  if(sampling.samples)fprintf(file,"%.6f",sampled_cpu_ms(category_samples[i]));else fputs("null",file);
  fputs(",\"estimated_cpu_ms_per_active_second\":",file);
  if(sampling.samples && sampling.seconds>0)fprintf(file,"%.6f",sampled_cpu_ms(category_samples[i])/sampling.seconds);else fputs("null",file);
  fputs(",\"estimated_cpu_ms_per_frame\":",file);
  if(sampling.samples && sampling.frame_count && !sampling.window_truncated)fprintf(file,"%.6f",sampled_cpu_ms_per_frame(category_samples[i],sampling.frame_count));else fputs("null",file);
  fprintf(file,",\"frame_count\":%llu",(unsigned long long)sampling.frame_count);
  fputc('}',file);
 }
 fprintf(file,"],\"frame_window_begin_ns\":%llu,\"frame_window_end_ns\":%llu,\"frame_window_count\":%llu,\"frame_window_truncated\":%s",
  (unsigned long long)sampling.frame_begin_ns,(unsigned long long)sampling.frame_end_ns,(unsigned long long)sampling.frame_count,sampling.window_truncated?"true":"false");
 fprintf(file,",\"renderer_diagnostics\":{\"capacity\":%u,\"retained\":%llu,\"written\":%llu,\"overwritten\":%llu,\"contention_drops\":%llu,\"first_start_ns\":%llu,\"last_end_ns\":%llu,\"clock\":\"CLOCK_MONOTONIC + CLOCK_THREAD_CPUTIME_ID\",\"clock_description\":\"wall ts/dur use CLOCK_MONOTONIC; cpu_ms is inclusive CLOCK_THREAD_CPUTIME_ID (user + kernel)\",\"limits\":\"bounded ring; retained events are capped at capacity and samples are dropped when the profiler model lock is busy\",\"cpu_note\":\"wall minus cpu is not an exact blocker or off-CPU measurement\"}",
  ProfModel::diagnostic_capacity,(unsigned long long)data.diagnostic_retained(),(unsigned long long)data.diagnostic_written,
  (unsigned long long)data.diagnostic_overwritten(),(unsigned long long)data.diagnostic_contention_drops,
  (unsigned long long)data.diagnostic_first_start(),(unsigned long long)data.diagnostic_last_end());
 fputs(",\"category_cpu_method\":\"Sample count times CPU sampling period; same completed frame window as graph; estimated user CPU across sampled threads, excluding kernel and lost samples; not frame latency.\",\"stages\":[",file);
 for(unsigned i=0;i<RBX_PROF_COUNT;++i){
  const auto &v=data.completedStats[i];fprintf(file,"%s{\"name\":",i?",":"");json_string(file,prof_names[i]);
  fprintf(file,",\"calls\":%llu,\"total_ms\":%.6f,\"max_ms\":%.6f,\"cpu_ms\":%.6f,\"gpu_ms\":%.6f,\"tracked_ram_mib\":%.6f,\"peak_mib\":%.6f,\"cpu_tracked\":%s,\"ram_tracked\":%s,\"gpu_tracked\":%s,\"cores\":",
   (unsigned long long)v.count,v.sum,v.max,resources.stage_cpu[i],resources.stage_gpu[i],resources.stage_ram[i],resources.stage_peak[i],resources.cpu_tracked[i]?"true":"false",resources.memory_tracked[i]?"true":"false",resources.gpu_tracked[i]?"true":"false");json_string(file,core_list(resources.stage_cores[i]));fputc('}',file);
 }
 fprintf(file,"],\"ips\":[");comma=false;
 for(const auto &[ip,v]:sampling.ips){const auto &symbol=symbol_for(ip);
  fprintf(file,"%s{\"ip\":\"0x%llx\",\"count\":%llu,\"category\":%u,\"module\":",comma?",":"",(unsigned long long)ip,(unsigned long long)v.count,symbol.category);json_string(file,symbol.module);
  fprintf(file,",\"offset\":%llu,\"symbol\":",(unsigned long long)symbol.offset);json_string(file,symbol.name);fprintf(file,",\"resolution\":");json_string(file,symbol.reason);fprintf(file,",\"cores\":{");bool sep=false;
  for(auto [core,count]:v.cores){fprintf(file,"%s\"%u\":%llu",sep?",":"",core,(unsigned long long)count);sep=true;}fprintf(file,"}}");comma=true;
 }
 fprintf(file,"],\"threads\":[");comma=false;
 for(const auto &t:sampling.threads){fprintf(file,"%s{\"tid\":%d,\"cpu_ms\":%.6f,\"core\":%d,\"state\":\"%c\",\"name\":",comma?",":"",t.tid,t.cpu_ms,t.core,t.state);json_string(file,t.name);fprintf(file,",\"wait\":");json_string(file,t.wait);fprintf(file,",\"user_ms\":%.6f,\"system_ms\":%.6f,\"samples\":%llu,\"top_ip\":\"0x%llx\",\"perf_error\":%d,\"attempts\":%u,\"proc_tid\":%d,\"attached\":%s",t.user_ms,t.system_ms,(unsigned long long)t.samples,(unsigned long long)t.top_ip,t.error,t.attempts,t.proc_tid,t.attached?"true":"false");fprintf(file,",\"runqueue_ms\":");if(std::isfinite(t.runqueue_ms))fprintf(file,"%.6f",t.runqueue_ms);else fputs("null",file);fputc('}',file);comma=true;}
 fprintf(file,"],\"covered_thread_cpu_ms\":%.6f,\"total_thread_cpu_ms\":%.6f,\"runner_cpu_window_ms\":",sampling.covered_cpu_ms,sampling.total_thread_cpu_ms);if(std::isfinite(resources.runner_cpu_ms))fprintf(file,"%.6f",resources.runner_cpu_ms);else fputs("null",file);
 fputs(",\"requested_egl_interval\":",file);json_string(file,presentation_policy());
 const auto &memory=sampling.memory;
 fprintf(file,",\"memory\":{\"available\":%s,\"sample_time_ns\":%llu,\"unit\":\"MB (1000000 bytes)\",\"method\":\"Linux smaps mapping RSS; RAM excludes GPU device mappings. Anonymous heaps have no guest/runtime owner; image totals exclude those heaps. GPU mappings are not a VRAM allocation total. UI allocations overlap RAM and exclude other profiler storage.\",\"profiler_ui_mb\":%.6f,\"profiler_ui_peak_mb\":%.6f",
  memory.valid?"true":"false",(unsigned long long)sampling.memory_time_ns,resources.points.empty()?0:resources.points.back().ram*1.048576,resources.ram_peak*1.048576);
 if(memory.valid){
  fprintf(file,",\"roblox_estimate_mb\":%.6f,\"roblox_identified_mb\":%.6f,\"roblox_estimate_method\":\"RSS minus macOS, compatibility, host-library/file and GPU mappings; equals Roblox files plus all unattributed heaps/stacks. Includes unidentified runtime/driver allocations; not an exact guest-only total.\"",memory.roblox_estimate(),memory.rss_mb[0]);
  fprintf(file,",\"rss_mb\":%.6f,\"ram_mb\":%.6f,\"ram_pss_mb\":%.6f,\"virtual_mb\":%.6f,\"gpu_mapped_mb\":%.6f,\"swap_mb\":%.6f,\"resident_categories\":[",
   memory.rss(),memory.ram(),memory.pss_ram_mb,memory.mapped_mb,memory.gpu_mapped_mb,memory.swap_mb);
  for(unsigned i=0;i<memory.rss_mb.size();++i){fprintf(file,"%s{\"name\":",i?",":"");json_string(file,memory_names[i]);fprintf(file,",\"rss_mb\":%.6f}",memory.rss_mb[i]);}
  fputc(']',file);
 }
 fputc('}',file);
 fprintf(file,",\"profiler_work\":{\"layout_builds\":%llu,\"cached_draws\":%llu,\"symbol_cache_hits\":%llu,\"symbol_cache_misses\":%llu,\"symbol_resolution_ms\":%.6f,\"overlay_visible\":%s},",(unsigned long long)work.layouts,(unsigned long long)work.draws,(unsigned long long)work.hits,(unsigned long long)work.misses,work.symbol_ns/1e6,work.overlay?"true":"false");
 fprintf(file,"\"timing_window_ms\":%.6f,\"thread_window_ms\":%.6f,\"sampler_error\":%d,\"timing_drops\":%llu}}\n",data.statWindow/1e6,sampling.window_ms,sampling.error,(unsigned long long)work.drops);int failed=ferror(file);if(fclose(file))failed=1;
 return failed?"Export failed: write":path;
}
static void draw(){
 if(!shown.load(std::memory_order_relaxed))return;
 NativeCpuScope cpu_scope(RBX_PROF_HUD);
 uint64_t start=now_ns();
 static ImGuiContext *context;
 static EGLContext owner;
 static ProfModel snapshot;
 static WorkSnapshot work;
 static uint64_t sampled=0;
 static ResourceHistory resources;
 static SamplingSnapshot sampling;static ProfileRanks ranks;
 static ProfModel::FrameWindow frame_window;
 static uint64_t capture_start=0;
 static std::array<ProfStats,RBX_PROF_COUNT> displayed{};
 static uint64_t displayed_at=0;
 static const std::vector<int> online=[] {
  std::vector<int> ids;std::ifstream file("/sys/devices/system/cpu/online");std::string list,item;std::getline(file,list);std::istringstream input(list);
  while(std::getline(input,item,',')){std::istringstream part(item);int first,last;if(!(part>>first))continue;last=first;if(part.peek()=='-'){part.get();part>>last;}
   for(int id=first;id<=last && id<int(LayerAccounting::cores);++id)if(id>=0)ids.push_back(id);
  }return ids;
 }();
 uint64_t now=now_ns();
 if(reset_capture.exchange(false)){
  stop_collectors();
  {std::lock_guard<std::mutex> lock(samples_mutex);diagnostic_contention_drops=0;model.resetCapture();model.diagnostic_contention_drops=0;snapshot=model;}
  sampling=SamplingSnapshot{};resources=ResourceHistory{};ranks=ProfileRanks{};frame_window={};
  symbol_cache.clear();code_mappings.clear();sampled=0;capture_start=now;
  capture_has_overlay=false;layout_builds=cached_draws=symbol_hits=symbol_misses=symbol_time_ns=0;
  dropped=0;work=work_now();displayed={};displayed_at=0;
 }
 {
 std::lock_guard<std::mutex> collectors_lock(collectors_mutex);
 if(enabled() && !sampler){sampler=std::make_unique<ProcessSampler>(enabled,0,true);gpu_usage=std::make_unique<ProcessGpuUsage>(enabled);}
 bool force_snapshot=snapshot_requested.exchange(false);
 if(force_snapshot || !sampled || (!paused.load() && now-sampled>250000000)){
  if(sampler && sampler->snapshot(sampling)){
   {std::lock_guard<std::mutex> lock(samples_mutex);model.diagnostic_contention_drops=diagnostic_contention_drops.load();snapshot=model;}
   work=work_now();
   frame_window=snapshot.frameWindow(sampling.through_ns);
   sampler->window(sampling,frame_window.begin,frame_window.end,frame_window.count());
   code_mappings=sampling.mappings;
   for(auto it=symbol_cache.begin();it!=symbol_cache.end();)if(it->second.category==3)it=symbol_cache.erase(it);else ++it;
   ranks.update(sampling);
   }else if(force_snapshot){
    // F7 stops collectors before this render pass; still copy the model's last
    // transition so the frozen export includes the final spans.
    std::lock_guard<std::mutex> lock(samples_mutex);
    model.diagnostic_contention_drops=diagnostic_contention_drops.load();snapshot=model;
    work=work_now(); // F7 must freeze current HUD metadata as well as the spans.
    frame_window=snapshot.frameWindow();
    sampling.frame_begin_ns=frame_window.begin;sampling.frame_end_ns=frame_window.end;sampling.frame_count=frame_window.count();
   }sampled=now;
 }
 // Rolling 1s window for the panel. The export keeps the tumbling
 // completedStats bucket so a capture's `stages` block stays a 1s aggregate.
 if(!paused.load() && (!displayed_at || now-displayed_at>=display_period_ns)){
  std::lock_guard<std::mutex> lock(samples_mutex);
  displayed=model.stats(now,1000000000);displayed_at=now;
 }
 if(!paused.load() && (resources.points.empty() || now-resources.points.back().time>=500000000)){
  collect_presenter_gpu();timespec process{};clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&process);
  resources.sample(now,layer,online,uint64_t(process.tv_sec)*1000000000+process.tv_nsec,gpu_usage?gpu_usage->percent():NAN);
 }
 } // collector ownership never escapes this render-thread snapshot
 static std::string saved;
 if(!capture_start)capture_start=now;
 bool auto_save=capture_duration_ns && now-capture_start>=capture_duration_ns;
 if(save_requested.exchange(false) || auto_save){
  // Export the same completed sampling/frame window as the visible graph.
  saved=export_trace(snapshot,resources,sampling,work);++panel_revision;
  if(auto_save){fprintf(stderr,"PROFILER capture: %s\n",saved.c_str());capture_duration_ns=0;{std::lock_guard<std::mutex> lock(samples_mutex);paused=true;++cpu_epoch;}stop_collectors();}
 }
 if(!shown.load())return;
 if(enabled())capture_has_overlay=true;
 if(!context){
  owner=eglGetCurrentContext();if(owner==EGL_NO_CONTEXT)return;
  ImGui::SetAllocatorFunctions(imgui_allocate,imgui_free);
  context=ImGui::CreateContext();ImGui::SetCurrentContext(context);
  ImGui::GetIO().IniFilename=nullptr;ImGui::GetIO().LogFilename=nullptr;
  ImGui::StyleColorsDark();auto &style=ImGui::GetStyle();style.WindowRounding=0;style.FrameRounding=0;
  style.WindowPadding=ImVec2(9,7);style.ItemSpacing=ImVec2(5,3);
  style.Colors[ImGuiCol_WindowBg]=ImVec4(.035f,.045f,.06f,.87f);style.Colors[ImGuiCol_Border]=ImVec4(.32f,.39f,.49f,.85f);
  style.Colors[ImGuiCol_Text]=ImVec4(.72f,.77f,.83f,1);style.Colors[ImGuiCol_TextDisabled]=ImVec4(.43f,.47f,.53f,1);
  if(!ImGui_ImplOpenGL3_Init("#version 130"))return;
 }
 if(eglGetCurrentContext()!=owner)return;
 ImGui::SetCurrentContext(context);

 GLint viewport[4];glGetIntegerv(GL_VIEWPORT,viewport);
 if(viewport[2]<1 || viewport[3]<1)return;
 static uint64_t layout_time=0,layout_revision=0;
 static int layout_width=0,layout_height=0;
 uint64_t revision=panel_revision.load();
 // Collectors still sample at 4/2 Hz; reuse the ImGui geometry between HUD
 // layouts. Drag/resize/scroll/key events and viewport changes rebuild immediately.
 bool reuse=display_period_ns && layout_time && now-layout_time<display_period_ns && revision==layout_revision && layout_width==viewport[2] && layout_height==viewport[3];
 if(reuse){
  ++cached_draws;unsigned gpu=gpu_begin(RBX_PROF_HUD);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());gpu_end(gpu);
  metric(RBX_PROF_HUD,(now_ns()-start)/1e6);return;
 }
 ++layout_builds;layout_time=now;layout_revision=revision;layout_width=viewport[2];layout_height=viewport[3];
 auto &io=ImGui::GetIO();io.DisplaySize=ImVec2(viewport[2],viewport[3]);io.DeltaTime=1.f/60;
 ImGui_ImplOpenGL3_NewFrame();ImGui::NewFrame();
 float scale=std::clamp(std::min(viewport[2]/1920.f,viewport[3]/1080.f),.85f,1.8f);io.FontGlobalScale=scale;

 float panel_width,panel_height,wheel,panel_x,panel_y;
 {
  std::lock_guard<std::mutex> lock(panel_mutex);
  panel_width=std::clamp(panel.width>0?panel.width:570*scale,360*scale,std::max(360*scale,viewport[2]-36*scale));
  panel_height=std::clamp(panel.height>0?panel.height:viewport[3]-100*scale,200*scale,std::max(200*scale,viewport[3]-90*scale));
  panel_x=std::clamp(panel.positioned?panel.x:30*scale,0.f,std::max(0.f,viewport[2]-panel_width));
  panel_y=std::clamp(panel.positioned?panel.y:38*scale,0.f,std::max(0.f,viewport[3]-panel_height));
  wheel=panel.scroll;panel.scroll=0;
 }
 ImGui::SetNextWindowPos(ImVec2(panel_x,panel_y),ImGuiCond_Always);
 ImGui::SetNextWindowSize(ImVec2(panel_width,panel_height));ImGui::SetNextWindowBgAlpha(.87f);
 ImGui::Begin("Custom Settings overlay",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove);
 if(wheel)ImGui::SetScrollY(std::max(0.f,ImGui::GetScrollY()-wheel*50*scale));
 ImVec2 panel_pos=ImGui::GetWindowPos(),panel_size=ImGui::GetWindowSize();
 {std::lock_guard<std::mutex> lock(panel_mutex);
  if(!panel.moving){panel.x=panel_pos.x;panel.y=panel_pos.y;}panel.title=ImGui::GetFrameHeight()+14;panel.view_width=viewport[2];panel.view_height=viewport[3];panel.grip=22*scale;
  if(!panel.dragging){panel.width=panel_size.x;panel.height=panel_size.y;}
 }
 ImVec4 blue(.52f,.78f,1,1),green(.56f,.91f,.65f,1),orange(1,.69f,.33f,1),purple(.66f,.65f,1,1),gray(.53f,.56f,.61f,1);
 auto section=[&](const char *label){ImGui::Spacing();ImGui::TextColored(blue,"%s",label);auto at=ImGui::GetCursorScreenPos();ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x,at.y-2*scale),ImVec2(at.x+ImGui::GetContentRegionAvail().x,at.y-2*scale),IM_COL32(74,91,115,180));};
 ImGui::TextColored(blue,"Custom Settings overlay");
 ImGui::SameLine(std::max(230*scale,panel_width-150*scale));ImGui::TextDisabled("F8 closes");
 unsigned n=frame_window.count();
 std::vector<float> frames;frames.reserve(n);double sum=0;float worst=0;
 for(uint64_t i=frame_window.first;i<frame_window.last;++i){float v=snapshot.frames[i%ProfModel::frame_capacity];frames.push_back(v);sum+=v;worst=std::max(worst,v);}
 auto sorted=frames;std::sort(sorted.begin(),sorted.end());
 auto percentile=[&](unsigned percent){return n?sorted[std::min<unsigned>(n-1,(n*percent+99)/100-1)]:0.f;};
 double mean=n?sum/n:0;
 section("FRAME");
 ImGui::Text("presented     %7.2f ms %6.1f fps   worst %7.2f ms",mean,mean?1000/mean:0,worst);
 auto p=ImGui::GetCursorScreenPos();float width=ImGui::GetContentRegionAvail().x,height=45*scale;
 auto *dl=ImGui::GetWindowDrawList();dl->AddRectFilled(p,ImVec2(p.x+width,p.y+height),IM_COL32(42,46,54,230));
 float ceiling=std::max(20.f,worst);
 for(unsigned i=0;i<n;++i){float x=p.x+i*width/std::max(1u,n),bar=frames[i]/ceiling*height;
  dl->AddRectFilled(ImVec2(x,p.y+height-bar),ImVec2(x+std::max(1.f,width/std::max(1u,n)),p.y+height),IM_COL32(255,178,87,255));
 }
 for(float budget:{4.167f,16.667f}){float y=p.y+height-height*budget/ceiling;dl->AddLine(ImVec2(p.x,y),ImVec2(p.x+width,y),IM_COL32(150,165,190,95));}
 ImGui::Dummy(ImVec2(width,height));
 ImGui::TextDisabled("p50 %.2f  p95 %.2f  p99 %.2f ms | %u frames",percentile(50),percentile(95),percentile(99),n);
 unsigned slow=0;double slow_run=0,longest=0;for(float v:frames){if(v>16.667){++slow;slow_run+=v;longest=std::max(longest,slow_run);}else slow_run=0;}
 ImGui::TextDisabled(">16.67ms %u | longest slow run %.0fms | target 4.17ms",slow,longest);
 ImGui::TextDisabled("CPU swap-return intervals; not display scanout timing.");
 ImGui::TextDisabled("Requested EGL interval: %s (not a scanout measurement)",presentation_policy());
 section("TIME PROFILING");
 ImGui::TextDisabled("Graph window: %.2fs | %u frames | estimated user CPU/frame",(frame_window.end-frame_window.begin)/1e9,n);
 if(sampling.window_truncated)ImGui::TextColored(orange,"Sample history truncated: ms/frame unavailable for this window.");
 if(!detailed_scopes)ImGui::TextColored(orange,"Sampling mode: CPU/draw scopes disabled; PC samples remain active.");
 const auto &groups=ranks.groups;const auto &guest=ranks.guest;
 ImVec4 colors[]={green,orange,purple,gray};
 for(unsigned i=0;i<4;++i){
  ImGui::Text("%-16s",cpu_categories[i]);ImGui::SameLine(175*scale);
  if(sampling.samples)ImGui::TextColored(colors[i],"%6.2f%%",100.*groups[i]/sampling.samples);else ImGui::TextDisabled("    --");
  ImGui::SameLine(265*scale);auto at=ImGui::GetCursorScreenPos();float w=std::max(1.f,ImGui::GetContentRegionAvail().x);
  dl->AddRectFilled(at,ImVec2(at.x+w,at.y+9*scale),IM_COL32(42,46,54,230));
  if(sampling.samples)dl->AddRectFilled(at,ImVec2(at.x+w*groups[i]/sampling.samples,at.y+9*scale),ImGui::ColorConvertFloat4ToU32(colors[i]));ImGui::Dummy(ImVec2(w,9*scale));
  if(sampling.samples && n && !sampling.window_truncated)ImGui::TextColored(colors[i],"  Est. CPU %7.3f ms/frame",sampled_cpu_ms_per_frame(groups[i],n));
  else ImGui::TextDisabled("  Est. CPU      -- ms/frame");
 }
 ImGui::TextDisabled("Sample estimates across threads; not frame latency.");
 ImGui::TextDisabled("Same completed frame window as graph. Kernel/lost samples excluded.");
 ImGui::Text("samples     %llu user-PC | active %.1fs",(unsigned long long)sampling.samples,sampling.seconds);
 ImGui::TextDisabled("sampler     %u/%zu threads | 100 Hz of CPU/thread",sampling.attached,sampling.threads.size());
 ImGui::TextDisabled("overhead    %.2f%% of a core | lost %llu | capped %llu",sampling.seconds?sampling.worker_cpu_ms/sampling.seconds/10:0,(unsigned long long)sampling.lost,(unsigned long long)sampling.overflow);
 ImGui::TextDisabled("states      %llu running / %llu sleeping observations",(unsigned long long)sampling.running,(unsigned long long)sampling.sleeping);
 if(sampling.total_thread_cpu_ms>0)ImGui::TextColored(sampling.denied?orange:green,"CPU coverage %.1f%% | %.1f / %.1f ms sampled threads",100*sampling.covered_cpu_ms/sampling.total_thread_cpu_ms,sampling.covered_cpu_ms,sampling.total_thread_cpu_ms);
 for(auto [error,count]:sampling.errors)ImGui::TextColored(orange,"%u threads: errno %d %s (retry 1s)",count,error,strerror(error));
 if(sampling.skipped)ImGui::TextColored(orange,"Thread limit: %u additional threads excluded",sampling.skipped);
 ImGui::TextDisabled("User CPU samples only; sleep observations are not time.");
 auto sample_values=[&](uint64_t count,ImVec4 color){
  ImGui::TextColored(color,"%5.2f%%",sampling.samples?100.*count/sampling.samples:0);ImGui::SameLine(68*scale);
  if(sampling.samples && n && !sampling.window_truncated)ImGui::TextColored(color,"%7.3f ms/frame",sampled_cpu_ms_per_frame(count,n));
  else ImGui::TextDisabled("     -- ms/frame");
  ImGui::SameLine(210*scale);
 };
 auto ranked=[&](const std::vector<std::pair<uint64_t,std::string>> &rows,unsigned limit,ImVec4 color){
  unsigned index=0;for(const auto &[count,label]:rows){if(index++>=limit)break;sample_values(count,color);ImGui::TextUnformatted(label.c_str());}
  if(rows.empty())ImGui::TextDisabled("No samples yet / sampling unavailable.");
 };
 section("GUEST REGIONS");ranked(ranks.regions,8,green);
 section("GUEST INSTRUCTIONS");
 ImGui::TextDisabled("Sampled PCs; stripped code uses module+offset.");
 unsigned index=0;
 for(auto [count,ip]:guest){if(index++>=8)break;sample_values(count,gray);ImGui::Text("0x%016llx  %s",(unsigned long long)ip,symbol_label(symbol_for(ip)).c_str());}
 if(guest.empty())ImGui::TextDisabled("No guest PCs available.");
 section("HOST SYMBOLS");ranked(ranks.hosts,12,purple);
 section("BRIDGE ENTRIES");
 auto stats=paused.load()?snapshot.completedStats:displayed;
 ImGui::TextDisabled("Track A has no CPU translator / region cache.");
 if(detailed_scopes)ImGui::Text("draw calls  %llu   descriptor binds %llu",(unsigned long long)stats[RBX_PROF_DRAW].count,(unsigned long long)stats[RBX_PROF_BINDINGS].count);
 if(detailed_scopes)ImGui::Text("submits     %llu   pipeline builds %llu",(unsigned long long)stats[RBX_PROF_SUBMIT].count,(unsigned long long)(stats[RBX_PROF_GRAPHICS_PIPELINE].count+stats[RBX_PROF_COMPUTE_PIPELINE].count));
 ImGui::Text("per submit  waits %.1f  signals %.1f  read %.1f  write %.1f",stats[RBX_PROF_WAITS].count?stats[RBX_PROF_WAITS].sum/stats[RBX_PROF_WAITS].count:0,stats[RBX_PROF_SIGNALS].count?stats[RBX_PROF_SIGNALS].sum/stats[RBX_PROF_SIGNALS].count:0,stats[RBX_PROF_READS].count?stats[RBX_PROF_READS].sum/stats[RBX_PROF_READS].count:0,stats[RBX_PROF_WRITES].count?stats[RBX_PROF_WRITES].sum/stats[RBX_PROF_WRITES].count:0);
 section("COMPATIBILITY STAGES");
 if(!detailed_scopes)ImGui::TextDisabled("CPU scopes and scope-call counts unavailable in sampling mode.");
 ImGui::TextDisabled("CPU: exclusive share of runner | wall: mean / max ms");
 std::vector<unsigned> order;for(unsigned i=0;i<RBX_PROF_COUNT;++i)if(!prof_counter(i))order.push_back(i);
 std::stable_sort(order.begin(),order.end(),[&](unsigned a,unsigned b){return resources.stage_cpu[a]>resources.stage_cpu[b];});
 if(ImGui::BeginTable("stages",5,ImGuiTableFlags_SizingStretchProp)){
  ImGui::TableSetupColumn("CPU %",0,.65);ImGui::TableSetupColumn("Stage",0,2.4);ImGui::TableSetupColumn("Mean",0,.65);ImGui::TableSetupColumn("Max",0,.65);ImGui::TableSetupColumn("Cores",0,1.4);ImGui::TableHeadersRow();
  for(unsigned i:order){ImGui::TableNextRow();ImGui::TableNextColumn();double cpu=runner_share(resources.stage_cpu[i],resources.runner_cpu_ms);
   if(resources.cpu_tracked[i] && std::isfinite(cpu))ImGui::TextColored(orange,"%.2f",cpu);else ImGui::TextDisabled("--");
   ImGui::TableNextColumn();ImGui::TextUnformatted(prof_names[i]);
   ImGui::TableNextColumn();if(stats[i].count)ImGui::Text("%.3f",stats[i].sum/stats[i].count);else ImGui::TextDisabled("--");
   ImGui::TableNextColumn();if(stats[i].count)ImGui::Text("%.3f",stats[i].max);else ImGui::TextDisabled("--");
   ImGui::TableNextColumn();ImGui::TextWrapped("%s",core_list(resources.stage_cores[i]).c_str());
  }ImGui::EndTable();
 }
 ImGui::TextDisabled("Wall spans overlap; asynchronous delays are not CPU.");
 section("THREADS");ImGui::TextDisabled("Latest %.0fms | CPU time / last core / state / wait",sampling.window_ms);
 index=0;for(const auto &t:sampling.threads){if(index++>=16)break;
  ImGui::Text("%5d %5.1f%% C%-3d %c  %s",t.tid,sampling.window_ms>0?t.cpu_ms/sampling.window_ms*100:0,t.core,t.state,t.name.c_str());
  ImGui::TextDisabled("      user %.1f / kernel %.1f ms | %llu PCs",t.user_ms,t.system_ms,(unsigned long long)t.samples);
  if(std::isfinite(t.runqueue_ms))ImGui::TextDisabled("      runnable scheduler wait %.3f ms",t.runqueue_ms);
  if(t.proc_tid!=t.tid)ImGui::TextDisabled("      procfs TID %d -> sampler TID %d",t.proc_tid,t.tid);
  if(t.top_ip)ImGui::TextWrapped("      hot %s",symbol_label(symbol_for(t.top_ip)).c_str());
  if(!t.attached)ImGui::TextColored(orange,"      perf errno %d: %s | attempts %u",t.error,strerror(t.error),t.attempts);
  if(!t.wait.empty() && t.wait!="0")ImGui::TextDisabled("      wait: %s",t.wait.c_str());
  else if(t.state=='S' || t.state=='D')ImGui::TextDisabled("      wait symbol restricted / not exposed by kernel");
 }
 if(!resources.points.empty()){
  const auto &latest=resources.points.back();
  section("CPU CORES - COMPATIBILITY SCOPES ONLY");
  if(detailed_scopes){
  unsigned core_index=0,columns=std::clamp(int(width/(100*scale)),1,8);
  for(int id:online){
   if(core_index%columns)ImGui::SameLine((core_index%columns)*width/columns);
   auto found=latest.cpu.find(id);double percent=found==latest.cpu.end()?NAN:found->second;
   if(std::isfinite(percent))ImGui::TextColored(blue,"CPU %d: %.1f%%",id,percent);else ImGui::TextDisabled("CPU %d: --",id);
   ++core_index;
  }
  ImGui::TextDisabled("Migrated CPU %.3fms unassigned; boundary estimates.",resources.migrated_ms);
  }else ImGui::TextDisabled("Per-core scope attribution disabled in sampling mode; see sampled thread PCs above.");
  section("GPU");
  ImGui::TextWrapped("%s",reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
  if(std::isfinite(latest.gpu))ImGui::TextColored(ImVec4(1,.42f,.7f,1),"runner usage %.1f%%",latest.gpu);else ImGui::TextDisabled("Per-process GPU utilization unavailable.");
  for(unsigned i:{unsigned(RBX_PROF_GPU),unsigned(RBX_PROF_COMPOSITE),unsigned(RBX_PROF_HUD)}){
   double share=runner_share(resources.stage_gpu[i],resources.total_gpu_ms);
   if(resources.gpu_tracked[i] && std::isfinite(share))ImGui::Text("%6.2f%% %8.3fms  %s",share,resources.stage_gpu[i],prof_names[i]);
  }
  ImGui::TextDisabled("Shares of timed GPU work; not full-frame GPU timing.");
  section("MEMORY");
  const auto &memory=sampling.memory;
  if(memory.valid){
   ImGui::TextColored(ImVec4(.48f,.85f,.42f,1),"Roblox RAM estimate  %.2f MB",memory.roblox_estimate());
   ImGui::Text("Identified Roblox libraries/assets %.2f MB",memory.rss_mb[0]);
   ImGui::TextColored(ImVec4(.73f,.49f,.96f,1),"Roblox + runtime RAM  %.2f MB",memory.ram());
   ImGui::Text("Total mapped RSS      %.2f MB",memory.rss());
   for(unsigned i=0;i<memory.rss_mb.size();++i)ImGui::Text("%9.2f MB  %s",memory.rss_mb[i],memory_names[i]);
   ImGui::Text("RAM proportional share %.2f MB | swap %.2f MB",memory.pss_ram_mb,memory.swap_mb);
   ImGui::Text("Virtual address space %.2f MB",memory.mapped_mb);
   ImGui::Text("GPU mapped address space %.2f MB",memory.gpu_mapped_mb);
  }else{
   ImGui::TextDisabled("Memory breakdown unavailable (smaps unreadable / incomplete).");
   if(std::isfinite(sampling.rss_mib))ImGui::Text("Raw runner RSS %.2f MB (may include GPU mappings)",sampling.rss_mib*1.048576);
  }
  ImGui::TextColored(ImVec4(.73f,.49f,.96f,1),"Profiler UI allocations %.2f MB | peak %.2f MB",latest.ram*1.048576,resources.ram_peak*1.048576);
  ImGui::TextWrapped("MB = 1,000,000 bytes; refreshed every second. RAM excludes GPU device mappings; mapped address space is not physical usage or total VRAM.");
  ImGui::TextWrapped("Roblox estimate = total RSS minus macOS, compatibility, host-library/file and GPU mappings. It retains all unattributed heaps/stacks, including unidentified runtime/driver allocations. Library rows exclude those heaps; UI allocations overlap RAM and exclude other profiler storage.");
 }
 section("UNATTRIBUTED WORK");
 double tracked_cpu=0;for(double ms:resources.stage_cpu)tracked_cpu+=ms;
 if(std::isfinite(resources.runner_cpu_ms))ImGui::Text("runner %.2f / scoped layer %.2f / residual %.2f CPU ms",resources.runner_cpu_ms,tracked_cpu,std::max(0.,resources.runner_cpu_ms-tracked_cpu));
 ImGui::TextDisabled("Residual includes guest, native threads and uninstrumented code.");
 unsigned unresolved=0;for(const auto &[ip,v]:sampling.ips){const auto &sym=symbol_for(ip);if(sym.category==3 && unresolved++<6)ImGui::TextWrapped("%llu PCs: %s",(unsigned long long)v.count,symbol_label(sym).c_str());}
 ImGui::TextDisabled("Stripped/anonymous code cannot have invented function names.");
 ImGui::TextDisabled("Scheduler wait is shown only where kernel schedstat supplies it.");
 section("CAPTURE HEALTH");
 ImGui::Text("HUD layouts %llu / cached presents %llu (10 Hz layout)",(unsigned long long)layout_builds,(unsigned long long)cached_draws);
 ImGui::Text("Symbol cache: %llu hits / %llu misses; %.2f ms resolving",(unsigned long long)symbol_hits,(unsigned long long)symbol_misses,symbol_time_ns/1e6);
 ImGui::Text("retained %llu/%u | overwritten %llu | drops %llu",(unsigned long long)std::min<uint64_t>(snapshot.written,ProfModel::capacity),ProfModel::capacity,(unsigned long long)(snapshot.written>ProfModel::capacity?snapshot.written-ProfModel::capacity:0),(unsigned long long)dropped.load());
 ImGui::TextDisabled("PC sampling: graph window, bounded 8192 PCs / 131072 samples.");
 ImGui::TextDisabled("F7 %s | F9 export all rows/PCs/threads + frame trace",paused?"resume":"freeze");
 ImGui::TextDisabled("Drag header / resize corner / wheel scrolls. Hide for benchmarks.");
 if(!saved.empty())ImGui::TextWrapped("%s",saved.c_str());
 // Draw outside the scrolling content clip so the resize grip stays reachable.
 dl->PushClipRect(panel_pos,ImVec2(panel_pos.x+panel_size.x,panel_pos.y+panel_size.y),false);
 ImVec2 corner(panel_pos.x+panel_size.x-3*scale,panel_pos.y+panel_size.y-3*scale);
 dl->AddTriangleFilled(corner,ImVec2(corner.x-18*scale,corner.y),ImVec2(corner.x,corner.y-18*scale),IM_COL32(95,180,245,255));
 dl->PopClipRect();
 ImGui::End();ImGui::Render();unsigned hud_gpu=gpu_begin(RBX_PROF_HUD);
 ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());gpu_end(hud_gpu);
 metric(RBX_PROF_HUD,(now_ns()-start)/1e6);
}
extern "C" const RbxProfilerAPI *rbx_profiler_api(){
 static const bool init=[] {
  const char *mode=getenv("ROBLOX_MAC_PROFILER");
  // Per-draw thread CPU clocks cross the Darling boundary thousands of times
  // per frame. Use the existing PC sampler by default; exact scopes are opt-in.
  detailed_scopes=mode && !strcmp(mode,"detailed");
  shown=mode && (!strcmp(mode,"1") || !strcmp(mode,"record") || !strcmp(mode,"sample") || detailed_scopes);
  if(const char *value=getenv("ROBLOX_MAC_PROFILER_HZ")){char *end=nullptr;double hz=strtod(value,&end);
   if(end!=value && !*end && std::isfinite(hz) && hz>=0 && hz<=1000)display_period_ns=hz>0?uint64_t(1e9/hz):0;}
  if(const char *value=getenv("ROBLOX_MAC_PROFILER_CAPTURE_SECONDS")){char *end=nullptr;double seconds=strtod(value,&end);if(end!=value && !*end && std::isfinite(seconds) && seconds>=1 && seconds<=3600)capture_duration_ns=uint64_t(seconds*1e9);}
  return true;}();(void)init;
 static const RbxProfilerAPI api={8,enabled,metric,frame,draw,cpu_begin,cpu_end,allocator,gpu_begin,gpu_end,set_symbolizer,
  []{return int(shown.load(std::memory_order_relaxed));},span_begin,span_end,
  []{return int(hosted.load(std::memory_order_relaxed));}};return &api;
}
