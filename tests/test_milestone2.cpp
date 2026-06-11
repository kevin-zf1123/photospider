// Photospider M3.2 IScheduler Interface Tests
// Tests for scheduler lifecycle, GraphRuntime scheduler ownership, and
// planned-task runtime dispatch.

#include <gtest/gtest.h>

#include <atomic>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "kernel/graph_runtime.hpp"
#include "kernel/scheduler/i_scheduler.hpp"
#include "kernel/scheduler/scheduler_task_runtime.hpp"

namespace ps {
namespace {

class MockScheduler : public IScheduler, public SchedulerTaskRuntime {
 public:
  void attach(GraphRuntime* runtime) override {
    runtime_ = runtime;
    attach_called_ = true;
  }

  void detach() override {
    runtime_ = nullptr;
    detach_called_ = true;
  }

  void start() override {
    running_ = true;
    start_called_ = true;
  }

  void shutdown() override {
    running_ = false;
    shutdown_called_ = true;
  }

  std::string name() const override { return "MockScheduler"; }
  std::string get_stats() const override { return "Mock stats"; }
  bool is_running() const override { return running_; }
  bool task_runtime_running() const override { return running_; }

  void submit_initial_tasks(
      std::vector<Task>&& tasks, int total_task_count,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
    last_priority_ = priority;
    tasks_to_complete_.store(total_task_count, std::memory_order_relaxed);
    for (auto& task : tasks) {
      if (task) {
        task();
      }
    }
  }

  void submit_ready_task_from_worker(
      Task&& task,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
    submit_ready_task_any_thread(std::move(task), priority, std::nullopt);
  }

  void submit_ready_task_any_thread(
      Task&& task,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) override {
    last_priority_ = priority;
    last_epoch_ = epoch.value_or(0);
    if (task) {
      task();
    }
  }

  void wait_for_completion() override {
    if (first_exception_) {
      std::rethrow_exception(first_exception_);
    }
  }

  void set_exception(std::exception_ptr e) override {
    if (!first_exception_) {
      first_exception_ = e;
    }
  }

  void inc_tasks_to_complete(int delta) override {
    tasks_to_complete_.fetch_add(delta, std::memory_order_relaxed);
  }

  void dec_tasks_to_complete() override {
    tasks_to_complete_.fetch_sub(1, std::memory_order_relaxed);
  }

  void log_event(SchedulerTraceAction action, int node_id) override {
    last_action_ = action;
    last_node_id_ = node_id;
  }

  bool was_attach_called() const { return attach_called_; }
  bool was_detach_called() const { return detach_called_; }
  bool was_start_called() const { return start_called_; }
  bool was_shutdown_called() const { return shutdown_called_; }
  GraphRuntime* attached_runtime() const { return runtime_; }
  int tasks_to_complete() const { return tasks_to_complete_.load(); }
  SchedulerTaskPriority last_priority() const { return last_priority_; }
  uint64_t last_epoch() const { return last_epoch_; }
  SchedulerTraceAction last_action() const { return last_action_; }
  int last_node_id() const { return last_node_id_; }

 private:
  GraphRuntime* runtime_ = nullptr;
  bool running_ = false;
  bool attach_called_ = false;
  bool detach_called_ = false;
  bool start_called_ = false;
  bool shutdown_called_ = false;
  std::atomic<int> tasks_to_complete_{0};
  SchedulerTaskPriority last_priority_{SchedulerTaskPriority::Normal};
  uint64_t last_epoch_{0};
  SchedulerTraceAction last_action_{SchedulerTraceAction::Execute};
  int last_node_id_{-1};
  std::exception_ptr first_exception_;
};

TEST(M32InterfaceAbstraction, MockSchedulerLifecycle) {
  auto scheduler = std::make_unique<MockScheduler>();

  EXPECT_FALSE(scheduler->is_running());
  EXPECT_FALSE(scheduler->task_runtime_running());
  EXPECT_EQ(scheduler->name(), "MockScheduler");
  EXPECT_FALSE(scheduler->was_attach_called());
  EXPECT_FALSE(scheduler->was_start_called());

  scheduler->attach(nullptr);
  EXPECT_TRUE(scheduler->was_attach_called());

  scheduler->start();
  EXPECT_TRUE(scheduler->was_start_called());
  EXPECT_TRUE(scheduler->is_running());
  EXPECT_TRUE(scheduler->task_runtime_running());

  scheduler->shutdown();
  EXPECT_TRUE(scheduler->was_shutdown_called());
  EXPECT_FALSE(scheduler->is_running());

  scheduler->detach();
  EXPECT_TRUE(scheduler->was_detach_called());
}

TEST(M32InterfaceAbstraction, MockSchedulerTaskRuntimeDispatch) {
  MockScheduler scheduler;
  scheduler.start();
  std::atomic<int> counter{0};

  std::vector<SchedulerTaskRuntime::Task> tasks;
  tasks.emplace_back([&]() {
    counter.fetch_add(1);
    scheduler.submit_ready_task_from_worker([&]() {
      counter.fetch_add(10);
      scheduler.dec_tasks_to_complete();
    });
    scheduler.dec_tasks_to_complete();
  });

  scheduler.submit_initial_tasks(std::move(tasks), 2,
                                 SchedulerTaskPriority::High);
  scheduler.wait_for_completion();

  EXPECT_EQ(counter.load(), 11);
  EXPECT_EQ(scheduler.tasks_to_complete(), 0);
  EXPECT_EQ(scheduler.last_priority(), SchedulerTaskPriority::Normal);

  scheduler.submit_ready_task_any_thread([] {}, SchedulerTaskPriority::High,
                                         42);
  EXPECT_EQ(scheduler.last_priority(), SchedulerTaskPriority::High);
  EXPECT_EQ(scheduler.last_epoch(), 42u);

  scheduler.log_event(SchedulerTraceAction::ExecuteTile, 7);
  EXPECT_EQ(scheduler.last_action(), SchedulerTraceAction::ExecuteTile);
  EXPECT_EQ(scheduler.last_node_id(), 7);
}

TEST(M32InterfaceAbstraction, MockSchedulerTaskRuntimePropagatesException) {
  MockScheduler scheduler;
  scheduler.set_exception(
      std::make_exception_ptr(std::runtime_error("planned task failed")));
  EXPECT_THROW(scheduler.wait_for_completion(), std::runtime_error);
}

class GraphRuntimeSchedulerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::filesystem::create_directories(
        "sessions/scheduler_test_session/cache");
  }

  void TearDown() override {
    std::filesystem::remove_all("sessions/scheduler_test_session");
  }
};

TEST_F(GraphRuntimeSchedulerTest, SetAndGetScheduler) {
  GraphRuntime::Info info{"scheduler_test", "sessions/scheduler_test_session",
                          "", ""};

  GraphRuntime runtime(info);

  EXPECT_FALSE(runtime.has_scheduler(ComputeIntent::GlobalHighPrecision));
  EXPECT_FALSE(runtime.has_scheduler(ComputeIntent::RealTimeUpdate));
  EXPECT_EQ(runtime.get_scheduler(ComputeIntent::GlobalHighPrecision), nullptr);

  auto hp_scheduler = std::make_unique<MockScheduler>();
  MockScheduler* hp_ptr = hp_scheduler.get();
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::move(hp_scheduler));

  EXPECT_TRUE(runtime.has_scheduler(ComputeIntent::GlobalHighPrecision));
  EXPECT_EQ(runtime.get_scheduler(ComputeIntent::GlobalHighPrecision), hp_ptr);
  EXPECT_TRUE(hp_ptr->was_attach_called());
  EXPECT_EQ(hp_ptr->attached_runtime(), &runtime);

  auto rt_scheduler = std::make_unique<MockScheduler>();
  MockScheduler* rt_ptr = rt_scheduler.get();
  runtime.set_scheduler(ComputeIntent::RealTimeUpdate, std::move(rt_scheduler));

  EXPECT_TRUE(runtime.has_scheduler(ComputeIntent::RealTimeUpdate));
  EXPECT_EQ(runtime.get_scheduler(ComputeIntent::RealTimeUpdate), rt_ptr);
  EXPECT_TRUE(runtime.has_scheduler(ComputeIntent::GlobalHighPrecision));
}

struct SchedulerLifecycleTracker {
  static std::atomic<int> shutdown_count;
  static std::atomic<int> detach_count;

  static void reset() {
    shutdown_count.store(0);
    detach_count.store(0);
  }
};

std::atomic<int> SchedulerLifecycleTracker::shutdown_count{0};
std::atomic<int> SchedulerLifecycleTracker::detach_count{0};

class TrackedMockScheduler : public MockScheduler {
 public:
  void shutdown() override {
    MockScheduler::shutdown();
    SchedulerLifecycleTracker::shutdown_count.fetch_add(1);
  }

  void detach() override {
    MockScheduler::detach();
    SchedulerLifecycleTracker::detach_count.fetch_add(1);
  }
};

TEST_F(GraphRuntimeSchedulerTest, ReplaceScheduler) {
  GraphRuntime::Info info{"scheduler_test", "sessions/scheduler_test_session",
                          "", ""};

  GraphRuntime runtime(info);
  runtime.start();

  SchedulerLifecycleTracker::reset();

  auto scheduler1 = std::make_unique<TrackedMockScheduler>();
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::move(scheduler1));

  EXPECT_EQ(SchedulerLifecycleTracker::shutdown_count.load(), 0);
  EXPECT_EQ(SchedulerLifecycleTracker::detach_count.load(), 0);

  auto scheduler2 = std::make_unique<TrackedMockScheduler>();
  MockScheduler* ptr2 = scheduler2.get();
  runtime.replace_scheduler(ComputeIntent::GlobalHighPrecision,
                            std::move(scheduler2));

  EXPECT_EQ(SchedulerLifecycleTracker::shutdown_count.load(), 1);
  EXPECT_EQ(SchedulerLifecycleTracker::detach_count.load(), 1);
  EXPECT_TRUE(ptr2->was_attach_called());
  EXPECT_TRUE(ptr2->was_start_called());
  EXPECT_EQ(runtime.get_scheduler(ComputeIntent::GlobalHighPrecision), ptr2);

  runtime.stop();
}

}  // namespace
}  // namespace ps
