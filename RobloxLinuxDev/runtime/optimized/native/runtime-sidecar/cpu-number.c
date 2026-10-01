// Pair stable process CPU IDs with the existing affinity-aware hw.ncpu shim.
#include "../../../../native/cpu-topology.h"
#include "../../../src/darling/src/startup/mldr/elfcalls/elfcalls.h"

// IDs are ranks inside the CPU domain the process started with (the launcher
// applies any affinity before exec), so they stay dense and stable. A thread
// running on a CPU outside that domain gets EAGAIN. macOS reads this from a
// register in ~20 cycles and Roblox's fiber scheduler calls it on hot paths;
// the former per-call pair of sched_getaffinity syscalls cost ~1 us each.
static uint64_t initial_mask[128];
static long initial_bytes;
static int mapping_ready;
// Linux keeps the CPU number in the low 12 bits of TSC_AUX, the value RDPID
// and the vDSO return, so 4096 entries cover every ID the fast paths report.
static uint16_t cpu_rank[4096];
static int cpu_rdpid;

extern struct elf_calls *_elfcalls;
typedef int (*cpu_getcpu_fn)(unsigned *, unsigned *, void *);
static cpu_getcpu_fn cpu_vdso_getcpu;

static void initialize_cpu_getcpu(void) {
    // Linux x86's vDSO CPU field is 12 bits. Keep the full-width syscall when
    // the kernel mask can include higher IDs, even outside our initial domain.
    if (initial_bytes > 4096 / 8) return;
    if (!_elfcalls || !_elfcalls->dlopen || !_elfcalls->dlsym) return;
    int saved = errno;
    void *vdso = _elfcalls->dlopen("linux-vdso.so.1");
    if (vdso) {
        // The existing ELF bridge uses Linux's loader/RTLD_DEFAULT (NULL).
        // Resolve the versioned x86_64 ABI; keep the handle across fork.
        void *(*versioned)(void *, const char *, const char *) =
            (void *(*)(void *, const char *, const char *))_elfcalls->dlsym(NULL, "dlvsym");
        cpu_getcpu_fn fast = versioned ?
            (cpu_getcpu_fn)versioned(vdso, "__vdso_getcpu", "LINUX_2.6") : NULL;
        if (fast) __atomic_store_n(&cpu_vdso_getcpu, fast, __ATOMIC_RELEASE);
    }
    errno = saved;
}

static unsigned rdpid_cpu(void) {
    uint64_t aux;
    __asm__ volatile("rdpid %0" : "=r"(aux));
    return (unsigned)aux & 0xfff;
}

static void initialize_cpu_rdpid(void) {
    if (!initial_bytes || initial_bytes > 4096 / 8) return;
    unsigned regs[4];
    __asm__ volatile("cpuid" : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3]) : "a"(0), "c"(0));
    if (regs[0] < 7) return;
    __asm__ volatile("cpuid" : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3]) : "a"(7), "c"(0));
    if (!(regs[2] & (1u << 22))) return; // CPUID.7.0:ECX.RDPID
    // Linux programs TSC_AUX whenever RDPID exists. Confirm it against getcpu;
    // retry because the thread can migrate between the two reads.
    for (int attempt = 0; attempt < 64; ++attempt) {
        unsigned fast = rdpid_cpu(), raw = UINT32_MAX;
        if (!cpu_linux_call(309, (long)&raw, 0, 0) && raw == fast && rdpid_cpu() == fast) {
            __atomic_store_n(&cpu_rdpid, 1, __ATOMIC_RELEASE);
            return;
        }
    }
}

static void capture_cpu_domain(void) {
    long bytes = cpu_linux_call(204, 0, sizeof(initial_mask), (long)initial_mask);
    if (bytes <= 0 || bytes > (long)sizeof(initial_mask) || bytes % sizeof(*initial_mask)) return;
    unsigned rank = 0;
    for (unsigned cpu = 0; cpu < sizeof(cpu_rank) / sizeof(*cpu_rank); ++cpu) cpu_rank[cpu] = UINT16_MAX;
    for (unsigned cpu = 0; cpu < (unsigned)bytes * 8; ++cpu)
        if (initial_mask[cpu / 64] & (UINT64_C(1) << (cpu % 64))) {
            if (cpu < sizeof(cpu_rank) / sizeof(*cpu_rank)) cpu_rank[cpu] = (uint16_t)rank;
            ++rank;
        }
    if (!rank) return;
    initial_bytes = bytes;
    __atomic_store_n(&mapping_ready, 1, __ATOMIC_RELEASE);
}

__attribute__((constructor)) static void initialize_cpu_number(void) {
    capture_cpu_domain();
    initialize_cpu_getcpu();
    initialize_cpu_rdpid();
}

static int cpu_number_error(int error) { errno = error; return error; }

int pthread_cpu_number_np(size_t *out) {
    if (!out) return cpu_number_error(EINVAL);
    if (!__atomic_load_n(&mapping_ready, __ATOMIC_ACQUIRE))
        return cpu_number_error(ENOTSUP);
    unsigned cpu = UINT32_MAX;
    if (__atomic_load_n(&cpu_rdpid, __ATOMIC_RELAXED)) {
        cpu = rdpid_cpu();
    } else {
        // Before/during resolution, or on vDSO failure, retain the raw syscall.
        cpu_getcpu_fn fast = __atomic_load_n(&cpu_vdso_getcpu, __ATOMIC_ACQUIRE);
        if ((!fast || fast(&cpu, NULL, NULL)) && cpu_linux_call(309, (long)&cpu, 0, 0) < 0)
            return cpu_number_error(EIO);
    }
    size_t rank;
    if (cpu < sizeof(cpu_rank) / sizeof(*cpu_rank)) {
        rank = cpu_rank[cpu];
        if (rank == UINT16_MAX) return cpu_number_error(EAGAIN);
    } else {
        // Only the raw syscall reports IDs above 4095; rank them from the mask.
        if (cpu >= (unsigned)initial_bytes * 8 || !(initial_mask[cpu / 64] & (UINT64_C(1) << (cpu % 64))))
            return cpu_number_error(EAGAIN);
        rank = 0;
        for (unsigned i = 0; i < cpu / 64; ++i) rank += __builtin_popcountll(initial_mask[i]);
        rank += __builtin_popcountll(initial_mask[cpu / 64] & ((UINT64_C(1) << (cpu % 64)) - 1));
    }
    *out = rank;
    return 0;
}
