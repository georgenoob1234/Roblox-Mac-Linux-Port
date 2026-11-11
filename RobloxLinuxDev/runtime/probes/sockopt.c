// Check Darwin's "don't fragment" socket options (shims/kqueue/sockopt.c).
// The Roblox client sets IP_DONTFRAG on its game UDP socket and abandons the
// connection when the call fails, so a runtime that rejects the option cannot
// join any game.
//
// usage: sockopt-check [SIZE]
// With SIZE (a datagram length above the loopback MTU, which check-sockopt.py
// lowers inside a private network namespace) the probe also sends: SIZE bytes
// must go out fragmented while the option is off and fail with EMSGSIZE while
// it is on.
#define __APPLE_USE_RFC_3542 1  // IPV6_DONTFRAG
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if !defined(__x86_64__)
#error this probe reads the host socket state with x86_64 Linux system calls
#endif

static int failures;
#define CHECK(condition, ...) do { if (!(condition)) { failures++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

// The host's view of the socket: Linux getsockopt (55) on the same descriptor.
static int host_option(int fd, int level, int name) {
    int value = -1;
    unsigned length = sizeof value;
    long status;
    register long r10 __asm__("r10") = (long)&value;
    register long r8 __asm__("r8") = (long)&length;
    __asm__ volatile("syscall" : "=a"(status)
        : "a"(55L), "D"((long)fd), "S"((long)level), "d"((long)name), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory", "cc");
    return status ? (int)status : value;
}
static int guest_option(int fd, int level, int name) {
    int value = -1;
    socklen_t length = sizeof value;
    errno = 0;
    return getsockopt(fd, level, name, &value, &length) ? -errno : value;
}

enum { PMTUDISC_DONT = 0, PMTUDISC_WANT = 1, PMTUDISC_PROBE = 3 };  // Linux IP_MTU_DISCOVER / IPV6_MTU_DISCOVER modes
struct family {
    const char *label, *option;
    int domain, level, name;                        // Darwin
    int host_level, host_mode, host_flag, off_mode;  // Linux; host_flag 0: the mode is the only state
    const struct sockaddr *discard;
    socklen_t discard_length;
    int exact_length;                               // xnu takes exactly an int here, not "at least"
};

static char datagram[65507];
// 0 sent whole, EMSGSIZE refused, anything else is unexpected.
static int send_result(int fd, const struct family *f, size_t size) {
    errno = 0;
    return sendto(fd, datagram, size, 0, f->discard, f->discard_length) == (ssize_t)size ? 0 : errno;
}

static void check_family(const struct family *f, size_t oversize) {
    int on = 1, off = 0, result;
    short narrow = 1;
    long wide = 1;
    int fd = socket(f->domain, SOCK_DGRAM, 0);
    if (fd < 0) { printf("SKIP %s: socket errno %d\n", f->label, errno); return; }

    CHECK(guest_option(fd, f->level, f->name) == 0, "%s: new socket reports %s=%d, expected 0", f->label, f->option, guest_option(fd, f->level, f->name));
    int sends = oversize && send_result(fd, f, 512) == 0;
    if (oversize && !sends) printf("SKIP %s sends: loopback refused a 512-byte datagram (errno %d)\n", f->label, errno);
    if (sends) CHECK((result = send_result(fd, f, oversize)) == 0, "%s: %zu bytes were not sent while %s was off (errno %d)", f->label, oversize, f->option, result);

    errno = 0;
    CHECK(setsockopt(fd, f->level, f->name, &on, sizeof on) == 0, "%s: setsockopt(%s, 1): errno %d (%s)", f->label, f->option, errno, strerror(errno));
    CHECK(guest_option(fd, f->level, f->name) == 1, "%s: %s reads back %d after enabling", f->label, f->option, guest_option(fd, f->level, f->name));
    CHECK(host_option(fd, f->host_level, f->host_mode) == PMTUDISC_PROBE, "%s: host MTU discovery mode is %d after enabling, expected %d",
          f->label, host_option(fd, f->host_level, f->host_mode), PMTUDISC_PROBE);
    if (f->host_flag) CHECK(host_option(fd, f->host_level, f->host_flag) == 1, "%s: host %s is %d after enabling", f->label, f->option, host_option(fd, f->host_level, f->host_flag));
    if (sends) {
        CHECK((result = send_result(fd, f, oversize)) == EMSGSIZE, "%s: %zu bytes were not refused with EMSGSIZE while %s was on (result %d)", f->label, oversize, f->option, result);
        CHECK((result = send_result(fd, f, 512)) == 0, "%s: 512 bytes were not sent while %s was on (errno %d)", f->label, f->option, result);
    }

    errno = 0;
    CHECK(setsockopt(fd, f->level, f->name, &off, sizeof off) == 0, "%s: setsockopt(%s, 0): errno %d (%s)", f->label, f->option, errno, strerror(errno));
    CHECK(guest_option(fd, f->level, f->name) == 0, "%s: %s reads back %d after disabling", f->label, f->option, guest_option(fd, f->level, f->name));
    CHECK(host_option(fd, f->host_level, f->host_mode) == f->off_mode, "%s: host MTU discovery mode is %d after disabling, expected %d",
          f->label, host_option(fd, f->host_level, f->host_mode), f->off_mode);
    if (f->host_flag) CHECK(host_option(fd, f->host_level, f->host_flag) == 0, "%s: host %s is %d after disabling", f->label, f->option, host_option(fd, f->host_level, f->host_flag));
    if (sends) CHECK((result = send_result(fd, f, oversize)) == 0, "%s: %zu bytes were not sent after %s was cleared (errno %d)", f->label, oversize, f->option, result);

    errno = 0;
    CHECK(setsockopt(fd, f->level, f->name, &narrow, sizeof narrow) == -1 && errno == EINVAL, "%s: a 2-byte %s value was not rejected with EINVAL (errno %d)", f->label, f->option, errno);
    if (f->exact_length) {
        errno = 0;
        CHECK(setsockopt(fd, f->level, f->name, &wide, sizeof wide) == -1 && errno == EINVAL, "%s: an 8-byte %s value was not rejected with EINVAL (errno %d)", f->label, f->option, errno);
    }
    close(fd);

    // xnu's TCP never reads either flag; the host's stream socket must keep its MTU discovery mode.
    fd = socket(f->domain, SOCK_STREAM, 0);
    CHECK(fd >= 0, "%s: stream socket errno %d", f->label, errno);
    if (fd >= 0) {
        int before = host_option(fd, f->host_level, f->host_mode);
        errno = 0;
        CHECK(setsockopt(fd, f->level, f->name, &on, sizeof on) == 0, "%s: setsockopt(%s, 1) on a stream socket: errno %d", f->label, f->option, errno);
        CHECK(host_option(fd, f->host_level, f->host_mode) == before, "%s: a stream socket's host MTU discovery mode changed from %d to %d",
              f->label, before, host_option(fd, f->host_level, f->host_mode));
        close(fd);
    }
}

int main(int argc, char **argv) {
    size_t oversize = argc > 1 ? strtoul(argv[1], NULL, 10) : 0;
    if (oversize > sizeof datagram) { printf("FAIL SIZE above %zu\n", sizeof datagram); return 2; }
    struct sockaddr_in discard4 = { .sin_len = sizeof discard4, .sin_family = AF_INET, .sin_port = htons(9), .sin_addr = { htonl(INADDR_LOOPBACK) } };
    struct sockaddr_in6 discard6 = { .sin6_len = sizeof discard6, .sin6_family = AF_INET6, .sin6_port = htons(9), .sin6_addr = IN6ADDR_LOOPBACK_INIT };
    const struct family ipv4 = { "IPv4", "IP_DONTFRAG", AF_INET, IPPROTO_IP, IP_DONTFRAG, 0, 10, 0, PMTUDISC_DONT,
                                 (const struct sockaddr *)&discard4, sizeof discard4, 0 };
    const struct family ipv6 = { "IPv6", "IPV6_DONTFRAG", AF_INET6, IPPROTO_IPV6, IPV6_DONTFRAG, 41, 23, 62, PMTUDISC_WANT,
                                 (const struct sockaddr *)&discard6, sizeof discard6, 1 };
    check_family(&ipv4, oversize);
    check_family(&ipv6, oversize);

    // Other options must still reach Darling's conversion table.
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    errno = 0;
    CHECK(fd >= 0 && setsockopt(fd, IPPROTO_IP, IP_TTL, &(int){64}, sizeof(int)) == 0 && guest_option(fd, IPPROTO_IP, IP_TTL) == 64,
          "IP_TTL no longer round-trips (errno %d)", errno);
    if (fd >= 0) close(fd);

    if (failures) return 1;
    printf("PASS sockopt: IP_DONTFRAG and IPV6_DONTFRAG select the host's PROBE mode on datagram sockets, read back as booleans%s\n",
           oversize ? " and refuse oversized datagrams only while set" : "; sends not exercised");
    return 0;
}
