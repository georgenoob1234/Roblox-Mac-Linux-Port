#pragma once
#include <pthread.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <dlfcn.h>
#include <mutex>
#include <thread>
#include <vector>
#include <unistd.h>

// Stable NVML ABI; optional driver library, not a build/runtime requirement.
// NVIDIA/go-nvml pkg/nvml/nvml.h: nvmlProcessUtilizationSample_t.
struct ProcessGpuSample {
 unsigned pid;unsigned long long timeStamp;unsigned smUtil,memUtil,encUtil,decUtil;
};
static double process_gpu_percent(const std::vector<ProcessGpuSample> &samples,unsigned pid,uint64_t now_us){
 const ProcessGpuSample *latest=nullptr;
 for(const auto &s:samples)if(s.pid==pid && s.smUtil<=100 && s.timeStamp<=now_us && now_us-s.timeStamp<=2500000 && (!latest || s.timeStamp>latest->timeStamp))latest=&s;
 return latest?double(latest->smUtil):NAN;
}
class ProcessGpuUsage {
 std::atomic<double> value{NAN};
 std::atomic<uint64_t> sampled{0};
 std::mutex mutex;std::condition_variable wake;bool stop=false;
 std::thread worker;
 static uint64_t monotonic_ms(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
 public:
 explicit ProcessGpuUsage(int (*active)()):worker([this,active]{
  pthread_setname_np(pthread_self(),"rbx-prof-gpu");
  void *lib=dlopen("libnvidia-ml.so.1",RTLD_LAZY|RTLD_LOCAL);if(!lib)return;
  auto init=(int(*)())dlsym(lib,"nvmlInit_v2");auto shutdown=(int(*)())dlsym(lib,"nvmlShutdown");
  auto count=(int(*)(unsigned*))dlsym(lib,"nvmlDeviceGetCount_v2");
  auto handle=(int(*)(unsigned,void**))dlsym(lib,"nvmlDeviceGetHandleByIndex_v2");
  auto read=(int(*)(void*,ProcessGpuSample*,unsigned*,unsigned long long))dlsym(lib,"nvmlDeviceGetProcessUtilization");
  if(!init || !shutdown || !count || !handle || !read || init()!=0){dlclose(lib);return;}
  unsigned devices=0;count(&devices);std::vector<void*> gpu;
  for(unsigned i=0;i<devices;++i){void *d=nullptr;if(!handle(i,&d))gpu.push_back(d);}
  while(true){
   if(active()){
    double result=NAN;
    auto now_us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    for(void *device:gpu){
     unsigned n=0;int status=read(device,nullptr,&n,0);
     if(status!=7 || !n || n>65536)continue; // insufficient-size / unsupported / no data
     std::vector<ProcessGpuSample> samples(n);
     if(read(device,samples.data(),&n,0)!=0 || n>samples.size())continue;samples.resize(n);
     double use=process_gpu_percent(samples,getpid(),now_us);
     if(std::isfinite(use))result=std::isfinite(result)?std::max(result,use):use;
    }
    value=result;sampled=monotonic_ms();
   }else {value=NAN;sampled=0;}
   std::unique_lock<std::mutex> lock(mutex);if(wake.wait_for(lock,std::chrono::milliseconds(500),[this]{return stop;}))break;
  }
  shutdown();dlclose(lib);
 }){}
 ~ProcessGpuUsage(){ {std::lock_guard<std::mutex> lock(mutex);stop=true;}wake.notify_one();worker.join(); }
 double percent()const{return monotonic_ms()-sampled.load()<=2500?value.load():NAN;}
};
