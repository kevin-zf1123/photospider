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

#include "graph_model.hpp"
#include "kernel/services/graph_event_service.hpp"

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

enum class TaskPriority { Normal, High };

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
    auto post(Fn&& fn) -> std::future<decltype(fn(std::declval<GraphModel&>()))> {
        using Ret = decltype(fn(std::declval<GraphModel&>()));
        auto task = std::make_shared<std::packaged_task<Ret()>>(
            [this, f = std::forward<Fn>(fn)](){ 
                if constexpr (!std::is_void_v<Ret>) {
                    return f(model_);
                } else {
                    f(model_);
                }
            }
        );
        std::future<Ret> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(global_queues_mutex_);
            normal_priority_queue_.push([task]{ (*task)(); });
            normal_enqueued_.fetch_add(1, std::memory_order_relaxed);
            ready_task_count_.fetch_add(1, std::memory_order_release);
        }
        if (sleeping_thread_count_.load(std::memory_order_acquire) > 0) {
            cv_task_available_.notify_one();
        }
        return fut;
    }
    
    std::vector<GraphEventService::ComputeEvent> drain_compute_events_now() {
        return event_service_.drain();
    }

    const Info& info() const { return info_; }
    GraphModel& model() { return model_; }
    GraphEventService& event_service() { return event_service_; }
    
    // [核心修改] 任务提交与执行接口
    void submit_initial_tasks(std::vector<Task>&& tasks, int total_task_count, TaskPriority priority = TaskPriority::Normal);
    void submit_ready_task_from_worker(Task&& task, TaskPriority priority = TaskPriority::Normal);
    void submit_ready_task_any_thread(Task&& task, TaskPriority priority = TaskPriority::Normal);
    void wait_for_completion();
    void set_exception(std::exception_ptr e);
    
    void dec_graph_tasks_to_complete();
    // Increment outstanding tasks in-flight; used when a node "kickoff" spawns micro-tasks lazily.
    void inc_graph_tasks_to_complete(int delta);

    static int this_worker_id();

    id get_metal_device();
    id get_metal_command_queue();

private:
    void run_loop(int thread_id);
    std::optional<Task> steal_task(int stealer_id);

    Info info_;
    GraphModel model_;
    GraphEventService event_service_;
    
    std::vector<std::thread> workers_;
    unsigned int num_workers_{0};
    std::atomic<bool> running_{false};

    std::vector<std::deque<Task>> local_task_queues_; // normal priority local queues
    std::vector<std::unique_ptr<std::mutex>> local_queue_mutexes_;
    
    // Phase 1: dual-priority global queues
    std::queue<Task> high_priority_queue_;
    std::queue<Task> normal_priority_queue_;
    std::mutex global_queues_mutex_;
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

    // Minimal metrics for priority effectiveness (Phase 1 observability)
    std::atomic<uint64_t> high_enqueued_{0};
    std::atomic<uint64_t> normal_enqueued_{0};
    std::atomic<uint64_t> high_executed_{0};
    std::atomic<uint64_t> normal_executed_{0};
};

} // namespace ps
