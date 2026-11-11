// Check Darwin's "don't fragment" socket options (shims/kqueue/sockopt.c).
// The Roblox client sets IP_DONTFRAG on its game UDP socket and abandons the
// connection when the call fails, so a runtime that rejects the option cannot
// join any game.
#define __APPLE_USE_RFC_3542 1  // IPV6_DONTFRAG
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
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

int main(void) {
    enum { LINUX_IP_MTU_DISCOVER = 10, LINUX_IP_PMTUDISC_DONT = 0, LINUX_IP_PMTUDISC_PROBE = 3, LINUX_IPV6_DONTFRAG = 62 };
    int on = 1, off = 0;
    short narrow = 1;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd >= 0, "socket(AF_INET): errno %d", errno);
    if (fd >= 0) {
        CHECK(guest_option(fd, IPPROTO_IP, IP_DONTFRAG) == 0, "new socket reports IP_DONTFRAG=%d, expected 0",
              guest_option(fd, IPPROTO_IP, IP_DONTFRAG));
        errno = 0;
        CHECK(setsockopt(fd, IPPROTO_IP, IP_DONTFRAG, &on, sizeof on) == 0, "setsockopt(IP_DONTFRAG, 1): errno %d (%s)", errno, strerror(errno));
        CHECK(guest_option(fd, IPPROTO_IP, IP_DONTFRAG) == 1, "IP_DONTFRAG reads back %d after enabling", guest_option(fd, IPPROTO_IP, IP_DONTFRAG));
        CHECK(host_option(fd, 0, LINUX_IP_MTU_DISCOVER) == LINUX_IP_PMTUDISC_PROBE, "host IP_MTU_DISCOVER is %d after enabling, expected %d",
              host_option(fd, 0, LINUX_IP_MTU_DISCOVER), LINUX_IP_PMTUDISC_PROBE);
        errno = 0;
        CHECK(setsockopt(fd, IPPROTO_IP, IP_DONTFRAG, &off, sizeof off) == 0, "setsockopt(IP_DONTFRAG, 0): errno %d (%s)", errno, strerror(errno));
        CHECK(guest_option(fd, IPPROTO_IP, IP_DONTFRAG) == 0, "IP_DONTFRAG reads back %d after disabling", guest_option(fd, IPPROTO_IP, IP_DONTFRAG));
        CHECK(host_option(fd, 0, LINUX_IP_MTU_DISCOVER) == LINUX_IP_PMTUDISC_DONT, "host IP_MTU_DISCOVER is %d after disabling, expected %d",
              host_option(fd, 0, LINUX_IP_MTU_DISCOVER), LINUX_IP_PMTUDISC_DONT);
        errno = 0;
        CHECK(setsockopt(fd, IPPROTO_IP, IP_DONTFRAG, &narrow, sizeof narrow) == -1 && errno == EINVAL,
              "a 2-byte IP_DONTFRAG value was not rejected with EINVAL (errno %d)", errno);
        // Other options must still reach the conversion table.
        errno = 0;
        CHECK(setsockopt(fd, IPPROTO_IP, IP_TTL, &(int){64}, sizeof(int)) == 0 && guest_option(fd, IPPROTO_IP, IP_TTL) == 64,
              "IP_TTL no longer round-trips (errno %d)", errno);
        close(fd);
    }

    int fd6 = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd6 < 0) {
        printf("SKIP IPv6: socket(AF_INET6) errno %d\n", errno);
    } else {
        struct sockaddr_in6 discard = { .sin6_len = sizeof discard, .sin6_family = AF_INET6, .sin6_port = htons(9), .sin6_addr = IN6ADDR_LOOPBACK_INIT };
        static char datagram[65500];  // with the 48 bytes of headers, larger than the loopback MTU (65536)
        int fragmented = sendto(fd6, datagram, sizeof datagram, 0, (struct sockaddr *)&discard, sizeof discard) == (ssize_t)sizeof datagram;
        int unfragmented_errno = errno;
        errno = 0;
        CHECK(setsockopt(fd6, IPPROTO_IPV6, IPV6_DONTFRAG, &on, sizeof on) == 0, "setsockopt(IPV6_DONTFRAG, 1): errno %d (%s)", errno, strerror(errno));
        CHECK(guest_option(fd6, IPPROTO_IPV6, IPV6_DONTFRAG) == 1, "IPV6_DONTFRAG reads back %d after enabling", guest_option(fd6, IPPROTO_IPV6, IPV6_DONTFRAG));
        CHECK(host_option(fd6, 41, LINUX_IPV6_DONTFRAG) == 1, "host IPV6_DONTFRAG is %d after enabling", host_option(fd6, 41, LINUX_IPV6_DONTFRAG));
        if (fragmented) {
            errno = 0;
            CHECK(sendto(fd6, datagram, sizeof datagram, 0, (struct sockaddr *)&discard, sizeof discard) == -1 && errno == EMSGSIZE,
                  "an over-MTU IPv6 datagram was not refused with EMSGSIZE once IPV6_DONTFRAG was set (errno %d)", errno);
        } else {
            printf("SKIP IPv6 send: loopback refused the datagram before the option was set (errno %d)\n", unfragmented_errno);
        }
        close(fd6);
    }

    if (failures) return 1;
    printf("PASS sockopt: IP_DONTFRAG maps to the host's IP_MTU_DISCOVER and reads back as a boolean; IPV6_DONTFRAG passes through\n");
    return 0;
}
