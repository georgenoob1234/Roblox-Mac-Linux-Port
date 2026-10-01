#pragma once
#include "api.h"
#include <dlfcn.h>
#include <chrono>
namespace RbxProfiler {
inline bool enabled() {
 static auto fn=(int(*)(void))dlsym(RTLD_DEFAULT,"tracka_profiler_enabled");
 return fn && fn();
}
inline void metric(unsigned id,double ms) {
 static auto fn=(void(*)(unsigned,double))dlsym(RTLD_DEFAULT,"tracka_profiler_metric");
 if(fn)fn(id,ms);
}
inline unsigned cpuBegin(unsigned id) {
 static auto fn=(unsigned(*)(unsigned))dlsym(RTLD_DEFAULT,"tracka_profiler_cpu_begin");
 return fn?fn(id):UINT32_MAX;
}
inline void cpuEnd(unsigned parent) {
 if(parent==UINT32_MAX)return; // Disabled scopes never entered the host accounting stack.
 static auto fn=(void(*)(unsigned))dlsym(RTLD_DEFAULT,"tracka_profiler_cpu_end");if(fn)fn(parent);
}
inline const void *allocator(unsigned id) {
 static auto fn=(const void*(*)(unsigned))dlsym(RTLD_DEFAULT,"tracka_profiler_allocator");return fn?fn(id):nullptr;
}
struct CpuScope {
 unsigned parent;explicit CpuScope(unsigned id):parent(cpuBegin(id)){}
 void change(unsigned id){cpuEnd(parent);parent=cpuBegin(id);}
 ~CpuScope(){cpuEnd(parent);}
};
struct Scope {
 CpuScope cpu;unsigned id; bool active; std::chrono::steady_clock::time_point start;
 explicit Scope(unsigned value):cpu(value),id(value),active(cpu.parent!=UINT32_MAX),start(active?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{}){}
 ~Scope(){if(active && enabled())metric(id,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());}
};
// Scope above is tied to cpuBegin, which returns UINT32_MAX unless the opt-in
// detailed mode is on, so it records nothing in the default sampling mode. Use
// this for wall-clock spans that must be measured in any mode; it still joins
// the CPU accounting stack when detailed scopes happen to be enabled.
struct WallScope {
 unsigned id,parent; bool active; std::chrono::steady_clock::time_point start;
 explicit WallScope(unsigned value):id(value),parent(UINT32_MAX),active(enabled()){
  if(active){parent=cpuBegin(id);start=std::chrono::steady_clock::now();}
 }
 ~WallScope(){
  if(active && enabled())metric(id,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
  cpuEnd(parent);
 }
};
// Host clocks keep these spans aligned with exported frames across the Darwin
// boundary. Unlike per-draw scopes, these sparse diagnostics run in sample mode.
struct DiagnosticScope {
 RbxProfilerSpan token{};
 unsigned id;
 uint64_t bytes,object,detail;
 explicit DiagnosticScope(unsigned value,uint64_t size=0,const void *owner=nullptr,uint64_t info=0):
  id(value),bytes(size),object(reinterpret_cast<uintptr_t>(owner)),detail(info){
  static auto begin=(void(*)(RbxProfilerSpan*))dlsym(RTLD_DEFAULT,"tracka_profiler_span_begin");
  if(begin)begin(&token);
 }
 DiagnosticScope(const DiagnosticScope&)=delete;
 DiagnosticScope& operator=(const DiagnosticScope&)=delete;
 bool active()const{return token.start_ns!=0;}
 void finish(){
  if(!active())return;
  static auto end=(void(*)(const RbxProfilerSpan*,unsigned,uint64_t,uint64_t,uint64_t))dlsym(RTLD_DEFAULT,"tracka_profiler_span_end");
  if(end)end(&token,id,bytes,object,detail);
  token.start_ns=0;
 }
 ~DiagnosticScope(){finish();}
};
}
