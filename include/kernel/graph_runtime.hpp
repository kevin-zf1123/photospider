// Photospider kernel: GraphRuntime per-graph worker thread and resources
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <vector>

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

struct ScheduledTask {
  uint64_t epoch{0};
  Task task;

  ScheduledTask() = default;
  ScheduledTask(uint64_t e, Task&& t) : epoch(e), task(std::move(t)) {}

  explicit operator bool() const { return static_cast<bool>(task); }
};

enum class TaskPriority { Normal, High };

class GraphRuntime;  // 前向声明

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

  struct SchedulerEvent {
    enum Action { ASSIGN_INITIAL, EXECUTE };
    uint64_t epoch;
    int node_id;
    int worker_id;
    Action action;
    std::chrono::time_point<std::chrono::high_resolution_clock> timestamp;
  };

  explicit GraphRuntime(const Info& info);
  ~GraphRuntime();

  GraphRuntime(const GraphRuntime&) = delete;
  GraphRuntime& operator=(const GraphRuntime&) = delete;

  void start();
  void stop();
  bool running() const { return running_; }

  template <typename Fn>
  auto post(Fn&& fn) -> std::future<decltype(fn(std::declval<GraphModel&>()))> {
    using Ret = decltype(fn(std::declval<GraphModel&>()));
    auto task = std::make_shared<std::packaged_task<Ret()>>(
        [this, f = std::forward<Fn>(fn)]() {
          if constexpr (!std::is_void_v<Ret>) {
            return f(model_);
          } else {
            f(model_);
          }
        });
    std::future<Ret> fut = task->get_future();
    submit_ready_task_any_thread([task] { (*task)(); }, TaskPriority::Normal,
                                 std::optional<uint64_t>{0});
    return fut;
  }

  std::vector<GraphEventService::ComputeEvent> drain_compute_events_now() {
    return event_service_.drain();
  }

  const Info& info() const { return info_; }
  GraphModel& model() { return model_; }
  GraphEventService& event_service() { return event_service_; }

  // [核心修改] 任务提交与执行接口
  void submit_initial_tasks(std::vector<Task>&& tasks, int total_task_count,
                            TaskPriority priority = TaskPriority::Normal);
  void submit_ready_task_from_worker(
      Task&& task, TaskPriority priority = TaskPriority::Normal);
  void submit_ready_task_any_thread(
      Task&& task, TaskPriority priority = TaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt);
  void wait_for_completion();
  void set_exception(std::exception_ptr e);

  void log_event(SchedulerEvent::Action action, int node_id);
  std::vector<SchedulerEvent> get_scheduler_log() const;
  void clear_scheduler_log();

  void dec_graph_tasks_to_complete();
  // Increment outstanding tasks in-flight; used when a node "kickoff" spawns
  // micro-tasks lazily.
  void inc_graph_tasks_to_complete(int delta);

  static int this_worker_id();
  static uint64_t this_task_epoch();
  uint64_t active_epoch() const;
  uint64_t begin_new_epoch();
  bool should_cancel_epoch(uint64_t epoch) const;
  void cancel_stale_enqueued_tasks(uint64_t min_epoch);

  id get_metal_device();
  id get_metal_command_queue();

private:
  void run_loop(int thread_id);
  std::optional<ScheduledTask> steal_task(int stealer_id);

  Info info_;
  GraphModel model_;
  GraphEventService event_service_;

  std::vector<std::thread> workers_;
  unsigned int num_workers_{0};
  std::atomic<bool> running_{false};

  std::vector<std::deque<ScheduledTask>>
      local_task_queues_;  // normal priority local queues
  std::vector<std::unique_ptr<std::mutex>> local_queue_mutexes_;

  // Phase 1: dual-priority global queues
  std::queue<ScheduledTask> high_priority_queue_;
  std::queue<ScheduledTask> normal_priority_queue_;
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
  static thread_local uint64_t tls_active_epoch_;

  std::atomic<uint64_t> epoch_counter_{0};
  std::atomic<uint64_t> active_epoch_{0};

  struct GpuContext;
  std::unique_ptr<GpuContext> gpu_context_;

  // Minimal metrics for priority effectiveness (Phase 1 observability)
  std::atomic<uint64_t> high_enqueued_{0};
  std::atomic<uint64_t> normal_enqueued_{0};
  std::atomic<uint64_t> high_executed_{0};
  std::atomic<uint64_t> normal_executed_{0};

  mutable std::mutex log_mutex_;
  std::vector<SchedulerEvent> scheduler_log_;
};

}  // namespace ps
