#pragma once
#include "api.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
static constexpr const char *prof_names[]={
 "Commit (inclusive)","Queue mutex wait","Submit -> scheduled","Submit -> completion",
 "waitUntilCompleted","Readback wait","Present enqueue","Present dispatch delay",
 "Commit: pre-submit","vkQueueSubmit2","Commit: post-submit","Resource preparation",
 "Upload preparation","End command buffer","Sync preparation","Wait semaphores",
 "Signal semaphores","Read images","Written images","GPU command execution",
 "Shader translation","Graphics pipeline compile","Compute pipeline compile",
 "GTK bridge round trip","Layer composition","Swap call","Profiler draw CPU","Draw encoding (inclusive)","Descriptor bindings","Pipeline readiness wait",
 "Upload staging memcpy","Upload copy bytes (counter)","Upload staging allocation/map",
 "Upload vkQueueSubmit","Upload fence wait","Upload queue mutex wait",
 "Blit buffer to texture encoding","Compute dispatch encoding (inclusive)","Compute descriptor bindings",
 "Descriptor cache hits", "Descriptor sets allocated", "Descriptor sets rewritten",
 "Descriptor arenas created", "Descriptor arenas oversized", "Descriptor arenas overflowed",
 "Texture destroy", "Buffer destroy"};
static_assert(sizeof(prof_names)/sizeof(*prof_names)==RBX_PROF_COUNT);
static constexpr const char *diag_names[]={
 "buffer create", "buffer vk create", "buffer pool lock", "buffer allocate",
 "buffer map", "buffer bind", "buffer copy", "buffer flush", "buffer blit",
 "command create", "command commit", "completion register", "completion lock",
 "completion wake", "command wait", "completion semaphore", "semaphore pool lock",
 "semaphore create", "texture acquire", "command sync", "queue submit", "queue lock",
 "command pool lock", "command pool create", "command begin", "query setup"};
static_assert(sizeof(diag_names)/sizeof(*diag_names)==RBX_DIAG_COUNT);
static bool prof_counter(unsigned id){return (id>=RBX_PROF_WAITS && id<=RBX_PROF_WRITES) || id==RBX_PROF_UPLOAD_COPY_BYTES || (id>=RBX_PROF_DESCRIPTOR_HITS && id<=RBX_PROF_ARENA_OVERFLOW);}
struct ProfSample {uint64_t end;double value;uint32_t tid,id;};
struct ProfDiagnostic {
 uint64_t start_ns,end_ns,cpu_ns,bytes,object,detail;
 uint32_t tid,id;
};
struct ProfStats {uint64_t count=0;double sum=0,max=0;void add(double v){++count;sum+=v;max=std::max(max,v);}};
struct ProfModel {
 struct FrameWindow {uint64_t first=0,last=0,begin=0,end=0;uint64_t count()const{return last-first;}};
 // ponytail: fixed 14 MiB diagnostic ring, 32x the old retention. Burst rates
 // can still outrun it; export reports overwrites and retained timestamps.
 // Keep models off the stack. Timing events remain in their separate ring.
 static constexpr unsigned capacity=65536,frame_capacity=600,diagnostic_capacity=262144;
 std::array<ProfSample,capacity> events{};
 std::array<ProfDiagnostic,diagnostic_capacity> diagnostic_events{};
 std::array<float,frame_capacity> frames{};
 std::array<uint64_t,frame_capacity> frame_ends{};
 std::array<uint64_t,frame_capacity> frame_starts{};
 uint64_t written=0,diagnostic_written=0,diagnostic_contention_drops=0,frame_written=0,last_frame=0;
 std::array<ProfStats,RBX_PROF_COUNT> currentStats{},completedStats{};
 uint64_t epoch=0,statEnd=0,statWindow=0;
 void resetCapture(){written=diagnostic_written=diagnostic_contention_drops=frame_written=last_frame=0;resetWindow();}
 void resetWindow(){epoch=statEnd=statWindow=0;currentStats={};completedStats={};}
 void record(unsigned id,double value,uint64_t now,uint32_t tid){
  if(id>=RBX_PROF_COUNT || !std::isfinite(value) || value<0)return;
  if(!epoch)epoch=now;
  if(now-epoch>=1000000000){completedStats=currentStats;currentStats={};statWindow=now-epoch;statEnd=now;epoch=now;}
  currentStats[id].add(value);
  // Item counters fire per descriptor set / per submit and were crowding the
  // ring down to ~1s of coverage. They stay in the stats; only timing spans
  // need the per-event series.
  if(prof_counter(id))return;
  events[written++%capacity]={now,value,tid,id};
 }
 void diagnostic(uint64_t start,uint64_t end,uint64_t cpu,uint32_t tid,unsigned id,uint64_t bytes,uint64_t object,uint64_t detail){
  if(id>=RBX_DIAG_COUNT || end<start)return;
  diagnostic_events[diagnostic_written++%diagnostic_capacity]={start,end,cpu,bytes,object,detail,tid,id};
 }
 void diagnostic_drop(){++diagnostic_contention_drops;}
 uint64_t diagnostic_retained()const{return std::min<uint64_t>(diagnostic_written,diagnostic_capacity);}
 uint64_t diagnostic_overwritten()const{return diagnostic_written>diagnostic_capacity?diagnostic_written-diagnostic_capacity:0;}
 uint64_t diagnostic_first_start()const{return diagnostic_retained()?diagnostic_events[(diagnostic_written-diagnostic_retained())%diagnostic_capacity].start_ns:0;}
 uint64_t diagnostic_last_end()const{return diagnostic_retained()?diagnostic_events[(diagnostic_written-1)%diagnostic_capacity].end_ns:0;}
 void frame(uint64_t now){if(last_frame && now>last_frame){unsigned i=frame_written++%frame_capacity;frames[i]=(now-last_frame)/1e6;frame_starts[i]=last_frame;frame_ends[i]=now;}last_frame=now;}
 FrameWindow frameWindow(uint64_t through=UINT64_MAX)const{
  FrameWindow w;w.first=frame_written>frame_capacity?frame_written-frame_capacity:0;w.last=frame_written;
  while(w.last>w.first && frame_ends[(w.last-1)%frame_capacity]>through)--w.last;
  if(w.count()){w.begin=frame_starts[w.first%frame_capacity];w.end=frame_ends[(w.last-1)%frame_capacity];}return w;
 }
 // Rolling window, newest first: the ring is append-ordered, so the walk stops
 // at the first event older than the interval instead of scanning all of
 // `capacity`. Cost is the events in the window, not the ring size, which is
 // what makes this affordable to recompute for every HUD refresh.
 std::array<ProfStats,RBX_PROF_COUNT> stats(uint64_t now,uint64_t interval=1000000000) const {
  std::array<ProfStats,RBX_PROF_COUNT> result{};
  const uint64_t first=written>capacity?written-capacity:0;
  for(uint64_t i=written;i-->first;){
   const auto &e=events[i%capacity];
   if(e.end>now)continue;                 // clock skew across threads: skip, don't stop
   if(now-e.end>interval)break;           // everything older is outside the window
   result[e.id].add(e.value);
  }
  return result;
 }
};
