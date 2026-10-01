#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <sstream>
#include <vector>
#include <algorithm>
#include <array>
#include <cmath>
#include "memory.h"
inline unsigned image_category(const std::string &path,const std::string &name){
 if(name=="RobloxPlayer" || name.find("mimalloc")!=std::string::npos)return 0;
 // Rebuilding these host libraries under optimized/ must not reclassify their CPU.
 if(name=="libc++.1.dylib" || name=="Metal")return 2;
 if(path.find("/optimized/")!=std::string::npos || path.find("/shims/")!=std::string::npos || name.find("indium")!=std::string::npos || name.find("roblox-wayland")!=std::string::npos)return 1;
 return 2;
}
struct CodeMapping {uint64_t start=0,end=0,offset=0;std::string permissions,path;};
inline bool parse_mapping(const std::string &line,CodeMapping &m){
 unsigned long long start,end,offset,inode;char perms[5]{},device[32]{};int consumed=0;
 if(sscanf(line.c_str(),"%llx-%llx %4s %llx %31s %llu %n",&start,&end,perms,&offset,device,&inode,&consumed)!=6 || end<=start)return false;
 m={start,end,offset,perms,line.substr(consumed)};return true;
}
inline const CodeMapping *find_mapping(const std::vector<CodeMapping> &maps,uint64_t ip){
 auto it=std::upper_bound(maps.begin(),maps.end(),ip,[](uint64_t value,const CodeMapping &m){return value<m.start;});
 if(it==maps.begin())return nullptr;--it;return ip<it->end?&*it:nullptr;
}

inline constexpr unsigned memory_gpu=4;
inline constexpr unsigned memory_macos=5;
inline constexpr const char *memory_names[]={"Roblox libraries/assets", "Compatibility layer files", "Host libraries/files", "Unattributed heaps/stacks", "GPU device mappings", "macOS libraries/frameworks"};
struct MemoryReading {
 bool valid=false;
 std::array<double,6> rss_mb{};
 double mapped_mb=0,gpu_mapped_mb=0,pss_ram_mb=0,swap_mb=0;
 double rss()const{double total=0;for(double mb:rss_mb)total+=mb;return total;}
 double ram()const{return rss()-rss_mb[memory_gpu];}
 // ponytail: residual estimate retains all anonymous heaps. Allocation-owner
 // tracking is required to subtract runtime/driver heaps as well as their files.
 double roblox_estimate()const{return rss_mb[0]+rss_mb[3];}
};
inline unsigned memory_category(std::string path){return rbx_memory_category(path.data());}
inline MemoryReading read_memory(FILE *input){
 RbxMemoryReading bytes{};MemoryReading result;
 if(!rbx_read_memory(input,&bytes))return result;
 result.valid=true;
 for(unsigned i=0;i<result.rss_mb.size();++i)result.rss_mb[i]=bytes.rss[i]/1e6;
 result.mapped_mb=bytes.mapped/1e6;result.gpu_mapped_mb=bytes.gpu_mapped/1e6;
 result.pss_ram_mb=bytes.pss_ram/1e6;result.swap_mb=bytes.swap/1e6;
 return result;
}
