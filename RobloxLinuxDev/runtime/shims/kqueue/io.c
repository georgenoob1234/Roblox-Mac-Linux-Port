// Darling's cancellable I/O checks each make a server round trip.
// WebRTC uses a pipe for every network-thread wakeup, so voice updates spend
// tens of milliseconds in those checks; asset open/pread/pwrite pay it too.
// libpthread already owns the local cancellation state; keep cancellation
// points around the existing I/O calls.
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <unistd.h>

extern int open_nocancel(const char *, int, ...) __asm("_open$NOCANCEL");
extern ssize_t pread_nocancel(int, void *, size_t, off_t) __asm("_pread$NOCANCEL");
extern ssize_t pwrite_nocancel(int, const void *, size_t, off_t) __asm("_pwrite$NOCANCEL");
extern ssize_t read_nocancel(int, void *, size_t) __asm("_read$NOCANCEL");
extern ssize_t write_nocancel(int, const void *, size_t) __asm("_write$NOCANCEL");
extern int poll_nocancel(struct pollfd *, nfds_t, int) __asm("_poll$NOCANCEL");

int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
    }
    pthread_testcancel();
    int result = open_nocancel(path, flags, mode);
    if (result < 0 && errno == EINTR) pthread_testcancel();
    return result;
}

ssize_t pread(int fd, void *buffer, size_t size, off_t offset) {
    pthread_testcancel();
    ssize_t result = pread_nocancel(fd, buffer, size, offset);
    if (result < 0 && errno == EINTR) pthread_testcancel();
    return result;
}

ssize_t pwrite(int fd, const void *buffer, size_t size, off_t offset) {
    pthread_testcancel();
    ssize_t result = pwrite_nocancel(fd, buffer, size, offset);
    if (result < 0 && errno == EINTR) pthread_testcancel();
    return result;
}

ssize_t read(int fd, void *buffer, size_t size) {
    pthread_testcancel();
    ssize_t result = read_nocancel(fd, buffer, size);
    if (result < 0 && errno == EINTR) pthread_testcancel();
    return result;
}

ssize_t write(int fd, const void *buffer, size_t size) {
    pthread_testcancel();
    ssize_t result = write_nocancel(fd, buffer, size);
    if (result < 0 && errno == EINTR) pthread_testcancel();
    return result;
}

int poll(struct pollfd *fds, nfds_t count, int timeout) {
    pthread_testcancel();
    int result = poll_nocancel(fds, count, timeout);
    if (result < 0 && errno == EINTR) pthread_testcancel();
    return result;
}

// The same check is made for every other cancellable call, on every thread
// but the main one: ~70 us per call against well under 1 us for the call
// itself. In game the client's network threads make about 2,700 of the calls
// below per second (recv 1,550, fcntl 590, recvmsg 340, close 125, sendmsg 85).
extern ssize_t recvfrom_nocancel(int, void *, size_t, int, struct sockaddr *, socklen_t *) __asm("_recvfrom$NOCANCEL");
extern ssize_t sendto_nocancel(int, const void *, size_t, int, const struct sockaddr *, socklen_t) __asm("_sendto$NOCANCEL");
extern ssize_t recvmsg_nocancel(int, struct msghdr *, int) __asm("_recvmsg$NOCANCEL");
extern ssize_t sendmsg_nocancel(int, const struct msghdr *, int) __asm("_sendmsg$NOCANCEL");
extern int fcntl_nocancel(int, int, ...) __asm("_fcntl$NOCANCEL");
extern int close_nocancel(int) __asm("_close$NOCANCEL");

ssize_t recvfrom(int fd, void *buffer, size_t size, int flags, struct sockaddr *restrict address, socklen_t *restrict length) {
    pthread_testcancel();
    ssize_t result = recvfrom_nocancel(fd, buffer, size, flags, address, length);
    if (result < 0 && errno == EINTR) pthread_testcancel();
    return result;
}

ssize_t sendto(int fd, const void *buffer, size_t size, int flags, const struct sockaddr *address, socklen_t length) {
    pthread_testcancel();
    ssize_t result = sendto_nocancel(fd, buffer, size, flags, address, length);
    if (result < 0 && errno == EINTR) pthread_testcancel();
    return result;
}

// libc's recv and send enter the kernel library directly rather than through
// recvfrom and sendto above, so they need their own entry points.
ssize_t recv(int fd, void *buffer, size_t size, int flags) {
    pthread_testcancel();
    ssize_t result = recvfrom_nocancel(fd, buffer, size, flags, NULL, NULL);
    if (result < 0 && errno == EINTR) pthread_testcancel();
    return result;
}

ssize_t send(int fd, const void *buffer, size_t size, int flags) {
    pthread_testcancel();
    ssize_t result = sendto_nocancel(fd, buffer, size, flags, NULL, 0);
    if (result < 0 && errno == EINTR) pthread_testcancel();
    return result;
}

ssize_t recvmsg(int fd, struct msghdr *message, int flags) {
    pthread_testcancel();
    ssize_t result = recvmsg_nocancel(fd, message, flags);
    if (result < 0 && errno == EINTR) pthread_testcancel();
    return result;
}

ssize_t sendmsg(int fd, const struct msghdr *message, int flags) {
    pthread_testcancel();
    ssize_t result = sendmsg_nocancel(fd, message, flags);
    if (result < 0 && errno == EINTR) pthread_testcancel();
    return result;
}

// Every fcntl command takes at most one int or pointer argument.
int fcntl(int fd, int command, ...) {
    va_list args;
    va_start(args, command);
    void *argument = va_arg(args, void *);
    va_end(args);
    pthread_testcancel();
    int result = fcntl_nocancel(fd, command, argument);
    if (result < 0 && errno == EINTR) pthread_testcancel();
    return result;
}

// Cancellation is acted on before the descriptor is released, never after:
// once close has been entered the descriptor is gone even if it reports EINTR.
int close(int fd) {
    pthread_testcancel();
    return close_nocancel(fd);
}
