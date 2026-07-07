#include <atomic>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kernel/scheduler/scheduler_plugin_api.hpp"
#include "kernel/scheduler/scheduler_task_runtime.hpp"

namespace {

std::atomic<int> g_active_count{0};
std::atomic<int> g_destroy_count{0};

class DestroyCountScheduler final : public ps::IScheduler,
                                    public ps::SchedulerTaskRuntime {
 public:
  DestroyCountScheduler() { g_active_count.fetch_add(1); }
  ~DestroyCountScheduler() override { g_active_count.fetch_sub(1); }

  void attach(ps::GraphRuntime*) override {}
  void detach() override {}
  void start() override { running_ = true; }
  void shutdown() override { running_ = false; }

  std::string name() const override { return "destroy_count_test"; }
  std::string get_stats() const override { return "destroy-count-test"; }
  bool is_running() const override { return running_; }
  bool task_runtime_running() const override { return running_; }

  void submit_initial_tasks(std::vector<Task>&& tasks, int total_task_count,
                            ps::SchedulerTaskPriority priority =
                                ps::SchedulerTaskPriority::Normal) override {
    (void)priority;
    tasks_to_complete_ = total_task_count;
    for (auto& task : tasks) {
      if (task) {
        task();
      }
    }
  }

  void submit_ready_task_from_worker(
      Task&& task, ps::SchedulerTaskPriority priority =
                       ps::SchedulerTaskPriority::Normal) override {
    submit_ready_task_any_thread(std::move(task), priority, std::nullopt);
  }

  void submit_ready_task_any_thread(
      Task&& task,
      ps::SchedulerTaskPriority priority = ps::SchedulerTaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) override {
    (void)priority;
    (void)epoch;
    if (task) {
      task();
    }
  }

  void wait_for_completion() override {}
  void set_exception(std::exception_ptr e) override { exception_ = e; }
  void inc_tasks_to_complete(int delta) override {
    tasks_to_complete_ += delta;
  }
  void dec_tasks_to_complete() override {
    if (tasks_to_complete_ > 0) {
      --tasks_to_complete_;
    }
  }
  void log_event(ps::SchedulerTraceAction, int) override {}

 private:
  bool running_ = false;
  int tasks_to_complete_ = 0;
  std::exception_ptr exception_;
};

}  // namespace

extern "C" {

int ps_scheduler_plugin_get_count() {
  return 1;
}

const char* ps_scheduler_plugin_get_name(int index) {
  return index == 0 ? "destroy_count_test" : nullptr;
}

const char* ps_scheduler_plugin_get_description(int index) {
  return index == 0 ? "Destroy-count scheduler lifecycle test" : nullptr;
}

ps::IScheduler* ps_scheduler_plugin_create(const char* type_name,
                                           unsigned int) {
  if (!type_name || std::string(type_name) != "destroy_count_test") {
    return nullptr;
  }
  return new DestroyCountScheduler();
}

void ps_scheduler_plugin_destroy(ps::IScheduler* scheduler) {
  g_destroy_count.fetch_add(1);
  delete scheduler;
}

const char* ps_scheduler_plugin_get_version() {
  return "test";
}

int ps_test_scheduler_active_count() {
  return g_active_count.load();
}

int ps_test_scheduler_destroy_count() {
  return g_destroy_count.load();
}

void ps_test_scheduler_reset_counts() {
  g_active_count.store(0);
  g_destroy_count.store(0);
}
}
