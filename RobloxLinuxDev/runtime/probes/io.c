// Check file/pipe I/O cancellation and WebRTC's wakeup-pipe pattern.
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int channel[2], ready, release, cleaned, mode, disabled, file;
static char path[] = "/tmp/rbx-io.XXXXXX", created[sizeof(path) + 4];
static void cleanup(void *unused) { (void)unused; cleaned = 1; }
static void *worker(void *unused) {
    (void)unused;
    char byte = 0;
    pthread_cleanup_push(cleanup, NULL);
    if (disabled) assert(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL));
    __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&release, __ATOMIC_ACQUIRE)) __builtin_ia32_pause();
    if (mode == 3) { struct pollfd fd = {channel[0], POLLIN, 0}; assert(poll(&fd, 1, 0) == 0); }
    else if (mode == 2) assert(write(channel[1], &byte, 1) == 1);
    else if (mode == 4 || mode == 5) {
        int fd = mode == 4 ? open(path, O_RDONLY) : open(created, O_CREAT | O_EXCL | O_RDWR, 0640);
        assert(disabled && fd >= 0);
        assert(!close(fd));
    }
    else if (mode == 6) assert(pread(file, &byte, 1, 0) == 1);
    else if (mode == 7) assert(pwrite(file, &byte, 1, 0) == 1);
    else assert(read(channel[0], &byte, 1) == 1);
    assert(disabled); // Enabled cancellation must happen inside the I/O call.
    assert(!pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL));
    pthread_testcancel();
    assert(0);
    pthread_cleanup_pop(1);
    return NULL;
}

int main(void) {
    alarm(10); // A cancellation regression fails instead of hanging the check.
    char buffer[65536] = {0};
    mode_t old_mask = umask(0022);
    assert((file = mkstemp(path)) >= 0);
    snprintf(created, sizeof(created), "%s.new", path);
    assert(open(created, O_RDONLY) == -1 && errno == ENOENT);
    int opened = open(created, O_CREAT | O_EXCL | O_RDWR, 0640);
    assert(opened >= 0);
    struct stat st;
    assert(!fstat(opened, &st) && (st.st_mode & 0777) == 0640);
    assert(open(created, O_CREAT | O_EXCL | O_RDWR, 0600) == -1 && errno == EEXIST);
    assert(!close(opened) && !unlink(created));
    assert((opened = open(path, O_RDONLY)) >= 0 && !close(opened));
    assert(write(file, "abcdef", 6) == 6);
    assert(lseek(file, 2, SEEK_SET) == 2);
    assert(pread(file, buffer, 4, 3) == 3 && !memcmp(buffer, "def", 3));
    assert(pread(file, buffer, 1, 6) == 0);
    assert(pwrite(file, "XY", 2, 3) == 2);
    assert(lseek(file, 0, SEEK_CUR) == 2);
    assert(pread(file, buffer, 6, 0) == 6 && !memcmp(buffer, "abcXYf", 6));
    assert(pread(-1, buffer, 1, 0) == -1 && errno == EBADF);
    assert(pwrite(-1, buffer, 1, 0) == -1 && errno == EBADF);
    assert(pread(file, buffer, 1, -1) == -1 && errno == EINVAL);
    assert(pwrite(file, buffer, 1, -1) == -1 && errno == EINVAL);
    assert(read(-1, buffer, 1) == -1 && errno == EBADF);
    assert(write(-1, buffer, 1) == -1 && errno == EBADF);
    assert(!pipe(channel));
    struct pollfd polled = {channel[0], POLLIN, 0};
    assert(poll(&polled, 1, 0) == 0);
    uint64_t timeout_start = mach_absolute_time();
    assert(poll(NULL, 0, 20) == 0);
    assert(mach_absolute_time() - timeout_start >= 19000000);
    assert(!fcntl(channel[0], F_SETFL, O_NONBLOCK));
    assert(read(channel[0], buffer, 1) == -1 && errno == EAGAIN);
    assert(!fcntl(channel[1], F_SETFL, O_NONBLOCK));
    while (write(channel[1], buffer, sizeof(buffer)) > 0) {}
    assert(errno == EAGAIN);
    assert(read(channel[0], buffer, 4096) == 4096);
    ssize_t count = write(channel[1], buffer, sizeof(buffer));
    assert(count > 0 && count < sizeof(buffer));
    assert(poll(&polled, 1, 0) == 1 && (polled.revents & POLLIN));
    while (read(channel[0], buffer, sizeof(buffer)) > 0) {}
    assert(errno == EAGAIN);
    close(channel[1]);
    assert(read(channel[0], buffer, 1) == 0);
    assert(poll(&polled, 1, 0) == 1 && (polled.revents & POLLHUP));
    close(channel[0]);
    assert(poll(&polled, 1, 0) == 1 && (polled.revents & POLLNVAL));
    for (mode = 1; mode <= 7; ++mode) for (disabled = 0; disabled <= 1; ++disabled) {
        assert(!pipe(channel));
        if (mode == 1) assert(write(channel[1], buffer, 1) == 1);
        ready = release = cleaned = 0;
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, worker, NULL));
        while (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) __builtin_ia32_pause();
        assert(!pthread_cancel(thread));
        __atomic_store_n(&release, 1, __ATOMIC_RELEASE);
        void *result;
        assert(!pthread_join(thread, &result));
        assert(result == PTHREAD_CANCELED && cleaned);
        if (mode == 5) {
            if (disabled) {
                assert(!stat(created, &st) && (st.st_mode & 0777) == 0640);
                assert(!unlink(created));
            } else assert(stat(created, &st) == -1 && errno == ENOENT);
        }
        close(channel[0]); close(channel[1]);
    }
    assert(!close(file) && !unlink(path));
    umask(old_mask);
    // Thread creation above activates Darling's threaded cancellation path.
    assert(!pipe(channel));
    uint64_t start = mach_absolute_time();
    for (int i = 0; i < 512; ++i) {
        int sent = i, received = -1;
        assert(write(channel[1], &sent, sizeof(sent)) == sizeof(sent));
        struct pollfd fd = {channel[0], POLLIN, 0};
        assert(poll(&fd, 1, 0) == 1 && (fd.revents & POLLIN));
        assert(read(channel[0], &received, sizeof(received)) == sizeof(received));
        assert(received == sent);
    }
    double ms = (mach_absolute_time() - start) / 1e6;
    close(channel[0]); close(channel[1]);
    printf("PASS file modes, positioned I/O, errors, EOF, partial I/O, poll readiness/timeout, pending and disabled open/pread/pwrite/read/write/poll cancellation; 512 pipe cycles %.3f ms\n", ms);
    return 0;
}
