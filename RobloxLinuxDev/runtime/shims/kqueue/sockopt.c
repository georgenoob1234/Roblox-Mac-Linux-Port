// Darwin's "don't fragment" socket options. Darling's socket-option table does
// not know them and answers ENOTSUP without asking the host. The Roblox client
// sets IP_DONTFRAG on its game UDP socket (path-MTU discovery for its QUIC
// transport) and abandons the connection when the call fails, so no game can
// be joined.
//
// What xnu does (bsd/netinet/ip_output.c, udp_usrreq.c, raw_ip.c and
// bsd/netinet6/ip6_output.c):
// - IP_DONTFRAG: UDP and raw sockets set DF on every datagram, and a datagram
//   larger than the outgoing interface's MTU fails with EMSGSIZE. The route's
//   cached path MTU is not consulted. TCP never reads the flag.
// - IPV6_DONTFRAG: a datagram larger than the link MTU fails with EMSGSIZE and
//   anything smaller is sent unfragmented, again without consulting the path
//   MTU. The option is ignored for TCP. The value must be exactly an int.
//
// On Linux "never fragment, limited by the device MTU only" is the PROBE mode
// of IP_MTU_DISCOVER / IPV6_MTU_DISCOVER; the DO mode, and IPV6_DONTFRAG on its
// own, also refuse sends above the kernel's cached path MTU. IPv6 additionally
// needs Linux's own IPV6_DONTFRAG, or oversized datagrams are fragmented at
// the source. Clearing an option restores "fragmentation allowed": the DONT
// mode for IPv4 (DF never set, as on Darwin) and Linux's default WANT mode for
// IPv6, where routers never fragment anyway.
//
// Stream sockets are left alone: the PROBE mode would make Linux TCP ignore
// the path MTU, which xnu's flag never does. Reading the option back on a
// stream socket therefore reports 0.
//
// Every other option keeps Darling's implementation.
#define __APPLE_USE_RFC_3542 1  // IPV6_DONTFRAG
#include <sys/socket.h>
#include <netinet/in.h>
#include <dlfcn.h>
#include <errno.h>

enum { linux_setsockopt = 54, linux_getsockopt = 55,
       linux_sol_socket = 1, linux_so_type = 3, linux_sock_stream = 1,
       linux_ipproto_ip = 0, linux_ip_mtu_discover = 10,
       linux_ipproto_ipv6 = 41, linux_ipv6_mtu_discover = 23, linux_ipv6_dontfrag = 62,
       linux_pmtudisc_dont = 0, linux_pmtudisc_want = 1, linux_pmtudisc_do = 2, linux_pmtudisc_probe = 3 };

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
static long host_set(int fd, int level, int name, int value) {
    return linux_sockopt(linux_setsockopt, fd, level, name, &value, sizeof value);
}
static long host_get(int fd, int level, int name, int *value) {
    unsigned size = sizeof *value;
    return linux_sockopt(linux_getsockopt, fd, level, name, value, (long)&size);
}

// Sets errno from a negative Linux result. Only the errors these host calls
// produce need translating.
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
    int ipv4 = level == IPPROTO_IP && name == IP_DONTFRAG;
    if (ipv4 || (level == IPPROTO_IPV6 && name == IPV6_DONTFRAG)) {
        if (!value) { errno = EFAULT; return -1; }
        if (ipv4 ? length < sizeof(int) : length != sizeof(int)) { errno = EINVAL; return -1; }
        int on = *(const int *)value != 0, type = 0;
        long result = host_get(fd, linux_sol_socket, linux_so_type, &type);
        if (result < 0) return finish(result);
        if (type == linux_sock_stream) return 0;
        if (ipv4)
            return finish(host_set(fd, linux_ipproto_ip, linux_ip_mtu_discover, on ? linux_pmtudisc_probe : linux_pmtudisc_dont));
        result = host_set(fd, linux_ipproto_ipv6, linux_ipv6_mtu_discover, on ? linux_pmtudisc_probe : linux_pmtudisc_want);
        if (result < 0) return finish(result);
        return finish(host_set(fd, linux_ipproto_ipv6, linux_ipv6_dontfrag, on));
    }
    ORIGINAL("setsockopt", int (*)(int, int, int, const void *, socklen_t))
    return call(fd, level, name, value, length);
}

int getsockopt(int fd, int level, int name, void *restrict value, socklen_t *restrict length) {
    int ipv4 = level == IPPROTO_IP && name == IP_DONTFRAG;
    if (ipv4 || (level == IPPROTO_IPV6 && name == IPV6_DONTFRAG)) {
        if (!value || !length) { errno = EFAULT; return -1; }
        if (*length < sizeof(int)) { errno = EINVAL; return -1; }
        int state = 0;
        long result = ipv4 ? host_get(fd, linux_ipproto_ip, linux_ip_mtu_discover, &state)
                           : host_get(fd, linux_ipproto_ipv6, linux_ipv6_dontfrag, &state);
        if (result < 0) return finish(result);
        // IPv4: the host reports the mode; Darwin callers expect a boolean.
        *(int *)value = ipv4 ? state == linux_pmtudisc_do || state == linux_pmtudisc_probe : state != 0;
        *length = sizeof(int);
        return 0;
    }
    ORIGINAL("getsockopt", int (*)(int, int, int, void *, socklen_t *))
    return call(fd, level, name, value, length);
}
