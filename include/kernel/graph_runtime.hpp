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
#include <optional>

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

class GraphRuntime; // 前向声明

struct TaskGraph {
    std::map<int, Task> tasks;
    std::map<int, std::atomic<int>> dependency_counters;
    std::map<int, std::vector<int>> dependents_map;
    std::vector<int> initial_ready_nodes;
    GraphRuntime* runtime_ptr = nullptr;
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
            [this, f = std::forward<Fn>(fn)](){ 
                try {
                    if constexpr (!std::is_void_v<Ret>) {
                        return f(graph_); 
                    } else {
                        f(graph_);
                    }
                } catch(...) {
                    set_exception(std::current_exception());
                }
                if constexpr (!std::is_void_v<Ret>) {
                    return Ret{};
                }
            }
        );
        std::future<Ret> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(global_queue_mutex_);
            global_task_queue_.push([task]{ (*task)(); });
            ready_task_count_.fetch_add(1, std::memory_order_release);
        }
        if (sleeping_thread_count_.load(std::memory_order_acquire) > 0) {
            cv_task_available_.notify_one();
        }
        return fut;
    }
    
    std::vector<NodeGraph::ComputeEvent> drain_compute_events_now() {
        return graph_.drain_compute_events();
    }

    const Info& info() const { return info_; }
    NodeGraph& get_nodegraph() { return graph_; }
    
    // [核心修改] 任务提交与执行接口
    void submit_initial_tasks(std::vector<Task>&& tasks, int total_task_count);
    void submit_ready_task_from_worker(Task&& task);
    void submit_ready_task_any_thread(Task&& task);
    void wait_for_completion();
    void set_exception(std::exception_ptr e);
    
    void dec_graph_tasks_to_complete();
    // Increment outstanding tasks in-flight; used when a node "kickoff" spawns micro-tasks lazily.
    void inc_graph_tasks_to_complete(int delta);

    static int this_worker_id();

    id get_metal_device();
    id get_metal_command_queue();

private:
    friend class NodeGraph; // 允许NodeGraph访问私有成员以提交任务(临时方案)

    void run_loop(int thread_id);
    std::optional<Task> steal_task(int stealer_id);

    Info info_;
    NodeGraph graph_;
    
    std::vector<std::thread> workers_;
    unsigned int num_workers_{0};
    std::atomic<bool> running_{false};

    std::vector<std::deque<Task>> local_task_queues_;
    std::vector<std::unique_ptr<std::mutex>> local_queue_mutexes_;
    
    std::queue<Task> global_task_queue_;
    std::mutex global_queue_mutex_;
    std::condition_variable cv_task_available_;

    std::atomic<int> ready_task_count_{0};
    std::atomic<int> sleeping_thread_count_{0};

    std::mutex completion_mutex_;
    std::condition_variable cv_completion_;
    std::atomic<int> tasks_to_complete_{0};
    
    std::mutex exception_mutex_;
    std::exception_ptr first_exception_{nullptr};
    std::atomic<bool> has_exception_{false};

    static thread_local int tls_worker_id_;

    struct GpuContext;
    std::unique_ptr<GpuContext> gpu_context_;
};

} // namespace ps
