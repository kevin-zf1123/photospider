// Photospider kernel: GraphRuntime per-graph worker thread and resources
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>
#include <map>
#include <exception>

#include "node_graph.hpp"

// [修改] 使用预处理器宏和前向声明来隔离平台特定的 Metal API
#ifdef __OBJC__
@protocol MTLDevice;
@protocol MTLCommandQueue;
#else
// 对于纯 C++ 文件，使用 void* 作为不透明指针
typedef void* id;
#endif


namespace ps {

using Task = std::function<void()>;

struct TaskGraph {
    std::map<int, Task> tasks;
    std::map<int, std::atomic<int>> dependency_counters;
    std::map<int, std::vector<int>> dependents_map;
    std::vector<int> initial_ready_nodes;
};

class GraphRuntime {
public:
    struct Info {
        std::string name;
        std::filesystem::path root;
        std::filesystem::path yaml;
        std::filesystem::path config;
    };

    explicit GraphRuntime(const Info& info);
    ~GraphRuntime();

    GraphRuntime(const GraphRuntime&) = delete;
    GraphRuntime& operator=(const GraphRuntime&) = delete;

    void start();
    void stop();
    bool running() const { return running_; }

    template<typename Fn>
    auto post(Fn&& fn) -> std::future<decltype(fn(std::declval<NodeGraph&>()))> {
        using Ret = decltype(fn(std::declval<NodeGraph&>()));
        auto task = std::make_shared<std::packaged_task<Ret()>>(
            [this, f = std::forward<Fn>(fn)](){ return f(graph_); }
        );
        std::future<Ret> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(global_queue_mutex_);
            global_task_queue_.push([task]{ (*task)(); });
        }
        cv_task_available_.notify_one();
        return fut;
    }
    
    std::vector<NodeGraph::ComputeEvent> drain_compute_events_now() {
        return graph_.drain_compute_events();
    }

    const Info& info() const { return info_; }
    NodeGraph& get_nodegraph() { return graph_; }

    void push_ready_task(Task&& task, int thread_id);
    void execute_task_graph_and_wait(std::shared_ptr<TaskGraph> task_graph);
    void notify_task_complete();
    void set_exception(std::exception_ptr e);
    
    // [新增] 获取 GPU 上下文的接口
    id get_metal_device();
    id get_metal_command_queue();

private:
    void run_loop(int thread_id);

    Info info_;
    NodeGraph graph_;
    
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};

    std::vector<std::deque<Task>> local_task_queues_;
    std::vector<std::unique_ptr<std::mutex>> local_queue_mutexes_;
    
    std::queue<Task> global_task_queue_;
    std::mutex global_queue_mutex_;
    std::condition_variable cv_task_available_;

    std::mutex completion_mutex_;
    std::condition_variable cv_completion_;
    std::atomic<int> tasks_remaining_{0};
    
    std::mutex exception_mutex_;
    std::exception_ptr first_exception_{nullptr};

    // [新增] 使用 pImpl 模式隐藏 GPU 上下文细节
    struct GpuContext;
    std::unique_ptr<GpuContext> gpu_context_;
};

} // namespace ps