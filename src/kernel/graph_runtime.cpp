// Photospider kernel: GraphRuntime implementation
#include "kernel/graph_runtime.hpp"

#include <filesystem>

namespace ps {

GraphRuntime::GraphRuntime(const Info& info)
    : info_(info), graph_(info.root / "cache") {
    // Ensure folder layout exists
    std::filesystem::create_directories(info_.root);
    std::filesystem::create_directories(info_.root / "cache");
}

GraphRuntime::~GraphRuntime() { stop(); }

void GraphRuntime::start() {
    if (running_) return;
    running_ = true;
    worker_ = std::thread(&GraphRuntime::run_loop, this);
}

void GraphRuntime::stop() {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        running_ = false;
        // push a no-op to wake thread
        queue_.push([]{});
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void GraphRuntime::run_loop() {
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [&]{ return !queue_.empty(); });
            job = std::move(queue_.front());
            queue_.pop();
        }
        if (!running_) break;
        try {
            job();
        } catch (const std::exception&) {
            // Swallow exceptions to prevent worker thread from terminating the process.
            // Exceptions are propagated to futures via packaged_task when callers use get().
        } catch (...) {
            // Unknown exception type; swallow to keep runtime alive.
        }
    }
}

} // namespace ps
