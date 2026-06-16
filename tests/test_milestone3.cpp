// Photospider M3.3 CpuWorkStealingScheduler Tests
// Tests for migrating run_loop and queue logic to CpuWorkStealingScheduler

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "kernel/graph_runtime.hpp"
#include "kernel/scheduler/cpu_work_stealing_scheduler.hpp"

namespace ps {
namespace {

// =============================================================================
// Basic Lifecycle Tests
// =============================================================================

TEST(M33CpuWorkStealingScheduler, Construction) {
  // Default construction uses hardware_concurrency
  CpuWorkStealingScheduler scheduler1;
  EXPECT_EQ(scheduler1.name(), "CpuWorkStealingScheduler");
  EXPECT_FALSE(scheduler1.is_running());

  // Construction with explicit worker count
  CpuWorkStealingScheduler scheduler2(4);
  EXPECT_FALSE(scheduler2.is_running());
}

TEST(M33CpuWorkStealingScheduler, StartAndShutdown) {
  CpuWorkStealingScheduler scheduler(2);

  EXPECT_FALSE(scheduler.is_running());

  scheduler.start();
  EXPECT_TRUE(scheduler.is_running());

  // Double start should be no-op
  scheduler.start();
  EXPECT_TRUE(scheduler.is_running());

  scheduler.shutdown();
  EXPECT_FALSE(scheduler.is_running());

  // Double shutdown should be no-op
  scheduler.shutdown();
  EXPECT_FALSE(scheduler.is_running());
}

TEST(M33CpuWorkStealingScheduler, AttachAndDetach) {
  CpuWorkStealingScheduler scheduler;

  // Initially not attached
  scheduler.attach(nullptr);
  scheduler.detach();
  // Should not crash
  EXPECT_TRUE(true);
}

// =============================================================================
// Task Submission and Execution Tests
// =============================================================================

TEST(M33CpuWorkStealingScheduler, SubmitAndExecuteSingleTask) {
  CpuWorkStealingScheduler scheduler(2);
  scheduler.start();

  std::atomic<int> counter{0};

  std::vector<CpuWorkStealingScheduler::Task> tasks;
  tasks.push_back([&counter, &scheduler]() {
    counter.fetch_add(1);
    scheduler.dec_tasks_to_complete();
  });

  scheduler.submit_initial_tasks(std::move(tasks), 1);
  scheduler.wait_for_completion();

  EXPECT_EQ(counter.load(), 1);

  scheduler.shutdown();
}

TEST(M33CpuWorkStealingScheduler, SubmitAndExecuteMultipleTasks) {
  CpuWorkStealingScheduler scheduler(4);
  scheduler.start();

  constexpr int kNumTasks = 100;
  std::atomic<int> counter{0};

  std::vector<CpuWorkStealingScheduler::Task> tasks;
  tasks.reserve(kNumTasks);
  for (int i = 0; i < kNumTasks; ++i) {
    tasks.push_back([&counter, &scheduler]() {
      counter.fetch_add(1);
      scheduler.dec_tasks_to_complete();
    });
  }

  scheduler.submit_initial_tasks(std::move(tasks), kNumTasks);
  scheduler.wait_for_completion();

  EXPECT_EQ(counter.load(), kNumTasks);

  scheduler.shutdown();
}

TEST(M33CpuWorkStealingScheduler,
     RepeatedShutdownAfterCompletedBatchDoesNotHang) {
  constexpr int kIterations = 64;
  constexpr int kNumTasks = 100;

  for (int iteration = 0; iteration < kIterations; ++iteration) {
    CpuWorkStealingScheduler scheduler(4);
    scheduler.start();

    std::atomic<int> counter{0};
    std::vector<CpuWorkStealingScheduler::Task> tasks;
    tasks.reserve(kNumTasks);
    for (int i = 0; i < kNumTasks; ++i) {
      tasks.push_back([&counter, &scheduler]() {
        counter.fetch_add(1);
        scheduler.dec_tasks_to_complete();
      });
    }

    scheduler.submit_initial_tasks(std::move(tasks), kNumTasks);
    scheduler.wait_for_completion();

    EXPECT_EQ(counter.load(), kNumTasks);

    scheduler.shutdown();
    EXPECT_FALSE(scheduler.is_running());
  }
}

TEST(M33CpuWorkStealingScheduler, HighPriorityTasks) {
  CpuWorkStealingScheduler scheduler(2);
  scheduler.start();

  std::atomic<int> counter{0};

  std::vector<CpuWorkStealingScheduler::Task> tasks;
  for (int i = 0; i < 10; ++i) {
    tasks.push_back([&counter, &scheduler]() {
      counter.fetch_add(1);
      scheduler.dec_tasks_to_complete();
    });
  }

  scheduler.submit_initial_tasks(std::move(tasks), 10,
                                 CpuWorkStealingScheduler::TaskPriority::High);
  scheduler.wait_for_completion();

  EXPECT_EQ(counter.load(), 10);

  scheduler.shutdown();
}

TEST(M33CpuWorkStealingScheduler, SubmitTaskFromWorker) {
  CpuWorkStealingScheduler scheduler(2);
  scheduler.start();

  std::atomic<int> counter{0};
  std::atomic<bool> nested_executed{false};

  std::vector<CpuWorkStealingScheduler::Task> tasks;
  tasks.push_back([&scheduler, &counter, &nested_executed]() {
    counter.fetch_add(1);
    // Submit a nested task from within worker
    scheduler.submit_ready_task_from_worker(
        [&counter, &nested_executed, &scheduler]() {
          counter.fetch_add(1);
          nested_executed.store(true);
          scheduler.dec_tasks_to_complete();
        });
    scheduler.inc_tasks_to_complete(1);
    scheduler.dec_tasks_to_complete();
  });

  scheduler.submit_initial_tasks(std::move(tasks), 1);
  scheduler.wait_for_completion();

  EXPECT_GE(counter.load(), 1);

  scheduler.shutdown();
}

TEST(M33CpuWorkStealingScheduler, SubmitTaskAnyThread) {
  CpuWorkStealingScheduler scheduler(2);
  scheduler.start();

  std::atomic<int> counter{0};

  // Submit from main thread (not a worker)
  std::vector<CpuWorkStealingScheduler::Task> initial;
  initial.push_back([&counter, &scheduler]() {
    counter.fetch_add(1);
    scheduler.dec_tasks_to_complete();
  });

  scheduler.submit_initial_tasks(std::move(initial), 2);

  // Submit additional task from any thread
  scheduler.submit_ready_task_any_thread([&counter, &scheduler]() {
    counter.fetch_add(1);
    scheduler.dec_tasks_to_complete();
  });

  scheduler.wait_for_completion();

  EXPECT_EQ(counter.load(), 2);

  scheduler.shutdown();
}

// =============================================================================
// Work Stealing Tests
// =============================================================================

TEST(M33CpuWorkStealingScheduler, WorkStealing) {
  // Use more workers than tasks initially assigned to some queues
  CpuWorkStealingScheduler scheduler(4);
  scheduler.start();

  constexpr int kNumTasks = 50;
  std::atomic<int> counter{0};
  std::set<int> worker_ids_used;
  std::mutex worker_mutex;

  std::vector<CpuWorkStealingScheduler::Task> tasks;
  tasks.reserve(kNumTasks);
  for (int i = 0; i < kNumTasks; ++i) {
    tasks.push_back([&counter, &worker_ids_used, &worker_mutex, &scheduler]() {
      counter.fetch_add(1);
      int worker_id = CpuWorkStealingScheduler::this_worker_id();
      if (worker_id >= 0) {
        std::lock_guard<std::mutex> lock(worker_mutex);
        worker_ids_used.insert(worker_id);
      }
      // Add some work to increase chance of stealing
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      scheduler.dec_tasks_to_complete();
    });
  }

  scheduler.submit_initial_tasks(std::move(tasks), kNumTasks);
  scheduler.wait_for_completion();

  EXPECT_EQ(counter.load(), kNumTasks);

  // With 4 workers and 50 tasks, multiple workers should have been used
  if (std::thread::hardware_concurrency() > 1) {
    EXPECT_GT(worker_ids_used.size(), 1u);
  }

  scheduler.shutdown();
}

// =============================================================================
// Epoch Management Tests
// =============================================================================

TEST(M33CpuWorkStealingScheduler, EpochManagement) {
  CpuWorkStealingScheduler scheduler(2);
  scheduler.start();

  // 初始 epoch 是 0
  uint64_t epoch0 = scheduler.active_epoch();
  EXPECT_EQ(epoch0, 0u);

  // begin_new_epoch 应该递增 epoch
  uint64_t epoch1 = scheduler.begin_new_epoch();
  uint64_t epoch2 = scheduler.begin_new_epoch();
  uint64_t epoch3 = scheduler.begin_new_epoch();

  EXPECT_LT(epoch1, epoch2);
  EXPECT_LT(epoch2, epoch3);
  EXPECT_EQ(scheduler.active_epoch(), epoch3);

  // 旧的 epochs 应该被取消
  EXPECT_TRUE(scheduler.should_cancel_epoch(epoch1));
  EXPECT_TRUE(scheduler.should_cancel_epoch(epoch2));
  EXPECT_FALSE(scheduler.should_cancel_epoch(epoch3));

  // Epoch 0 永远不会被取消（特殊标记）
  EXPECT_FALSE(scheduler.should_cancel_epoch(0));

  scheduler.shutdown();
}

TEST(M33CpuWorkStealingScheduler, EpochCancellation) {
  CpuWorkStealingScheduler scheduler(2);
  scheduler.start();

  std::atomic<int> executed_count{0};

  // 首先创建一个新 epoch，以便后续的任务可以被取消
  scheduler.begin_new_epoch();

  // 提交任务
  std::vector<CpuWorkStealingScheduler::Task> tasks;
  for (int i = 0; i < 20; ++i) {
    tasks.push_back([&executed_count, &scheduler]() {
      // 模拟一些工作
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      executed_count.fetch_add(1);
      scheduler.dec_tasks_to_complete();
    });
  }

  scheduler.submit_initial_tasks(std::move(tasks), 20);

  // 快速开始新 epoch 来取消待处理任务
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  scheduler.begin_new_epoch();

  // 由于任务被取消，等待可能永远不会完成
  // 因此我们使用 shutdown 来强制停止
  scheduler.shutdown();

  // 不是所有任务都会执行，因为 epoch 取消了
  // 只验证没有崩溃，并且计数器是合理的
  EXPECT_LE(executed_count.load(), 20);
}

// =============================================================================
// Exception Handling Tests
// =============================================================================

TEST(M33CpuWorkStealingScheduler, ExceptionHandling) {
  CpuWorkStealingScheduler scheduler(2);
  scheduler.start();

  std::vector<CpuWorkStealingScheduler::Task> tasks;
  tasks.push_back([]() { throw std::runtime_error("Test exception"); });

  scheduler.submit_initial_tasks(std::move(tasks), 1);

  EXPECT_THROW(scheduler.wait_for_completion(), std::runtime_error);

  scheduler.shutdown();
}

TEST(M33CpuWorkStealingScheduler, RecoveryAfterException) {
  CpuWorkStealingScheduler scheduler(2);
  scheduler.start();

  // First batch throws
  {
    std::vector<CpuWorkStealingScheduler::Task> tasks;
    tasks.push_back([]() { throw std::runtime_error("Test exception"); });
    scheduler.submit_initial_tasks(std::move(tasks), 1);
    EXPECT_THROW(scheduler.wait_for_completion(), std::runtime_error);
  }

  // Second batch should work fine
  {
    std::atomic<int> counter{0};
    std::vector<CpuWorkStealingScheduler::Task> tasks;
    tasks.push_back([&counter, &scheduler]() {
      counter.fetch_add(1);
      scheduler.dec_tasks_to_complete();
    });
    scheduler.submit_initial_tasks(std::move(tasks), 1);
    scheduler.wait_for_completion();
    EXPECT_EQ(counter.load(), 1);
  }

  scheduler.shutdown();
}

// =============================================================================
// Stats and Monitoring Tests
// =============================================================================

TEST(M33CpuWorkStealingScheduler, Stats) {
  CpuWorkStealingScheduler scheduler(2);
  scheduler.start();

  std::string stats_before = scheduler.get_stats();
  EXPECT_FALSE(stats_before.empty());
  EXPECT_NE(stats_before.find("Workers:"), std::string::npos);

  std::atomic<int> counter{0};
  std::vector<CpuWorkStealingScheduler::Task> tasks;
  for (int i = 0; i < 10; ++i) {
    tasks.push_back([&counter, &scheduler]() {
      counter.fetch_add(1);
      scheduler.dec_tasks_to_complete();
    });
  }
  scheduler.submit_initial_tasks(std::move(tasks), 10);
  scheduler.wait_for_completion();

  std::string stats_after = scheduler.get_stats();
  EXPECT_FALSE(stats_after.empty());

  scheduler.shutdown();
}

// =============================================================================
// Integration with GraphRuntime Tests
// =============================================================================

class M33SchedulerIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::filesystem::create_directories("sessions/m33_test_session/cache");
  }

  void TearDown() override {
    std::filesystem::remove_all("sessions/m33_test_session");
  }
};

TEST_F(M33SchedulerIntegrationTest, SetSchedulerOnRuntime) {
  GraphRuntime::Info info{"m33_test", "sessions/m33_test_session", "", ""};

  GraphRuntime runtime(info);

  // Create and set scheduler
  auto scheduler = std::make_unique<CpuWorkStealingScheduler>(2);
  CpuWorkStealingScheduler* ptr = scheduler.get();

  EXPECT_FALSE(runtime.has_scheduler(ComputeIntent::GlobalHighPrecision));

  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::move(scheduler));

  EXPECT_TRUE(runtime.has_scheduler(ComputeIntent::GlobalHighPrecision));
  EXPECT_EQ(runtime.get_scheduler(ComputeIntent::GlobalHighPrecision), ptr);
}

TEST_F(M33SchedulerIntegrationTest, ReplaceSchedulerOnRuntime) {
  GraphRuntime::Info info{"m33_test", "sessions/m33_test_session", "", ""};

  GraphRuntime runtime(info);
  runtime.start();

  // Set initial scheduler
  auto scheduler1 = std::make_unique<CpuWorkStealingScheduler>(2);
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::move(scheduler1));

  // Replace with new scheduler
  auto scheduler2 = std::make_unique<CpuWorkStealingScheduler>(4);
  CpuWorkStealingScheduler* ptr2 = scheduler2.get();

  runtime.replace_scheduler(ComputeIntent::GlobalHighPrecision,
                            std::move(scheduler2));

  EXPECT_EQ(runtime.get_scheduler(ComputeIntent::GlobalHighPrecision), ptr2);

  runtime.stop();
}

TEST_F(M33SchedulerIntegrationTest, RegisteredSchedulerExposesTaskRuntime) {
  GraphRuntime::Info info{"m33_test", "sessions/m33_test_session", "", ""};

  GraphRuntime runtime(info);
  runtime.start();

  auto scheduler = std::make_unique<CpuWorkStealingScheduler>(2);
  scheduler->start();
  CpuWorkStealingScheduler* scheduler_ptr = scheduler.get();
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::move(scheduler));

  auto* task_runtime = dynamic_cast<SchedulerTaskRuntime*>(
      runtime.get_scheduler(ComputeIntent::GlobalHighPrecision));
  ASSERT_NE(task_runtime, nullptr);
  EXPECT_EQ(task_runtime, scheduler_ptr);
  EXPECT_TRUE(task_runtime->task_runtime_running());

  runtime.stop();
}

// =============================================================================
// Thread-Local Storage Tests
// =============================================================================

TEST(M33CpuWorkStealingScheduler, ThreadLocalWorkerIdAndEpoch) {
  CpuWorkStealingScheduler scheduler(2);
  scheduler.start();

  std::atomic<int> captured_worker_id{-2};
  std::atomic<uint64_t> captured_epoch{0};

  std::vector<CpuWorkStealingScheduler::Task> tasks;
  tasks.push_back([&captured_worker_id, &captured_epoch, &scheduler]() {
    captured_worker_id.store(CpuWorkStealingScheduler::this_worker_id());
    captured_epoch.store(CpuWorkStealingScheduler::this_task_epoch());
    scheduler.dec_tasks_to_complete();
  });

  scheduler.submit_initial_tasks(std::move(tasks), 1);
  scheduler.wait_for_completion();

  // Worker ID should be valid (0 or 1 for 2 workers)
  int worker_id = captured_worker_id.load();
  EXPECT_GE(worker_id, 0);
  EXPECT_LT(worker_id, 2);

  // Epoch should be set
  EXPECT_GT(captured_epoch.load(), 0u);

  scheduler.shutdown();
}

// =============================================================================
// Edge Case Tests
// =============================================================================

TEST(M33CpuWorkStealingScheduler, EmptyTaskList) {
  CpuWorkStealingScheduler scheduler(2);
  scheduler.start();

  std::vector<CpuWorkStealingScheduler::Task> tasks;
  scheduler.submit_initial_tasks(std::move(tasks), 0);
  scheduler.wait_for_completion();

  // Should complete immediately without hanging
  EXPECT_TRUE(true);

  scheduler.shutdown();
}

TEST(M33CpuWorkStealingScheduler, SingleWorker) {
  CpuWorkStealingScheduler scheduler(1);
  scheduler.start();

  std::atomic<int> counter{0};

  std::vector<CpuWorkStealingScheduler::Task> tasks;
  for (int i = 0; i < 10; ++i) {
    tasks.push_back([&counter, &scheduler]() {
      counter.fetch_add(1);
      scheduler.dec_tasks_to_complete();
    });
  }

  scheduler.submit_initial_tasks(std::move(tasks), 10);
  scheduler.wait_for_completion();

  EXPECT_EQ(counter.load(), 10);

  scheduler.shutdown();
}

TEST(M33CpuWorkStealingScheduler, RapidStartStopCycles) {
  CpuWorkStealingScheduler scheduler(2);

  for (int i = 0; i < 5; ++i) {
    scheduler.start();
    EXPECT_TRUE(scheduler.is_running());

    std::atomic<int> counter{0};
    std::vector<CpuWorkStealingScheduler::Task> tasks;
    tasks.push_back([&counter, &scheduler]() {
      counter.fetch_add(1);
      scheduler.dec_tasks_to_complete();
    });
    scheduler.submit_initial_tasks(std::move(tasks), 1);
    scheduler.wait_for_completion();
    EXPECT_EQ(counter.load(), 1);

    scheduler.shutdown();
    EXPECT_FALSE(scheduler.is_running());
  }
}

}  // namespace
}  // namespace ps
