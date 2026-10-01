#pragma once
#include "api.h"
#include <array>
#include <atomic>
#include <bitset>
#include <string>

// CPU time is exclusive of nested instrumented scopes, never wall time spent
// asleep. A migration between boundaries is unassigned, not pinned to a false core.
struct LayerAccounting {
 static constexpr unsigned cores=256;
 std::array<std::array<std::atomic<uint64_t>,cores>,RBX_PROF_COUNT> cpu{};
 std::array<std::array<std::atomic<bool>,cores>,RBX_PROF_COUNT> seen{};
 std::array<std::atomic<uint64_t>,RBX_PROF_COUNT> migrated{},live{},peak{},gpu{};
 std::array<std::atomic<bool>,RBX_PROF_COUNT> memory_tracked{},cpu_tracked{},gpu_tracked{};
 void charge(unsigned stage,int from,int to,uint64_t ns){
  if(stage>=RBX_PROF_COUNT)return;
  // Telemetry flags publish no other memory; sequentially consistent stores
  // would emit a locked xchg on every scope boundary on x86.
  cpu_tracked[stage].store(true,std::memory_order_relaxed);
  if(from>=0 && unsigned(from)<cores)seen[stage][from].store(true,std::memory_order_relaxed);
  if(to>=0 && unsigned(to)<cores)seen[stage][to].store(true,std::memory_order_relaxed);
  if(from!=to || to<0 || unsigned(to)>=cores){migrated[stage]+=ns;return;}
  cpu[stage][to].fetch_add(ns,std::memory_order_relaxed);
 }
 void allocated(unsigned stage,size_t size){
  memory_tracked[stage]=true;
  uint64_t value=live[stage].fetch_add(size)+size,old=peak[stage].load();
  while(value>old && !peak[stage].compare_exchange_weak(old,value)){}
 }
 void freed(unsigned stage,size_t size){live[stage].fetch_sub(size);}
};
static std::string core_list(const std::bitset<LayerAccounting::cores> &set){
 std::string result;
 for(unsigned i=0;i<set.size();++i)if(set[i]){
  unsigned first=i;while(i+1<set.size() && set[i+1])++i;
  if(!result.empty())result+=", ";result+=std::to_string(first);
  if(i>first)result+="-"+std::to_string(i);
 }
 return result.empty()?"--":result;
}
