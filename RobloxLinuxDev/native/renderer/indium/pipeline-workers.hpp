#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
#include <unistd.h>

namespace Indium {
// Only pending graphics PSOs use this queue. Offline Metal translation already
// runs in parallel. Keep the queue bounded instead of spawning a thread per PSO.
class PipelineWorkers {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::packaged_task<void()>> jobs;
    // Speculative work (shader libraries of newly loaded shaders): runs only
    // while no pipeline state is waiting, and never blocks the submitter.
    std::deque<std::function<void()>> background;
    std::vector<std::thread> threads;
    bool stopping = false;
public:
    static unsigned count() {
        static const unsigned value = [] {
            const long cpus = std::max(1L, sysconf(_SC_NPROCESSORS_ONLN));
            unsigned count = std::min(4L, cpus);
            if (const char* text = std::getenv("RBX_PIPELINE_WORKERS")) {
                char* end; const long requested = std::strtol(text, &end, 10);
                if (end != text && !*end && requested >= 0 && requested <= 32)
                    count = std::min(requested, cpus);
            }
            return count;
        }();
        return value;
    }
    explicit PipelineWorkers(unsigned count) {
        try {
            for (unsigned i = 0; i < count; ++i) threads.emplace_back([this] {
                for (;;) {
                    std::packaged_task<void()> job;
                    std::function<void()> speculative;
                    {
                        std::unique_lock lock(mutex);
                        changed.wait(lock, [&] { return stopping || !jobs.empty() || !background.empty(); });
                        if (!jobs.empty()) { job = std::move(jobs.front()); jobs.pop_front(); }
                        else if (!stopping) { speculative = std::move(background.front()); background.pop_front(); }
                        else return;
                    }
                    changed.notify_all();
                    if (job.valid()) job(); // packaged_task transfers errors to the requesting PSO.
                    else speculative();     // catches its own errors
                }
            });
        } catch (...) { stop(); throw; }
    }
    ~PipelineWorkers() { stop(); }
    void stop() {
        { std::lock_guard lock(mutex); stopping = true; }
        changed.notify_all();
        for (auto& thread : threads) if (thread.joinable()) thread.join();
    }
    std::future<void> submit(std::packaged_task<void()> job) {
        std::future<void> result;
        {
            std::lock_guard lock(mutex);
            if (stopping) throw std::runtime_error("Pipeline compiler is stopping");
            // Preparation/optimization is optional. A loading burst must not
            // park the render thread behind a full compiler queue; the caller
            // can still compile/link a rejected pipeline when it is drawn.
            if (threads.empty() || jobs.size() >= 64) return result;
            result = job.get_future();
            jobs.push_back(std::move(job));
        }
        changed.notify_all();
        return result;
    }
    void submitBackground(std::function<void()> job) {
        { std::lock_guard lock(mutex); if (stopping) return; background.push_back(std::move(job)); }
        changed.notify_all();
    }
    static PipelineWorkers& shared() { static PipelineWorkers pool(count()); return pool; }
};
}
