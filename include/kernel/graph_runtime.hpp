// Photospider kernel: GraphRuntime per-graph worker thread and resources
#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

#include "node_graph.hpp"

namespace ps {

// Encapsulates a single loaded graph with its own resources and worker thread.
class GraphRuntime {
public:
    struct Info {
        std::string name;            // logical name or id
        std::filesystem::path root;  // per-graph root folder
        std::filesystem::path yaml;  // YAML path for this graph (copied/symlinked under root)
        std::filesystem::path config;// optional config path for this graph (not edited by kernel)
    };

    explicit GraphRuntime(const Info& info);
    ~GraphRuntime();

    // Non-copyable
    GraphRuntime(const GraphRuntime&) = delete;
    GraphRuntime& operator=(const GraphRuntime&) = delete;

    // Control
    void start();
    void stop();
    bool running() const { return running_; }

    // Schedule a task on the graph thread and get a future result.
    template<typename Fn>
    auto post(Fn&& fn) -> std::future<decltype(fn(std::declval<NodeGraph&>()))> {
        using Ret = decltype(fn(std::declval<NodeGraph&>()));
        auto task = std::make_shared<std::packaged_task<Ret()>>(
            [this, f = std::forward<Fn>(fn)](){ return f(graph_); }
        );
        std::future<Ret> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push([task]{ (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    // Access immutable info
    const Info& info() const { return info_; }

private:
    void run_loop();

    Info info_;
    NodeGraph graph_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> queue_;
};

} // namespace ps

