// Exercise the real Darling queue, including its nonblocking fallback.
#include <darlingserver/message.hpp>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <csignal>
#include <unistd.h>

using namespace DarlingServer;

static Message message(char value) {
    Message result(1, 0);
    result.data()[0] = value;
    return result;
}

static char receive(int fd) {
    char value;
    assert(recv(fd, &value, 1, MSG_DONTWAIT) == 1);
    return value;
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0);
    MessageQueue queue;
    int wakeups = 0, callbacks = 0;
    queue.setMessageArrivalNotificationCallback([&] { ++wakeups; });
    queue.pushOrSend(sockets[0], message('a'));
    assert(queue.empty() && wakeups == 0 && receive(sockets[1]) == 'a');

    auto deferred = message('b');
    deferred.setSendNotificationCallback([&] { ++callbacks; });
    queue.pushOrSend(sockets[0], std::move(deferred));
    queue.pushOrSend(sockets[0], message('c'));
    assert(!queue.empty() && wakeups == 2 && callbacks == 0);
    char value;
    assert(recv(sockets[1], &value, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    assert(queue.sendMany(sockets[0]));
    assert(queue.empty() && callbacks == 1);
    assert(receive(sockets[1]) == 'b' && receive(sockets[1]) == 'c');

    // Saturate the socket: the reply must queue without blocking or disappearing.
    while (send(sockets[0], "x", 1, MSG_DONTWAIT) == 1) {}
    assert(errno == EAGAIN);
    queue.pushOrSend(sockets[0], message('d'));
    assert(!queue.empty() && wakeups == 3);
    while (recv(sockets[1], &value, 1, MSG_DONTWAIT) == 1) assert(value == 'x');
    assert(errno == EAGAIN);
    assert(queue.sendMany(sockets[0]));
    assert(queue.empty() && receive(sockets[1]) == 'd');
    close(sockets[1]);
    queue.pushOrSend(sockets[0], message('e'));
    assert(queue.empty());
    close(sockets[0]);
    puts("PASS server replies: immediate send, ordered callback fallback, full socket, dead peer");
}
