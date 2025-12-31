// Photospider M3.2 IScheduler Interface Tests
// Tests for the IScheduler interface and GraphRuntime scheduler management

#include <gtest/gtest.h>
#include <future>
#include <thread>

#include "kernel/scheduler/i_scheduler.hpp"
#include "kernel/graph_runtime.hpp"

namespace ps {
namespace {

// =============================================================================
// Mock Scheduler for testing
// =============================================================================
class MockScheduler : public IScheduler {
 public:
  MockScheduler() = default;
  ~MockScheduler() override = default;

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

  std::future<NodeOutput> schedule(const ComputeOptions& opts) override {
    schedule_called_ = true;
    last_opts_ = opts;
    
    // Return a simple promise for testing
    std::promise<NodeOutput> promise;
    NodeOutput output;
    output.debug.compute_device = "MockScheduler";
    promise.set_value(output);
    return promise.get_future();
  }

  std::string name() const override { return "MockScheduler"; }
  
  std::string get_stats() const override {
    return "Mock stats";
  }

  bool is_running() const override { return running_; }

  // Test accessors
  bool was_attach_called() const { return attach_called_; }
  bool was_detach_called() const { return detach_called_; }
  bool was_start_called() const { return start_called_; }
  bool was_shutdown_called() const { return shutdown_called_; }
  bool was_schedule_called() const { return schedule_called_; }
  const ComputeOptions& last_options() const { return last_opts_; }
  GraphRuntime* attached_runtime() const { return runtime_; }

 private:
  GraphRuntime* runtime_ = nullptr;
  bool running_ = false;
  bool attach_called_ = false;
  bool detach_called_ = false;
  bool start_called_ = false;
  bool shutdown_called_ = false;
  bool schedule_called_ = false;
  ComputeOptions last_opts_;
};

// =============================================================================
// ComputeOptions Tests
// =============================================================================
TEST(M32InterfaceAbstraction, ComputeOptionsDefaultValues) {
  ComputeOptions opts;
  EXPECT_EQ(opts.intent, ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(opts.node_id, -1);
  EXPECT_FALSE(opts.dirty_roi.has_value());
  EXPECT_EQ(opts.cache_precision, "int8");
  EXPECT_FALSE(opts.force_recache);
  EXPECT_FALSE(opts.enable_timing);
  EXPECT_FALSE(opts.disable_disk_cache);
  EXPECT_EQ(opts.epoch, 0u);
}

TEST(M32InterfaceAbstraction, ComputeOptionsWithIntent) {
  ComputeOptions opts;
  opts.intent = ComputeIntent::RealTimeUpdate;
  opts.node_id = 42;
  opts.dirty_roi = cv::Rect(10, 20, 100, 200);
  opts.cache_precision = "float32";
  opts.force_recache = true;
  opts.epoch = 123;
  
  EXPECT_EQ(opts.intent, ComputeIntent::RealTimeUpdate);
  EXPECT_EQ(opts.node_id, 42);
  EXPECT_TRUE(opts.dirty_roi.has_value());
  EXPECT_EQ(opts.dirty_roi->x, 10);
  EXPECT_EQ(opts.dirty_roi->y, 20);
  EXPECT_EQ(opts.dirty_roi->width, 100);
  EXPECT_EQ(opts.dirty_roi->height, 200);
  EXPECT_EQ(opts.cache_precision, "float32");
  EXPECT_TRUE(opts.force_recache);
  EXPECT_EQ(opts.epoch, 123u);
}

// =============================================================================
// IScheduler Interface Tests
// =============================================================================
TEST(M32InterfaceAbstraction, MockSchedulerLifecycle) {
  auto scheduler = std::make_unique<MockScheduler>();
  
  EXPECT_FALSE(scheduler->is_running());
  EXPECT_EQ(scheduler->name(), "MockScheduler");
  EXPECT_FALSE(scheduler->was_attach_called());
  EXPECT_FALSE(scheduler->was_start_called());
  
  scheduler->attach(nullptr);
  EXPECT_TRUE(scheduler->was_attach_called());
  
  scheduler->start();
  EXPECT_TRUE(scheduler->was_start_called());
  EXPECT_TRUE(scheduler->is_running());
  
  scheduler->shutdown();
  EXPECT_TRUE(scheduler->was_shutdown_called());
  EXPECT_FALSE(scheduler->is_running());
  
  scheduler->detach();
  EXPECT_TRUE(scheduler->was_detach_called());
}

TEST(M32InterfaceAbstraction, MockSchedulerSchedule) {
  auto scheduler = std::make_unique<MockScheduler>();
  
  ComputeOptions opts;
  opts.intent = ComputeIntent::RealTimeUpdate;
  opts.node_id = 99;
  opts.dirty_roi = cv::Rect(0, 0, 50, 50);
  
  auto future = scheduler->schedule(opts);
  auto result = future.get();
  
  EXPECT_TRUE(scheduler->was_schedule_called());
  EXPECT_EQ(scheduler->last_options().intent, ComputeIntent::RealTimeUpdate);
  EXPECT_EQ(scheduler->last_options().node_id, 99);
  EXPECT_EQ(result.debug.compute_device, "MockScheduler");
}

// =============================================================================
// GraphRuntime Scheduler Management Tests
// =============================================================================
class GraphRuntimeSchedulerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create a temporary test session directory
    std::filesystem::create_directories("sessions/scheduler_test_session/cache");
  }
  
  void TearDown() override {
    // Cleanup
    std::filesystem::remove_all("sessions/scheduler_test_session");
  }
};

TEST_F(GraphRuntimeSchedulerTest, SetAndGetScheduler) {
  GraphRuntime::Info info{
    "scheduler_test",
    "sessions/scheduler_test_session",
    "",
    ""
  };
  
  GraphRuntime runtime(info);
  
  // Initially no scheduler
  EXPECT_FALSE(runtime.has_scheduler(ComputeIntent::GlobalHighPrecision));
  EXPECT_FALSE(runtime.has_scheduler(ComputeIntent::RealTimeUpdate));
  EXPECT_EQ(runtime.get_scheduler(ComputeIntent::GlobalHighPrecision), nullptr);
  
  // Set HP scheduler
  auto hp_scheduler = std::make_unique<MockScheduler>();
  MockScheduler* hp_ptr = hp_scheduler.get();
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision, std::move(hp_scheduler));
  
  EXPECT_TRUE(runtime.has_scheduler(ComputeIntent::GlobalHighPrecision));
  EXPECT_EQ(runtime.get_scheduler(ComputeIntent::GlobalHighPrecision), hp_ptr);
  EXPECT_TRUE(hp_ptr->was_attach_called());
  EXPECT_EQ(hp_ptr->attached_runtime(), &runtime);
  
  // Set RT scheduler
  auto rt_scheduler = std::make_unique<MockScheduler>();
  MockScheduler* rt_ptr = rt_scheduler.get();
  runtime.set_scheduler(ComputeIntent::RealTimeUpdate, std::move(rt_scheduler));
  
  EXPECT_TRUE(runtime.has_scheduler(ComputeIntent::RealTimeUpdate));
  EXPECT_EQ(runtime.get_scheduler(ComputeIntent::RealTimeUpdate), rt_ptr);
  
  // Both schedulers should be present
  EXPECT_TRUE(runtime.has_scheduler(ComputeIntent::GlobalHighPrecision));
  EXPECT_TRUE(runtime.has_scheduler(ComputeIntent::RealTimeUpdate));
}

// Helper to track scheduler lifecycle across ownership transfers
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
  GraphRuntime::Info info{
    "scheduler_test",
    "sessions/scheduler_test_session",
    "",
    ""
  };
  
  GraphRuntime runtime(info);
  runtime.start();
  
  // Reset tracker
  SchedulerLifecycleTracker::reset();
  
  // Set initial scheduler (using tracked version)
  auto scheduler1 = std::make_unique<TrackedMockScheduler>();
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision, std::move(scheduler1));
  
  EXPECT_EQ(SchedulerLifecycleTracker::shutdown_count.load(), 0);
  EXPECT_EQ(SchedulerLifecycleTracker::detach_count.load(), 0);
  
  // Replace with new scheduler
  auto scheduler2 = std::make_unique<TrackedMockScheduler>();
  MockScheduler* ptr2 = scheduler2.get();
  runtime.replace_scheduler(ComputeIntent::GlobalHighPrecision, std::move(scheduler2));
  
  // Old scheduler should have been shutdown and detached
  EXPECT_EQ(SchedulerLifecycleTracker::shutdown_count.load(), 1);
  EXPECT_EQ(SchedulerLifecycleTracker::detach_count.load(), 1);
  
  // New scheduler should be attached and started
  EXPECT_TRUE(ptr2->was_attach_called());
  EXPECT_TRUE(ptr2->was_start_called());
  EXPECT_EQ(runtime.get_scheduler(ComputeIntent::GlobalHighPrecision), ptr2);
  
  runtime.stop();
}

TEST_F(GraphRuntimeSchedulerTest, SubmitComputeWithoutScheduler) {
  GraphRuntime::Info info{
    "scheduler_test",
    "sessions/scheduler_test_session",
    "",
    ""
  };
  
  GraphRuntime runtime(info);
  
  ComputeOptions opts;
  opts.intent = ComputeIntent::GlobalHighPrecision;
  opts.node_id = 1;
  
  // Should throw because no scheduler is registered
  auto future = runtime.submit_compute(opts);
  EXPECT_THROW(future.get(), std::runtime_error);
}

TEST_F(GraphRuntimeSchedulerTest, SubmitComputeWithScheduler) {
  GraphRuntime::Info info{
    "scheduler_test",
    "sessions/scheduler_test_session",
    "",
    ""
  };
  
  GraphRuntime runtime(info);
  
  auto scheduler = std::make_unique<MockScheduler>();
  MockScheduler* ptr = scheduler.get();
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision, std::move(scheduler));
  
  ComputeOptions opts;
  opts.intent = ComputeIntent::GlobalHighPrecision;
  opts.node_id = 42;
  opts.cache_precision = "float32";
  
  auto future = runtime.submit_compute(opts);
  auto result = future.get();
  
  EXPECT_TRUE(ptr->was_schedule_called());
  EXPECT_EQ(ptr->last_options().node_id, 42);
  EXPECT_EQ(ptr->last_options().cache_precision, "float32");
  EXPECT_EQ(result.debug.compute_device, "MockScheduler");
}

TEST_F(GraphRuntimeSchedulerTest, MultipleSchedulersRouting) {
  GraphRuntime::Info info{
    "scheduler_test",
    "sessions/scheduler_test_session",
    "",
    ""
  };
  
  GraphRuntime runtime(info);
  
  // Set HP scheduler
  auto hp_scheduler = std::make_unique<MockScheduler>();
  MockScheduler* hp_ptr = hp_scheduler.get();
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision, std::move(hp_scheduler));
  
  // Set RT scheduler
  auto rt_scheduler = std::make_unique<MockScheduler>();
  MockScheduler* rt_ptr = rt_scheduler.get();
  runtime.set_scheduler(ComputeIntent::RealTimeUpdate, std::move(rt_scheduler));
  
  // Submit HP task
  ComputeOptions hp_opts;
  hp_opts.intent = ComputeIntent::GlobalHighPrecision;
  hp_opts.node_id = 1;
  
  auto hp_future = runtime.submit_compute(hp_opts);
  hp_future.get();
  
  EXPECT_TRUE(hp_ptr->was_schedule_called());
  EXPECT_FALSE(rt_ptr->was_schedule_called());
  
  // Submit RT task
  ComputeOptions rt_opts;
  rt_opts.intent = ComputeIntent::RealTimeUpdate;
  rt_opts.node_id = 2;
  
  auto rt_future = runtime.submit_compute(rt_opts);
  rt_future.get();
  
  EXPECT_TRUE(rt_ptr->was_schedule_called());
  EXPECT_EQ(rt_ptr->last_options().node_id, 2);
}

}  // namespace
}  // namespace ps

