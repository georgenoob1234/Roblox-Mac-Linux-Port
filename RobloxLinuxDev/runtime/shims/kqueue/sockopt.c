// Darwin's "don't fragment" socket options. Darling's socket-option table does
// not know them and answers ENOTSUP without asking the host. The Roblox client
// sets IP_DONTFRAG on its game UDP socket (path-MTU discovery for its QUIC
// transport) and abandons the connection when the call fails, so no game can
// be joined.
//
// IP_DONTFRAG (xnu ip_output): DF is set on every datagram, and a datagram
// larger than the outgoing interface's MTU fails with EMSGSIZE; the route's
// cached path MTU is not consulted. Linux has no boolean for this. The same
// behaviour is IP_MTU_DISCOVER = IP_PMTUDISC_PROBE (IP_PMTUDISC_DO would also
// refuse sends above the kernel's cached path MTU, which Darwin does not).
// Clearing the option maps to IP_PMTUDISC_DONT: DF never set, fragmentation
// allowed, as on Darwin.
//
// IPV6_DONTFRAG is the RFC 3542 option on both systems, with the same number
// and the same int boolean, so it goes to the host unchanged.
//
// Every other option keeps Darling's implementation.
#define __APPLE_USE_RFC_3542 1  // IPV6_DONTFRAG
#include <sys/socket.h>
#include <netinet/in.h>
#include <dlfcn.h>
#include <errno.h>

enum { linux_setsockopt = 54, linux_getsockopt = 55,
       linux_ipproto_ip = 0, linux_ipproto_ipv6 = 41,
       linux_ip_mtu_discover = 10, linux_ipv6_dontfrag = 62,
       linux_pmtudisc_dont = 0, linux_pmtudisc_do = 2, linux_pmtudisc_probe = 3 };

// Guest descriptors are the host's descriptors.
static long linux_sockopt(long number, int fd, int level, int name, const void *value, long length) {
    long result;
    register long r10 __asm__("r10") = (long)value;
    register long r8 __asm__("r8") = length;
    __asm__ volatile("syscall" : "=a"(result)
        : "a"(number), "D"((long)fd), "S"((long)level), "d"((long)name), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory", "cc");
    return result;
}

// Sets errno from a negative Linux result. Only the errors these two host
// calls produce need translating; the rest of 1..34 is numbered alike.
static int finish(long result) {
    if (result >= 0) return 0;
    switch (-result) {
    case 9: errno = EBADF; break;
    case 14: errno = EFAULT; break;
    case 88: errno = ENOTSOCK; break;
    case 92: errno = ENOPROTOOPT; break;
    case 95: errno = EOPNOTSUPP; break;
    default: errno = EINVAL; break;
    }
    return -1;
}

#define ORIGINAL(name, type) \
    static __typeof__(type) original; __typeof__(type) call = __atomic_load_n(&original, __ATOMIC_ACQUIRE); \
    if (!call) { call = (__typeof__(type))dlsym(RTLD_NEXT, name); __atomic_store_n(&original, call, __ATOMIC_RELEASE); }

int setsockopt(int fd, int level, int name, const void *value, socklen_t length) {
    if (level == IPPROTO_IP && name == IP_DONTFRAG) {
        if (!value) { errno = EFAULT; return -1; }
        if (length < sizeof(int)) { errno = EINVAL; return -1; }
        int mode = *(const int *)value ? linux_pmtudisc_probe : linux_pmtudisc_dont;
        return finish(linux_sockopt(linux_setsockopt, fd, linux_ipproto_ip, linux_ip_mtu_discover, &mode, sizeof mode));
    }
    if (level == IPPROTO_IPV6 && name == IPV6_DONTFRAG)
        return finish(linux_sockopt(linux_setsockopt, fd, linux_ipproto_ipv6, linux_ipv6_dontfrag, value, length));
    ORIGINAL("setsockopt", int (*)(int, int, int, const void *, socklen_t))
    return call(fd, level, name, value, length);
}

int getsockopt(int fd, int level, int name, void *restrict value, socklen_t *restrict length) {
    if (level == IPPROTO_IP && name == IP_DONTFRAG) {
        // The host reports the mode; Darwin callers expect a boolean int.
        if (!value || !length) { errno = EFAULT; return -1; }
        if (*length < sizeof(int)) { errno = EINVAL; return -1; }
        int mode = 0;
        socklen_t size = sizeof mode;
        if (finish(linux_sockopt(linux_getsockopt, fd, linux_ipproto_ip, linux_ip_mtu_discover, &mode, (long)&size))) return -1;
        *(int *)value = mode == linux_pmtudisc_do || mode == linux_pmtudisc_probe;
        *length = sizeof(int);
        return 0;
    }
    if (level == IPPROTO_IPV6 && name == IPV6_DONTFRAG)
        return finish(linux_sockopt(linux_getsockopt, fd, linux_ipproto_ipv6, linux_ipv6_dontfrag, value, (long)length));
    ORIGINAL("getsockopt", int (*)(int, int, int, void *, socklen_t *))
    return call(fd, level, name, value, length);
}
