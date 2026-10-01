#include "model.h"
#include "resources.h"
#include "panel.h"
#include "gpu-usage.h"
#include "sampler.h"
#include <cassert>
#include <cstdio>
int main(int argc,char **){
 assert(image_category("/usr/lib/libc++.1.dylib","libc++.1.dylib")==2);
 assert(image_category("/optimized/native/lib/libc++.1.dylib","libc++.1.dylib")==2);
 assert(image_category("/optimized/native/frameworks/Metal.framework/Metal","Metal")==2);
 assert(image_category("/optimized/native/lib/libobjc.A.dylib","libobjc.A.dylib")==1);
 assert(image_category("/client/RobloxPlayer","RobloxPlayer")==0);
 assert(sampled_cpu_ms_per_frame(30,100)==3 && std::isnan(sampled_cpu_ms_per_frame(1,0)));
 {
  static ProfModel frames;frames.frame(100);
  for(uint64_t t=200;t<=70200;t+=100)frames.frame(t);
  auto w=frames.frameWindow(70000);
  assert(w.count()==598 && w.begin==10200 && w.end==70000);
  SampleHistory history;SamplingSnapshot window;
  history.add({w.begin,1,0});history.add({w.begin+1,2,1});
  history.add({w.end+1,3,0});history.add({w.end,2,1}); // cross-thread drain order
  history.window(window,w.begin,w.end);
  assert(window.samples==2 && window.ips.size()==1 && window.ips.at(2).count==2);
  assert(sampled_cpu_ms_per_frame(window.samples,w.count())==20./598);
  assert(!window.window_truncated);
  history.evicted_through=w.begin+1;history.window(window,w.begin,w.end);
  assert(window.window_truncated);
  assert(frames.frameWindow(0).count()==0);
 }
 assert(sampled_cpu_ms(0)==0 && sampled_cpu_ms(100)==1000);
 // More unrelated CPU work lowers the share without lowering category time.
 assert(sampled_cpu_ms(3)==30);
 assert(runner_share(sampled_cpu_ms(3),sampled_cpu_ms(10))==30);
 assert(runner_share(sampled_cpu_ms(3),sampled_cpu_ms(15))==20);
 ThreadReading reading;
 std::string stat="7 (worker ) name) R";for(int field=4;field<=39;++field)stat+=" "+std::to_string(field);
 assert(thread_reading(stat,reading) && reading.name=="worker ) name" && reading.ticks==29 && reading.core==39);
 assert(!thread_reading("broken",reading));
 assert(namespace_tid("Name:\tMain\nNSpid:\t9000 45 18\n",9000)==18);
 assert(namespace_tid("Name:\tMain\nNSpid:\t45\n",45)==45);
 assert(namespace_tid("Name:\tMain\n",45)==45);
 CodeMapping mapping;assert(parse_mapping("1000-2000 r-xp 00001000 08:01 1 /a path/lib.so",mapping));
 assert(mapping.path=="/a path/lib.so" && mapping.offset==4096);
 std::vector<CodeMapping> maps{mapping};assert(find_mapping(maps,0x1000) && !find_mapping(maps,0x2000));
 assert(!parse_mapping("broken",mapping));
 {
  auto read=[](const std::string &text){
   FILE *file=tmpfile();assert(file);assert(fwrite(text.data(),1,text.size(),file)==text.size());rewind(file);
   auto result=read_memory(file);fclose(file);return result;
  };
  auto stanza=[](const char *path,int rss){return std::string("1000-200001000 rw-p 00000000 00:00 0 ")+path+"\nSize: 8388608 kB\nRss: "+std::to_string(rss)+" kB\nPss: "+std::to_string(rss/2)+" kB\nSwap: 4 kB\nVmFlags: rd wr\n";};
  std::string fixture=stanza("/client/RobloxPlayer",1024)+stanza("/optimized/ui/libroblox-wayland.so",128)+stanza("/usr/lib/libc.so",64)+stanza("[heap]",2048)+stanza("/dev/nvidiactl",4096);
  auto memory=read(fixture);
  assert(memory.valid && std::abs(memory.rss()-7.536640)<1e-8);
  assert(std::abs(memory.ram()-3.342336)<1e-8 && std::abs(memory.pss_ram_mb-1.671168)<1e-8);
  assert(std::abs(memory.swap_mb-.020480)<1e-8);
  assert(memory.rss_mb[0]==1.048576 && memory.rss_mb[1]==.131072 && memory.rss_mb[2]==.065536 && memory.rss_mb[3]==2.097152);
  assert(memory.gpu_mapped_mb==8589.934592 && std::abs(memory.mapped_mb-5*memory.gpu_mapped_mb)<1e-8);
  assert(memory_category("/dev/dri/renderD128")==memory_gpu && memory_category("")==3);
  assert(memory_category("/client/RobloxPlayer (deleted)")==0 && memory_category("/client/RobloxPlayer.app/Contents/Resources/texture")==0);
  assert(memory_category("/client/RobloxPlayer.app/Contents/MacOS/libgame.dylib")==0);
  assert(memory_category("/usr/lib/libmimalloc.dylib")==0);
  assert(memory_category("/usr/lib/libSystem.B.dylib")==memory_macos);
  assert(memory_category("/optimized/native/lib/libsystem_malloc.dylib")==memory_macos);
  assert(memory_category("/System/Library/Frameworks/Foundation.framework/Foundation")==memory_macos);
  assert(memory_category("/shims/metal/libmetal-gaps.dylib")==1);
  auto split=read(fixture+stanza("/usr/lib/libSystem.B.dylib",512));assert(split.valid && split.rss_mb[memory_macos]==.524288);
  assert(std::abs(split.roblox_estimate()-3.145728)<1e-8);
  assert(std::abs(split.roblox_estimate()-(split.rss()-split.rss_mb[1]-split.rss_mb[2]-split.rss_mb[memory_gpu]-split.rss_mb[memory_macos]))<1e-8);
  for(const auto &bad:{std::string(),std::string("garbage\n"),fixture+"3000-4000 rw-p 0 00:00 0\nRss: 4 kB\n",std::string("1000-2000 rw-p 0 00:00 0\nRss: -1 kB\nVmFlags: rd\n")}){
   assert(!read(bad).valid);
  }
  for(const char *value:{"18446744073709551616 kB","18014398509481984 kB","4 MB","4 kB garbage"}){
   assert(!read(std::string("1000-2000 rw-p 0 00:00 0\nRss: ")+value+"\nPss: 0 kB\nSwap: 0 kB\nVmFlags: rd\n").valid);
  }
  assert(read(stanza(("/client/RobloxPlayer.app/"+std::string(70000,'x')).c_str(),1024)).rss_mb[0]==1.048576);
  puts("PASS memory breakdown: resident versus reserved/GPU, categories, MB units and incomplete scans");
 }
 SamplingSnapshot limited;for(unsigned i=0;i<8193;++i)sample_ip(limited,i,7);
 sample_ip(limited,0,8);assert(limited.samples==8194 && limited.ips.size()==8192 && limited.overflow==1 && limited.ips[0].cores[8]==1);
 if(argc>1){
  ProcessSampler sampler(+[](){return 1;});
  auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(1100);
  volatile uint64_t work=1;while(std::chrono::steady_clock::now()<until)work=work*1664525+1013904223;
  SamplingSnapshot live;assert(sampler.snapshot(live));assert(live.sweeps>=2 && !live.threads.empty() && live.rss_mib>0);
  assert(live.memory.valid && live.memory.rss()>0 && live.memory_time_ns);
  if(live.attached)assert(live.samples>0 && !live.ips.empty());else assert(live.denied && live.error);
  unsigned mapped=0;for(const auto &t:live.threads){if(t.proc_tid!=t.tid)++mapped;if(t.samples)assert(t.top_ip);assert(t.attempts>0);}
  printf("Thread namespace mappings: %u; CPU coverage %.1f / %.1f ms\n",mapped,live.covered_cpu_ms,live.total_thread_cpu_ms);
  printf("PASS live sampler: %u attached, %llu PCs, %u unavailable (errno %d)\n",live.attached,(unsigned long long)live.samples,live.denied,live.error);
 }
 puts("PASS thread-stat parser, bounded PC accounting and core samples");
 static ProfModel m;m.frame(1000000);m.frame(5000000);assert(m.frames[0]==4);
 m.record(RBX_PROF_SWAP,2,100,7);m.record(RBX_PROF_SWAP,4,200,8);
 auto s=m.stats(200,150);assert(s[RBX_PROF_SWAP].count==2 && s[RBX_PROF_SWAP].sum==6 && s[RBX_PROF_SWAP].max==4);
 assert(m.stats(300,150)[RBX_PROF_SWAP].count==1);
 m.record(RBX_PROF_COUNT,1,300,7);m.record(0,NAN,300,7);m.record(0,-1,300,7);assert(m.written==2);
 for(unsigned i=0;i<ProfModel::capacity+10;++i)m.record(RBX_PROF_GPU,1,1000+i,9);
 s=m.stats(100000,100000);assert(s[RBX_PROF_GPU].count==ProfModel::capacity && !s[RBX_PROF_SWAP].count);
 static ProfModel rolling;
 for(unsigned i=0;i<ProfModel::capacity+20;++i)rolling.record(RBX_PROF_DRAW,1,1000+i,7);
 rolling.record(RBX_PROF_DRAW,2,1000001000,7);
 assert(rolling.completedStats[RBX_PROF_DRAW].count==ProfModel::capacity+20);
 assert(rolling.currentStats[RBX_PROF_DRAW].sum==2);
 rolling.resetWindow();assert(!rolling.epoch && !rolling.completedStats[RBX_PROF_DRAW].count);
 assert(prof_counter(RBX_PROF_READS) && !prof_counter(RBX_PROF_GPU));
 ProfModel &diagnostics=rolling;diagnostics.resetCapture();
 diagnostics.diagnostic(100,130,7,11,RBX_DIAG_BUFFER_CREATE,4096,0x1234,9);
 assert(diagnostics.diagnostic_written==1 && diagnostics.diagnostic_retained()==1 && diagnostics.diagnostic_overwritten()==0);
 assert(diagnostics.diagnostic_events[0].start_ns==100 && diagnostics.diagnostic_events[0].end_ns==130 && diagnostics.diagnostic_events[0].cpu_ns==7);
 diagnostics.diagnostic(100,130,0,0,RBX_DIAG_COUNT,0,0,0); // invalid id
 diagnostics.diagnostic(200,199,0,0,RBX_DIAG_BUFFER_CREATE,0,0,0); // invalid span
 assert(diagnostics.diagnostic_written==1);
 for(unsigned i=0;i<ProfModel::diagnostic_capacity+3;++i)diagnostics.diagnostic(1000+i,1001+i,1,2,RBX_DIAG_COMMAND_WAIT,i,3,4);
 assert(diagnostics.diagnostic_written==ProfModel::diagnostic_capacity+4);
 assert(diagnostics.diagnostic_retained()==ProfModel::diagnostic_capacity && diagnostics.diagnostic_overwritten()==4);
 assert(diagnostics.diagnostic_events[0].start_ns==1000+ProfModel::diagnostic_capacity-1);
 diagnostics.diagnostic_drop();assert(diagnostics.diagnostic_contention_drops==1);
 diagnostics.resetCapture();assert(!diagnostics.diagnostic_written && !diagnostics.diagnostic_contention_drops);
 LayerAccounting layer;ResourceHistory h;std::vector<int> online{0,7};
 h.sample(1000000000,layer,online,1000000000);assert(std::isnan(h.points.back().cpu.at(-1)));
 layer.charge(RBX_PROF_DRAW,7,7,250000000);layer.charge(RBX_PROF_SHADER,0,7,1000);
 layer.allocated(RBX_PROF_SHADER,2097152);layer.freed(RBX_PROF_SHADER,1048576);
 layer.gpu[RBX_PROF_GPU]=7000000;layer.gpu[RBX_PROF_COMPOSITE]=3000000;
 h.sample(1500000000,layer,online,2000000000,42);
 assert(h.runner_cpu_ms==1000 && runner_share(h.stage_cpu[RBX_PROF_DRAW],h.runner_cpu_ms)==25);
 assert(h.total_gpu_ms==10 && runner_share(h.stage_gpu[RBX_PROF_GPU],h.total_gpu_ms)==70 && h.points.back().gpu==42);
 assert(std::isnan(runner_share(0,0)));
 assert(h.points.back().cpu.at(7)==50 && h.points.back().cpu.at(-1)==25);
 assert(h.points.back().cpu.size()==3 && h.stage_cores[RBX_PROF_SHADER][0] && h.stage_cores[RBX_PROF_SHADER][7]);
 assert(h.stage_ram[RBX_PROF_SHADER]==1 && h.stage_peak[RBX_PROF_SHADER]==2);
 assert(h.migrated_ms==.001 && core_list(h.stage_cores[RBX_PROF_SHADER])=="0, 7");
 h.sample(5000000000,layer,online);assert(std::isnan(h.points.back().cpu.at(-1)));
 for(uint64_t t=5500000000;t<=70000000000;t+=500000000)h.sample(t,layer,online);
 assert(h.points.size()==1);
 ProfPanel panel;panel.x=10;panel.y=10;panel.width=200;panel.height=300;panel.view_width=panel.view_height=1000;
 assert(!panel.mouse(1,.05,.05,0,true)); // game click through ordinary panel content
 assert(!panel.mouse(1,.205,.305,0,false)); // Shift Lock must not grab resize
 assert(panel.mouse(1,.205,.305,0,true));
 assert(panel.mouse(0,.255,.355,0,true) && std::abs(panel.width-250)<.01 && std::abs(panel.height-350)<.01);
 assert(panel.mouse(2,.9,.9,0,false) && !panel.dragging); // paired release outside / hidden
 assert(!panel.mouse(2,.9,.9,0,true));
 assert(panel.mouse(3,.05,.05,2,true) && panel.scroll==2);
 assert(!panel.mouse(3,.9,.9,2,true));
 assert(panel.mouse(1,.255,.355,0,true));panel.mouse(4,0,0,0,false);
 assert(!panel.mouse(0,.9,.9,0,true) && panel.mouse(2,.9,.9,0,true));
 assert(panel.mouse(1,.05,.02,0,true));
 assert(panel.mouse(0,.15,.12,0,true) && std::abs(panel.x-110)<.01 && std::abs(panel.y-110)<.01);
 assert(panel.mouse(2,.15,.12,0,true) && !panel.moving);
 std::vector<ProcessGpuSample> gpu_samples{{42,1000000,20,0,0,0},{99,2000000,100,0,0,0},{42,2000000,35,0,0,0}};
 assert(process_gpu_percent(gpu_samples,42,2100000)==35);
 assert(std::isnan(process_gpu_percent(gpu_samples,42,5000000)));
 assert(std::isnan(process_gpu_percent(gpu_samples,1,2100000)));
 assert(std::isnan(process_gpu_percent(gpu_samples,42,900000)));
 puts("PASS runner CPU denominator, GPU shares, process-only GPU selection and stale/missing samples");
 puts("PASS resize grip, scaled coordinates, click passthrough, wheel bounds and release after blur");
 puts("PASS scoped layer CPU, migration, sparse IDs, per-stage RAM/free/peak and 60s retention");
 puts("PASS profiler bounded retention, timing units, rolling window and invalid samples");
}
