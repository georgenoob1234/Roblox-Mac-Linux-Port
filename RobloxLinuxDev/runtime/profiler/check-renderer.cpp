// Exercise the real wrapper with exported host spies; no renderer/GPU required.
#include "renderer.h"
#include <cassert>
#include <cstdio>
#include <ctime>
static bool collecting;
static unsigned current=RBX_PROF_COUNT,starts,ends,metrics,queries,clocks;
static unsigned diagnostic_begins,diagnostic_ends;
extern "C" void tracka_profiler_span_begin(RbxProfilerSpan *span){
 ++diagnostic_begins;*span={};if(collecting)span->start_ns=++clocks;
}
extern "C" void tracka_profiler_span_end(const RbxProfilerSpan *span,unsigned id,uint64_t bytes,uint64_t object,uint64_t detail){
 if(!collecting)return;
 assert(span->start_ns && id==RBX_DIAG_BUFFER_COPY && bytes==128 && object==0x1234 && detail==7);
 ++diagnostic_ends;++clocks;
}
// Interpose the standalone check's clock so hidden reads fail deterministically.
extern "C" int clock_gettime(clockid_t clock,timespec *time) noexcept {
 assert(clock==CLOCK_MONOTONIC);time->tv_sec=++clocks;time->tv_nsec=0;return 0;
}
extern "C" int tracka_profiler_enabled(){++queries;return collecting;}
extern "C" unsigned tracka_profiler_cpu_begin(unsigned stage){
 ++starts;if(!collecting)return UINT32_MAX;
 unsigned previous=current;current=stage;return previous;
}
extern "C" void tracka_profiler_cpu_end(unsigned parent){assert(parent!=UINT32_MAX);++ends;current=parent;}
extern "C" void tracka_profiler_metric(unsigned,double ms){assert(collecting && ms>=0);++metrics;}
int main(){
 {RbxProfiler::Scope draw(RBX_PROF_DRAW);RbxProfiler::Scope bindings(RBX_PROF_BINDINGS);}
 assert(starts==2 && ends==0 && metrics==0 && queries==0 && clocks==0);
 collecting=true;
 {RbxProfiler::Scope draw(RBX_PROF_DRAW);
  assert(current==RBX_PROF_DRAW);
  {RbxProfiler::Scope bindings(RBX_PROF_BINDINGS);assert(current==RBX_PROF_BINDINGS);}
  assert(current==RBX_PROF_DRAW);
 }
 assert(current==RBX_PROF_COUNT && starts==4 && ends==2 && metrics==2 && queries==2 && clocks==4);
 {RbxProfiler::CpuScope scope(RBX_PROF_DRAW);scope.change(RBX_PROF_BINDINGS);assert(current==RBX_PROF_BINDINGS);}
 assert(current==RBX_PROF_COUNT);
 const auto before=clocks;
 {RbxProfiler::Scope scope(RBX_PROF_DRAW);
  {RbxProfiler::Scope nested(RBX_PROF_BINDINGS);collecting=false;}
  assert(current==RBX_PROF_DRAW);
 }
 assert(current==RBX_PROF_COUNT); // Unwind even if collection stopped during a scope.
 assert(clocks==before+2 && metrics==2); // Starts only; no hidden end clocks/metrics.
 {RbxProfiler::Scope scope(RBX_PROF_DRAW);collecting=true;}
 assert(current==RBX_PROF_COUNT && clocks==before+2 && metrics==2);
 {RbxProfiler::Scope scope(RBX_PROF_DRAW);}
 assert(current==RBX_PROF_COUNT && clocks==before+4 && metrics==3);
 const auto diagnostic_clocks=clocks;
 collecting=false;
 {RbxProfiler::DiagnosticScope hidden(RBX_DIAG_BUFFER_COPY);assert(!hidden.active());}
 assert(clocks==diagnostic_clocks && diagnostic_ends==0);
 collecting=true;
 {RbxProfiler::DiagnosticScope span(RBX_DIAG_BUFFER_COPY,128,reinterpret_cast<void*>(0x1234),7);
  assert(span.active());span.finish();span.finish();assert(!span.active());}
 assert(diagnostic_ends==1 && clocks==diagnostic_clocks+2);
 {RbxProfiler::DiagnosticScope stopped(RBX_DIAG_BUFFER_COPY);collecting=false;}
 assert(diagnostic_ends==1 && clocks==diagnostic_clocks+3);
 {RbxProfiler::DiagnosticScope hidden(RBX_DIAG_BUFFER_COPY);collecting=true;}
 assert(diagnostic_begins==4 && diagnostic_ends==1 && clocks==diagnostic_clocks+3);
 std::puts("PASS nested scopes, disabled bridge elision, stage changes, hidden transitions and clock elision");
}
