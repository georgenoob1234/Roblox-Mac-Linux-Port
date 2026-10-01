// c++ -std=c++17 -O2 -pthread pipeline-workers-check.cpp -o /tmp/pipeline-workers-check
#include <stdexcept>
#include "indium/pipeline-workers.hpp"
#include <atomic>
#include <cassert>
#include <cstdio>

int main() {
    using namespace std::chrono_literals;
    Indium::PipelineWorkers pool(1);
    std::promise<void> entered, release;
    auto gate = release.get_future();
    auto running = pool.submit(std::packaged_task<void()>([&] { entered.set_value(); gate.wait(); }));
    entered.get_future().wait();
    std::atomic<unsigned> done{};
    std::vector<std::future<void>> queued;
    for (unsigned i = 0; i < 64; ++i)
        queued.push_back(pool.submit(std::packaged_task<void()>([&] { ++done; })));
    auto overflow = std::async(std::launch::async, [&] {
        return pool.submit(std::packaged_task<void()>([&] { ++done; }));
    });
    const bool returned = overflow.wait_for(100ms) == std::future_status::ready;
    release.set_value(); // Drain even when testing the broken, blocking implementation.
    auto rejected = overflow.get();
    running.get();
    for (auto& job : queued) { assert(job.valid()); job.get(); }
    assert(returned && !rejected.valid() && done == 64);
    auto failure = pool.submit(std::packaged_task<void()>([] { throw std::runtime_error("expected"); }));
    bool caught = false;
    try { failure.get(); } catch (const std::runtime_error&) { caught = true; }
    assert(caught);
    Indium::PipelineWorkers disabled(0);
    assert(!disabled.submit(std::packaged_task<void()>([] {})).valid());
    pool.stop();
    caught = false;
    try { pool.submit(std::packaged_task<void()>([] {})); } catch (const std::runtime_error&) { caught = true; }
    assert(caught);
    std::puts("PASS: saturated compiler queue never waits; accepted work drains; errors propagate; zero workers and shutdown");
}
