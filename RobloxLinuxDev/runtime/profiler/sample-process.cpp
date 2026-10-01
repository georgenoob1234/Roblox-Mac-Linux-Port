// Reuse the HUD sampler for a hung runner: no guest calls, signals or ptrace.
#include "sampler.h"
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
int main(int argc,char **argv){
 if(argc<2 || argc>3){fprintf(stderr,"usage: sample-process HOST_PID [SECONDS=3, max 60]\n");return 2;}
 char *end=nullptr;long pid=strtol(argv[1],&end,10);if(!*argv[1] || *end || pid<1 || pid>INT32_MAX)return 2;
 long seconds=argc==3?strtol(argv[2],&end,10):3;if((argc==3 && (!*argv[2] || *end)) || seconds<1 || seconds>60)return 2;
 ProcessSampler sampler(+[]{return 1;},pid);
 std::this_thread::sleep_for(std::chrono::seconds(seconds));SamplingSnapshot s;if(!sampler.snapshot(s))return 1;
 printf("samples=%llu attached=%u denied=%u lost=%llu\n",(unsigned long long)s.samples,s.attached,s.denied,(unsigned long long)s.lost);
 printf("active_seconds=%.6f capped=%llu sample_period_ns=%llu\n",s.seconds,(unsigned long long)s.overflow,(unsigned long long)ip_sample_period_ns);
 for(const auto &t:s.threads)if(t.cpu_ms || t.samples)printf("thread %d %s cpu=%.1f user=%.1f kernel=%.1f state=%c hot=0x%llx\n",t.tid,t.name.c_str(),t.cpu_ms,t.user_ms,t.system_ms,t.state,(unsigned long long)t.top_ip);
 std::vector<std::pair<uint64_t,uint64_t>> ranked;for(const auto &[ip,count]:s.ips)ranked.emplace_back(count.count,ip);std::sort(ranked.rbegin(),ranked.rend());
 for(size_t i=0;i<ranked.size() && i<25;++i){auto [count,ip]=ranked[i];const auto *m=find_mapping(s.mappings,ip);printf("%llu 0x%llx %s mapping=0x%llx fileoffset=0x%llx\n",(unsigned long long)count,(unsigned long long)ip,m?m->path.c_str():"[unmapped]",(unsigned long long)(m?m->start:0),(unsigned long long)(m?m->offset:0));}
 // Full module totals retain small distributed hotspots omitted by the top PCs.
 std::map<std::string,uint64_t> modules;
 for(const auto &[ip,value]:s.ips){const auto *m=find_mapping(s.mappings,ip);modules[m && !m->path.empty()?m->path:"[unmapped/anonymous]"]+=value.count;}
 for(const auto &[path,count]:modules)std::cout<<"module "<<count<<" "<<std::quoted(path)<<"\n";
 return s.samples?0:1;
}
