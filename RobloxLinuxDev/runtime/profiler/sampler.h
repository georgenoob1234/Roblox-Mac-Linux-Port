#pragma once
#include <pthread.h>
#include "mappings.h"
#include <linux/perf_event.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <dirent.h>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <map>
#include <memory>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <chrono>
#include <deque>

inline constexpr uint64_t ip_sample_period_ns=10000000;
// Statistical CPU-time estimate, not elapsed frame time or a per-call timer.
inline double sampled_cpu_ms(uint64_t count){return double(count)*(ip_sample_period_ns/1e6);}

struct ThreadReading {uint64_t ticks=0,user=0,system=0,born=0;char state='?';int core=-1;std::string name;};
// procfs may belong to an ancestor PID namespace. perf_event_open needs the
// caller's namespace TID, not the directory name from that procfs mount.
inline int namespace_tid(const std::string &status,int fallback){
 std::istringstream input(status);std::string line;
 while(std::getline(input,line))if(line.rfind("NSpid:",0)==0){std::istringstream ids(line.substr(6));int id,last=0;while(ids>>id)last=id;return last>0?last:fallback;}
 return fallback;
}
inline bool thread_reading(const std::string &line,ThreadReading &out){
 auto begin=line.find('('),end=line.rfind(')');if(begin==std::string::npos || end==std::string::npos || end<=begin)return false;
 out.name=line.substr(begin+1,end-begin-1);std::istringstream in(line.substr(end+2));std::vector<std::string> fields;std::string value;
 while(in>>value)fields.push_back(value);if(fields.size()<37)return false;
 try{out.state=fields[0][0];out.user=std::stoull(fields[11]);out.system=std::stoull(fields[12]);out.ticks=out.user+out.system;out.born=std::stoull(fields[19]);out.core=std::stoi(fields[36]);}catch(...){return false;}return true;
}
struct IpReading {uint64_t count=0;std::map<unsigned,uint64_t> cores;};
struct ThreadHotspot {int tid=0,core=-1;double cpu_ms=0;char state='?';std::string name,wait;
 int proc_tid=0;
 double user_ms=0,system_ms=0,runqueue_ms=NAN;uint64_t samples=0,top_ip=0,top_count=0;int error=0;unsigned attempts=0;bool attached=false;
};
struct SamplingSnapshot {
 std::map<uint64_t,IpReading> ips;
 std::vector<ThreadHotspot> threads;
 std::vector<CodeMapping> mappings;std::map<int,unsigned> errors;
 double covered_cpu_ms=0,total_thread_cpu_ms=0;unsigned skipped=0;
 uint64_t samples=0,lost=0,overflow=0,running=0,sleeping=0,sweeps=0;
 double seconds=0,worker_cpu_ms=0,window_ms=0,rss_mib=NAN;
 MemoryReading memory;uint64_t memory_time_ns=0;
 unsigned attached=0,denied=0;int error=0;
 uint64_t through_ns=0,frame_count=0,frame_begin_ns=0,frame_end_ns=0;bool window_truncated=false;
};
// Each entry is a sampled userspace PC, not a call count or blocked-time sample.
inline void sample_ip(SamplingSnapshot &s,uint64_t ip,unsigned core){
 ++s.samples;auto found=s.ips.find(ip);
 // ponytail: first 8192 distinct PCs retained; capped samples remain visible
 // as unaccounted. Use a bounded heavy-hitter sketch for longer captures.
 if(found==s.ips.end() && s.ips.size()>=8192){++s.overflow;return;}
 auto &v=s.ips[ip];++v.count;if(core<256)++v.cores[core];
}
inline double sampled_cpu_ms_per_frame(uint64_t samples,uint64_t frames){return frames?sampled_cpu_ms(samples)/frames:NAN;}
struct TimedIp {uint64_t time,ip;unsigned core;};
struct SampleHistory {
 // Bounded visible-only history. Report insufficient coverage instead of inventing timing.
 static constexpr size_t capacity=131072;
 std::deque<TimedIp> events;uint64_t evicted_through=0;
 void add(TimedIp event){events.push_back(event);if(events.size()>capacity){evicted_through=std::max(evicted_through,events.front().time);events.pop_front();}}
 void window(SamplingSnapshot &out,uint64_t begin,uint64_t end){
  out.ips.clear();out.samples=out.overflow=0;out.frame_begin_ns=begin;out.frame_end_ns=end;
  out.window_truncated=begin<evicted_through;
  for(const auto &e:events)if(e.time>begin && e.time<=end)sample_ip(out,e.ip,e.core);
  // Threads are drained separately, so timestamps are not globally sorted.
  while(!events.empty() && events.front().time<=begin)events.pop_front();
 }
};
class ProcessSampler {
 struct Ring {int fd=-1;void *mapping=MAP_FAILED;size_t size=0;uint64_t ticks=0,user=0,system=0,born=0,retry=0;bool baseline=false;int error=0;unsigned attempts=0;
  uint64_t runqueue=0;bool scheduler_baseline=false;
 std::map<uint64_t,uint64_t> recent;
  ~Ring(){if(mapping!=MAP_FAILED)munmap(mapping,size);if(fd>=0)close(fd);}
 };
 std::mutex mutex;std::condition_variable wake;bool stop=false;
 SamplingSnapshot published;
 SampleHistory history;
 // The worker can publish immediately; construct its destination first.
 std::thread worker;
 static uint64_t clock_ns(clockid_t id){timespec t{};clock_gettime(id,&t);return uint64_t(t.tv_sec)*1000000000+t.tv_nsec;}
 static std::string read_line(const std::string &path){std::ifstream f(path);std::string s;std::getline(f,s);return s;}
 static std::string read_status(const std::string &path){std::ifstream f(path);return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};}
 static void drain(Ring &ring,SamplingSnapshot &s,std::vector<TimedIp> *batch){
  if(ring.mapping==MAP_FAILED)return;
  auto *meta=static_cast<perf_event_mmap_page*>(ring.mapping);
  uint64_t head=__atomic_load_n(&meta->data_head,__ATOMIC_ACQUIRE),tail=meta->data_tail;
  size_t bytes=meta->data_size,offset=meta->data_offset;
  if(!bytes || offset>ring.size || bytes>ring.size-offset)return;
  const char *data=static_cast<char*>(ring.mapping)+offset;
  auto copy=[&](void *dst,size_t n,uint64_t pos){size_t at=pos%bytes,first=std::min(n,bytes-at);memcpy(dst,data+at,first);if(n>first)memcpy(static_cast<char*>(dst)+first,data,n-first);};
  if(head<tail || head-tail>bytes){++s.lost;tail=head;}
  while(head-tail>=sizeof(perf_event_header)){
   perf_event_header h;copy(&h,sizeof(h),tail);
   if(h.size<sizeof(h) || h.size>bytes || h.size>head-tail){++s.lost;tail=head;break;}
   // PERF_SAMPLE_IP | TID | TIME | CPU, using CLOCK_MONOTONIC like presentation.
   if(h.type==PERF_RECORD_SAMPLE && h.size>=40){uint64_t ip,time;unsigned cpu;copy(&ip,8,tail+8);copy(&time,8,tail+24);copy(&cpu,4,tail+32);
    // Rolling mode counts timestamped history at snapshot time. Maintaining
    // and copying a second cumulative IP/core map only to discard it is waste.
    if(batch)batch->push_back({time,ip,cpu});else sample_ip(s,ip,cpu);
    if(ring.recent.size()<256 || ring.recent.count(ip))++ring.recent[ip];}
   else if(h.type==PERF_RECORD_LOST && h.size>=24){uint64_t lost;copy(&lost,8,tail+16);s.lost+=lost;}
   tail+=h.size;
  }
  __atomic_store_n(&meta->data_tail,tail,__ATOMIC_RELEASE);
 }
 void run(int (*active)(),int target,bool windowed){
  pthread_setname_np(pthread_self(),"rbx-prof-cpu");
  const std::string process=target>0?"/proc/"+std::to_string(target):"/proc/self";
  std::map<int,std::unique_ptr<Ring>> rings;SamplingSnapshot s;
  uint64_t previous=0;long hz=sysconf(_SC_CLK_TCK),page=sysconf(_SC_PAGESIZE);int self=syscall(SYS_gettid);
  while(true){
   std::vector<TimedIp> batch;
   auto cpu_start=clock_ns(CLOCK_THREAD_CPUTIME_ID),now=clock_ns(CLOCK_MONOTONIC);
   if(active()){
    double elapsed=previous?(now-previous)/1e6:0;previous=now;s.seconds+=elapsed/1000.;s.window_ms=elapsed;s.threads.clear();s.attached=s.denied=s.skipped=0;s.error=0;s.errors.clear();s.covered_cpu_ms=s.total_thread_cpu_ms=0;
    std::map<int,bool> alive;
    if(DIR *dir=opendir((process+"/task").c_str())){
     while(auto *entry=readdir(dir)){
      char *end;long number=strtol(entry->d_name,&end,10);if(*end || number<=0)continue;if(alive.size()>=256){++s.skipped;continue;}
      std::string root=process+"/task/"+std::to_string(number)+"/";
      int tid=target>0?int(number):namespace_tid(read_status(root+"status"),number);if(tid==self)continue;alive[tid]=true;
      ThreadReading r;if(!thread_reading(read_line(root+"stat"),r))continue;
      auto &slot=rings[tid];
      if(slot && slot->born!=r.born)slot.reset();
      if(!slot){slot=std::make_unique<Ring>();slot->born=r.born;}
      if(slot->fd<0 && now>=slot->retry){
       ++slot->attempts;slot->retry=now+1000000000;perf_event_attr attr{};attr.size=sizeof(attr);attr.type=PERF_TYPE_SOFTWARE;attr.config=PERF_COUNT_SW_CPU_CLOCK;
       attr.sample_period=ip_sample_period_ns;attr.sample_type=PERF_SAMPLE_IP|PERF_SAMPLE_TID|PERF_SAMPLE_TIME|PERF_SAMPLE_CPU;attr.exclude_kernel=1;attr.exclude_hv=1;attr.wakeup_events=1;attr.use_clockid=1;attr.clockid=CLOCK_MONOTONIC;
       slot->fd=syscall(SYS_perf_event_open,&attr,tid,-1,-1,PERF_FLAG_FD_CLOEXEC);
       if(slot->fd>=0){slot->size=page*5;slot->mapping=mmap(nullptr,slot->size,PROT_READ|PROT_WRITE,MAP_SHARED,slot->fd,0);if(slot->mapping==MAP_FAILED){slot->error=errno;close(slot->fd);slot->fd=-1;}}
       else slot->error=errno;
       if(slot->fd>=0)slot->error=0;
      }
      auto &ring=*slot;ring.recent.clear();if(ring.fd>=0){++s.attached;drain(ring,s,windowed?&batch:nullptr);}else {++s.denied;++s.errors[ring.error];s.error=ring.error;}
      double ms=ring.baseline && elapsed>0 && r.ticks>=ring.ticks ? (r.ticks-ring.ticks)*1000./hz:0;
      double user_ms=ring.baseline && r.user>=ring.user?(r.user-ring.user)*1000./hz:0;
      double system_ms=ring.baseline && r.system>=ring.system?(r.system-ring.system)*1000./hz:0;
      ring.ticks=r.ticks;ring.user=r.user;ring.system=r.system;ring.baseline=true;
      s.total_thread_cpu_ms+=ms;if(ring.fd>=0)s.covered_cpu_ms+=ms;
      if(r.state=='R')++s.running;else if(r.state=='S' || r.state=='D')++s.sleeping;
      ThreadHotspot t{tid,r.core,ms,r.state,r.name,r.state=='S'||r.state=='D'?read_line(root+"wchan"):""};
      t.proc_tid=number;t.user_ms=user_ms;t.system_ms=system_ms;t.error=ring.error;t.attempts=ring.attempts;t.attached=ring.fd>=0;
      std::istringstream scheduler(read_line(root+"schedstat"));uint64_t runtime,delay,slices;
      if(scheduler>>runtime>>delay>>slices){
       if(delay && ring.scheduler_baseline && delay>=ring.runqueue)t.runqueue_ms=(delay-ring.runqueue)/1e6;
       ring.runqueue=delay;ring.scheduler_baseline=true;
      }
      for(auto [ip,count]:ring.recent){t.samples+=count;if(count>t.top_count){t.top_count=count;t.top_ip=ip;}}
      s.threads.push_back(std::move(t));
     }closedir(dir);
    }
    for(auto it=rings.begin();it!=rings.end();)if(!alive.count(it->first)){drain(*it->second,s,windowed?&batch:nullptr);it=rings.erase(it);}else ++it;
    s.mappings.clear();std::ifstream maps(process+"/maps");std::string line;
    while(std::getline(maps,line)){CodeMapping m;if(parse_mapping(line,m))s.mappings.push_back(std::move(m));}
    std::istringstream memory(read_line(process+"/statm"));uint64_t total,resident;s.rss_mib=NAN;if(memory>>total>>resident)s.rss_mib=resident*double(page)/1048576.;
    // ponytail: one smaps scan per second on the existing worker; no heap hooks.
    // Add allocator ownership tracking only if a guest-only heap total is needed.
    if(!s.memory_time_ns || now-s.memory_time_ns>=1000000000){
     FILE *smaps=fopen((process+"/smaps").c_str(),"re");s.memory=read_memory(smaps);if(smaps)fclose(smaps);s.memory_time_ns=now;
    }
    std::sort(s.threads.begin(),s.threads.end(),[](const auto &a,const auto &b){return a.cpu_ms>b.cpu_ms;});++s.sweeps;
    {std::lock_guard<std::mutex> lock(mutex);for(auto e:batch)history.add(e);s.through_ns=now;published=s;s.worker_cpu_ms+=(clock_ns(CLOCK_THREAD_CPUTIME_ID)-cpu_start)/1e6;published.worker_cpu_ms=s.worker_cpu_ms;}
   }else {rings.clear();previous=0;}
   std::unique_lock<std::mutex> lock(mutex);if(wake.wait_for(lock,std::chrono::milliseconds(250),[this]{return stop;}))break;
  }
 }
 public:
 explicit ProcessSampler(int (*active)(),int target=0,bool windowed=false):worker([this,active,target,windowed]{run(active,target,windowed);}){}
 ~ProcessSampler(){{std::lock_guard<std::mutex> lock(mutex);stop=true;}wake.notify_one();worker.join();}
 bool snapshot(SamplingSnapshot &out){std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);if(!lock)return false;out=published;return true;}
 void window(SamplingSnapshot &out,uint64_t begin,uint64_t end,uint64_t frames){std::lock_guard<std::mutex> lock(mutex);history.window(out,begin,end);out.frame_count=frames;}
};
