/* Exercise the actual CLI poll loop with a live/dead server and guest socket. */
#define _GNU_SOURCE
#define INSTALL_PREFIX "/usr"
#define main darling_main
#include "darling.c"
#undef main
#include <assert.h>
#include <sys/wait.h>

int main(void)
{
    alarm(10); /* A missing server-exit wakeup must fail, not hang the build. */
    for (int mode = 0; mode < 3; ++mode) {
        int sockets[2], status;
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        pid_t server = fork();
        assert(server >= 0);
        if (!server) {
            close(sockets[0]); close(sockets[1]);
            for (;;) pause();
        }
        server_pidfd = syscall(SYS_pidfd_open, server, 0);
        assert(server_pidfd >= 0);
        if (mode == 2) {
            assert(kill(server, SIGKILL) == 0);
            assert(waitpid(server, &status, 0) == server);
        }
        pid_t cli = fork();
        assert(cli >= 0);
        if (!cli) {
            close(sockets[1]);
            shellLoop(sockets[0], -1);
            _exit(99);
        }
        close(sockets[0]);
        if (mode == 0) {
            int code = 7;
            assert(write(sockets[1], &code, sizeof(code)) == sizeof(code));
        } else if (mode == 1) {
            usleep(50000);
            assert(kill(server, SIGKILL) == 0);
        }
        /* Keep the guest socket open: a dead server cannot deliver its exit. */
        assert(waitpid(cli, &status, 0) == cli);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == (mode ? 1 : 7));
        close(sockets[1]); close(server_pidfd);
        if (mode != 2) {
            kill(server, SIGKILL);
            assert(waitpid(server, &status, 0) == server);
        }
    }
    puts("PASS CLI guest exit, server failure, and server failure before polling");
}
