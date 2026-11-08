// Per-thread user-space CPU sampling with frame-pointer call chains.
// usage: stacksample PID SECONDS OUTDIR
// Writes OUTDIR/stacks.txt ("tid count ip0 ip1 ..."), maps.txt, threads.txt.
#include <linux/perf_event.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <chrono>
#include <fstream>
#include <sstream>

struct Slot { int tid; int fd; void* base; size_t size; };
static const size_t kPages = 64; // data pages per ring

static std::vector<int> threads(int pid) {
    std::vector<int> out;
    std::string path = "/proc/" + std::to_string(pid) + "/task";
    if (DIR* d = opendir(path.c_str())) {
        while (auto* e = readdir(d)) if (e->d_name[0] != '.') out.push_back(atoi(e->d_name));
        closedir(d);
    }
    return out;
}
static std::string slurp(const std::string& p) { std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str(); }
static unsigned long long cpuTicks(int pid, int tid) {
    std::string s = slurp("/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/stat");
    auto close = s.rfind(')'); if (close == std::string::npos) return 0;
    std::istringstream in(s.substr(close + 2)); std::string f; unsigned long long ut = 0, st = 0;
    for (int i = 3; i <= 15; ++i) { in >> f; if (i == 14) ut = strtoull(f.c_str(), 0, 10); if (i == 15) st = strtoull(f.c_str(), 0, 10); }
    return ut + st;
}

int main(int argc, char** argv) {
    if (argc != 4) { fprintf(stderr, "usage: stacksample PID SECONDS OUTDIR\n"); return 2; }
    int pid = atoi(argv[1]); int seconds = atoi(argv[2]); std::string out = argv[3];
    long page = sysconf(_SC_PAGESIZE);
    std::map<int, Slot> slots;
    std::map<std::vector<uint64_t>, uint64_t> stacks; // [tid, ips...] -> count
    std::map<int, unsigned long long> startTicks;
    std::map<int, std::string> names;
    uint64_t lost = 0, total = 0;
    auto attach = [&] {
        for (int tid : threads(pid)) {
            if (slots.count(tid)) continue;
            perf_event_attr a{}; a.size = sizeof(a); a.type = PERF_TYPE_SOFTWARE; a.config = PERF_COUNT_SW_CPU_CLOCK;
            a.sample_period = 1000000; // 1 ms
            a.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_CALLCHAIN;
            a.exclude_kernel = 1; a.exclude_hv = 1; a.exclude_callchain_kernel = 1; a.wakeup_events = 1;
            int fd = syscall(SYS_perf_event_open, &a, tid, -1, -1, PERF_FLAG_FD_CLOEXEC);
            if (fd < 0) { slots[tid] = {tid, -1, nullptr, 0}; continue; }
            size_t size = (kPages + 1) * page;
            void* base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (base == MAP_FAILED) { close(fd); slots[tid] = {tid, -1, nullptr, 0}; continue; }
            slots[tid] = {tid, fd, base, size};
            startTicks[tid] = cpuTicks(pid, tid);
            std::string n = slurp("/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/comm");
            if (!n.empty() && n.back() == '\n') n.pop_back();
            names[tid] = n;
        }
    };
    auto drain = [&](Slot& s) {
        auto* meta = static_cast<perf_event_mmap_page*>(s.base);
        char* data = static_cast<char*>(s.base) + page; size_t dsize = kPages * page;
        uint64_t head = __atomic_load_n(&meta->data_head, __ATOMIC_ACQUIRE), tail = meta->data_tail;
        std::vector<char> rec;
        while (tail < head) {
            perf_event_header h; for (size_t i = 0; i < sizeof h; ++i) reinterpret_cast<char*>(&h)[i] = data[(tail + i) % dsize];
            rec.resize(h.size); for (size_t i = 0; i < h.size; ++i) rec[i] = data[(tail + i) % dsize];
            if (h.type == PERF_RECORD_SAMPLE) {
                const char* p = rec.data() + sizeof h;
                uint32_t rpid, rtid; memcpy(&rpid, p, 4); memcpy(&rtid, p + 4, 4); p += 8;
                uint64_t nr; memcpy(&nr, p, 8); p += 8;
                std::vector<uint64_t> key{rtid};
                for (uint64_t i = 0; i < nr && i < 24; ++i) { uint64_t ip; memcpy(&ip, p + 8 * i, 8); if (ip >= (uint64_t)-4095) continue; key.push_back(ip); }
                ++stacks[key]; ++total;
            } else if (h.type == PERF_RECORD_LOST) { uint64_t l; memcpy(&l, rec.data() + sizeof h + 8, 8); lost += l; }
            tail += h.size;
        }
        __atomic_store_n(&meta->data_tail, tail, __ATOMIC_RELEASE);
    };
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    auto nextAttach = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < end) {
        if (std::chrono::steady_clock::now() >= nextAttach) { attach(); nextAttach += std::chrono::seconds(1); }
        for (auto& [tid, s] : slots) if (s.fd >= 0) drain(s);
        usleep(2000);
        if (kill(pid, 0) != 0) break;
    }
    for (auto& [tid, s] : slots) if (s.fd >= 0) drain(s);
    FILE* f = fopen((out + "/stacks.txt").c_str(), "w");
    for (auto& [k, c] : stacks) { fprintf(f, "%llu %llu", (unsigned long long)k[0], (unsigned long long)c); for (size_t i = 1; i < k.size(); ++i) fprintf(f, " %llx", (unsigned long long)k[i]); fputc('\n', f); }
    fclose(f);
    f = fopen((out + "/threads.txt").c_str(), "w");
    for (auto& [tid, n] : names) fprintf(f, "%d %llu %s\n", tid, cpuTicks(pid, tid) - startTicks[tid], n.c_str());
    fclose(f);
    std::ofstream(out + "/maps.txt") << slurp("/proc/" + std::to_string(pid) + "/maps");
    printf("samples=%llu lost=%llu threads=%zu\n", (unsigned long long)total, (unsigned long long)lost, slots.size());
    return 0;
}
