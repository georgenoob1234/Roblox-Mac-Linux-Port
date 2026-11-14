// Proves that a fatal renderer error names its cause on stderr before the process aborts.
#include <indium/fatal.hpp>

#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <cstring>
#include <functional>
#include <string>

static std::string dying(const std::function<void()>& body, int& status) {
    int fds[2];
    if (pipe(fds) != 0) std::exit(2);
    pid_t child = fork();
    if (child == 0) {
        dup2(fds[1], 2); close(fds[0]); close(fds[1]);
        body();
        _exit(0);
    }
    close(fds[1]);
    std::string text; char buffer[256]; ssize_t n;
    while ((n = read(fds[0], buffer, sizeof buffer)) > 0) text.append(buffer, size_t(n));
    close(fds[0]);
    waitpid(child, &status, 0);
    return text;
}
static void expect(const char* what, const std::function<void()>& body, const std::string& wanted) {
    int status = 0;
    auto text = dying(body, status);
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT || text != wanted) {
        std::fprintf(stderr, "FAIL %s: status=%#x output=\"%s\" wanted=\"%s\"\n", what, status, text.c_str(), wanted.c_str());
        std::exit(1);
    }
}
int main() {
    int line = 0;
    line = __LINE__; expect("failed Vulkan call", [] { INDIUM_VULKAN_FATAL("vkQueueSubmit2", VK_ERROR_DEVICE_LOST); },
        "Indium: fatal: vkQueueSubmit2 failed: VK_ERROR_DEVICE_LOST (-4) at fatal-check.cpp:" + std::to_string(line) + "\n");
    line = __LINE__; expect("out of device memory", [] { INDIUM_VULKAN_FATAL("vkAllocateMemory", VK_ERROR_OUT_OF_DEVICE_MEMORY); },
        "Indium: fatal: vkAllocateMemory failed: VK_ERROR_OUT_OF_DEVICE_MEMORY (-2) at fatal-check.cpp:" + std::to_string(line) + "\n");
    line = __LINE__; expect("result without a name", [] { INDIUM_VULKAN_FATAL("vkCreateBuffer", VkResult(-12345)); },
        "Indium: fatal: vkCreateBuffer failed: unknown VkResult (-12345) at fatal-check.cpp:" + std::to_string(line) + "\n");
    line = __LINE__; expect("broken invariant", [] { INDIUM_FATAL("the reason"); },
        "Indium: fatal: the reason at fatal-check.cpp:" + std::to_string(line) + "\n");
    std::puts("PASS a fatal renderer error names the call, the result and the place before it aborts");
}
