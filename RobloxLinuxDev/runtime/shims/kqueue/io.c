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
