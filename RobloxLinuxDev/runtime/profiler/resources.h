#pragma once
#include "accounting.h"
#include <cmath>
#include <deque>
#include <map>
#include <vector>

struct ResourceSample {
 uint64_t time=0;
 std::map<int,double> cpu; // -1 = normalized layer total, other IDs = observed CPUs
 double ram=0,gpu=NAN;
};
struct ResourceHistory {
 static constexpr uint64_t second=1000000000,span=60*second;
 std::deque<ResourceSample> points;
 std::array<std::bitset<LayerAccounting::cores>,RBX_PROF_COUNT> stage_cores;
 std::array<double,RBX_PROF_COUNT> stage_cpu{},stage_ram{},stage_peak{};
 std::array<bool,RBX_PROF_COUNT> cpu_tracked{},memory_tracked{},gpu_tracked{};
 double migrated_ms=0,ram_peak=0,runner_cpu_ms=NAN,total_gpu_ms=0;
 uint64_t previous_process=0;
 std::array<double,RBX_PROF_COUNT> stage_gpu{};
 void sample(uint64_t now,LayerAccounting &layer,const std::vector<int> &online,uint64_t process_cpu=0,double gpu_percent=NAN){
  ResourceSample point;point.time=now;point.gpu=gpu_percent;
  double seconds=points.empty()?0:(now-points.back().time)/1e9;
  bool valid=seconds>0 && seconds<=1.5;
  runner_cpu_ms=valid && previous_process && process_cpu>=previous_process?(process_cpu-previous_process)/1e6:NAN;
  previous_process=process_cpu;total_gpu_ms=0;
  migrated_ms=0;stage_cpu={};stage_cores={};
  for(int core:online)point.cpu[core]=valid?0:NAN;
  point.cpu[-1]=valid?0:NAN;
  for(unsigned stage=0;stage<RBX_PROF_COUNT;++stage){
   for(unsigned core=0;core<LayerAccounting::cores;++core){
    uint64_t ns=layer.cpu[stage][core].exchange(0,std::memory_order_relaxed);
    if(layer.seen[stage][core].exchange(false))stage_cores[stage].set(core);
    if(valid && ns){stage_cpu[stage]+=ns/1e6;point.cpu[core]+=ns/1e9/seconds*100.;}
   }
   double migrated=layer.migrated[stage].exchange(0)/1e6;
   if(valid){migrated_ms+=migrated;stage_cpu[stage]+=migrated;}
   gpu_tracked[stage]=layer.gpu_tracked[stage];
   double gpu_ms=layer.gpu[stage].exchange(0)/1e6;stage_gpu[stage]=valid?gpu_ms:0;total_gpu_ms+=stage_gpu[stage];
   cpu_tracked[stage]=layer.cpu_tracked[stage];memory_tracked[stage]=layer.memory_tracked[stage];
   stage_ram[stage]=layer.live[stage].load()/1048576.;
   stage_peak[stage]=layer.peak[stage].load()/1048576.;point.ram+=stage_ram[stage];
  }
  if(valid){
   for(const auto &[core,percent]:point.cpu)if(core>=0)point.cpu[-1]+=percent;
   point.cpu[-1]/=std::max<size_t>(1,online.size());
  }
  ram_peak=std::max(ram_peak,point.ram);points.push_back(std::move(point));
  while(points.size()>1)points.pop_front(); // Numeric readouts need only the latest sample.
 }
};

static double runner_share(double part,double total){
 return std::isfinite(part) && std::isfinite(total) && total>0 && part>=0 ? 100.*part/total:NAN;
}
