// Photospider M3.2 IScheduler Interface Tests
// Tests for scheduler lifecycle, GraphRuntime scheduler ownership, and
// planned-task runtime dispatch.

#include <gtest/gtest.h>

#include <atomic>
#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/scheduler/scheduler.hpp"
#include "photospider/scheduler/scheduler_task_runtime.hpp"
#include "runtime/graph_runtime.hpp"

namespace ps {
namespace {

/**
 * @brief Minimal host-context probe for direct scheduler lifecycle tests.
 * @note The probe exposes no graph, cache, native-device, or runtime owner.
 */
class StubSchedulerHostContext final : public SchedulerHostContext {
 public:
  /** @copydoc SchedulerHostContext::is_device_available */
  bool is_device_available(Device device) const noexcept override {
    return device == Device::CPU;
  }

  /** @copydoc SchedulerHostContext::set_task_context */
  void set_task_context(int worker_id, std::uint64_t epoch) noexcept override {
    (void)worker_id;
    (void)epoch;
  }

  /** @copydoc SchedulerHostContext::clear_task_context */
  void clear_task_context() noexcept override {}

  /** @copydoc SchedulerHostContext::log_event */
  void log_event(SchedulerTraceAction action, int node_id, int worker_id,
                 std::uint64_t epoch) noexcept override {
    (void)action;
    (void)node_id;
    (void)worker_id;
    (void)epoch;
  }

  /** @brief Releases the concrete host probe. */
  ~StubSchedulerHostContext() override = default;
};

/**
 * @brief Owns callback storage while exposing scheduler-facing task handles.
 * @note Every handle borrows this executor until synchronous mock settlement.
 */
class CallbackTaskExecutor final : public TaskExecutor {
 public:
  /**
   * @brief Adds one callback and returns its dense task id.
   * @param task Callback whose ownership moves into the executor.
   * @return Zero-based task id for a later `TaskHandle`.
   * @throws std::bad_alloc if callback storage growth cannot allocate.
   */
  int add(SchedulerTaskRuntime::Task task) {
    tasks_.push_back(std::move(task));
    return static_cast<int>(tasks_.size() - 1U);
  }

  /** @copydoc TaskExecutor::run_task */
  void run_task(int task_id) override {
    if (task_id < 0 || static_cast<std::size_t>(task_id) >= tasks_.size()) {
      throw std::out_of_range("mock task id is outside executor storage");
    }
    tasks_[static_cast<std::size_t>(task_id)]();
  }

 private:
  /** @brief Owned callbacks indexed by the handles supplied to the mock. */
  std::vector<SchedulerTaskRuntime::Task> tasks_;
};

/**
 * @brief Synchronous scheduler mock implementing the exact public SDK shape.
 * @note The mock borrows only `SchedulerHostContext`; GraphRuntime remains
 *       reachable solely when it is itself the concrete host implementation.
 */
class MockScheduler : public IScheduler {
 public:
  /** @copydoc IScheduler::attach */
  void attach(SchedulerHostContext& host) override {
    host_ = &host;
    attach_called_ = true;
  }

  /** @copydoc IScheduler::detach */
  void detach() override {
    host_ = nullptr;
    detach_called_ = true;
  }

  /** @copydoc IScheduler::start */
  void start() override {
    host_at_start_ = host_;
    running_ = true;
    start_called_ = true;
  }

  /** @copydoc IScheduler::shutdown */
  void shutdown() override {
    running_ = false;
    shutdown_called_ = true;
  }

  /** @copydoc IScheduler::name */
  std::string name() const override { return "MockScheduler"; }

  /** @copydoc IScheduler::get_stats */
  std::string get_stats() const override { return "Mock stats"; }

  /** @copydoc IScheduler::is_running */
  bool is_running() const override { return running_; }

  /** @copydoc SchedulerTaskRuntime::submit_initial_task_handles */
  void submit_initial_task_handles(
      std::vector<TaskHandle>&& handles, int total_task_count,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
    last_priority_ = priority;
    tasks_to_complete_.store(total_task_count, std::memory_order_relaxed);
    for (const TaskHandle& handle : handles) {
      if (handle) {
        handle.run();
      }
    }
  }

  /** @copydoc SchedulerTaskRuntime::submit_ready_task_handles_from_worker */
  void submit_ready_task_handles_from_worker(
      std::vector<TaskHandle>&& handles,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
    last_priority_ = priority;
    for (const TaskHandle& handle : handles) {
      if (handle) {
        handle.run();
      }
    }
  }

  /** @copydoc SchedulerTaskRuntime::submit_ready_task_any_thread */
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

  /** @copydoc SchedulerTaskRuntime::wait_for_completion */
  void wait_for_completion() override {
    if (first_exception_) {
      std::rethrow_exception(first_exception_);
    }
  }

  /** @copydoc SchedulerTaskRuntime::set_exception */
  void set_exception(std::exception_ptr e) override {
    if (!first_exception_) {
      first_exception_ = e;
    }
  }

  /** @copydoc SchedulerTaskRuntime::inc_tasks_to_complete */
  void inc_tasks_to_complete(int delta) override {
    tasks_to_complete_.fetch_add(delta, std::memory_order_relaxed);
  }

  /** @copydoc SchedulerTaskRuntime::dec_tasks_to_complete */
  void dec_tasks_to_complete() override {
    tasks_to_complete_.fetch_sub(1, std::memory_order_relaxed);
  }

  /** @copydoc SchedulerTaskRuntime::log_event */
  void log_event(SchedulerTraceAction action, int node_id) override {
    last_action_ = action;
    last_node_id_ = node_id;
  }

  /** @brief Reports whether attach was called. */
  bool was_attach_called() const { return attach_called_; }
  /** @brief Reports whether detach was called. */
  bool was_detach_called() const { return detach_called_; }
  /** @brief Reports whether start was called. */
  bool was_start_called() const { return start_called_; }
  /** @brief Reports whether shutdown was called. */
  bool was_shutdown_called() const { return shutdown_called_; }
  /** @brief Returns the currently borrowed host, or null after detach. */
  SchedulerHostContext* attached_host() const { return host_; }
  /** @brief Returns the host observed by the latest start call. */
  SchedulerHostContext* host_at_start() const { return host_at_start_; }
  /** @brief Returns current logical completion accounting. */
  int tasks_to_complete() const { return tasks_to_complete_.load(); }
  /** @brief Returns the latest submitted priority. */
  SchedulerTaskPriority last_priority() const { return last_priority_; }
  /** @brief Returns the latest any-thread epoch. */
  uint64_t last_epoch() const { return last_epoch_; }
  /** @brief Returns the latest trace action. */
  SchedulerTraceAction last_action() const { return last_action_; }
  /** @brief Returns the latest trace node id. */
  int last_node_id() const { return last_node_id_; }

 private:
  /** @brief Borrowed public host context, or null while detached. */
  SchedulerHostContext* host_ = nullptr;
  /** @brief Host context observed by the latest start call. */
  SchedulerHostContext* host_at_start_ = nullptr;
  /** @brief Public lifecycle state. */
  bool running_ = false;
  /** @brief Whether attach has been called. */
  bool attach_called_ = false;
  /** @brief Whether detach has been called. */
  bool detach_called_ = false;
  /** @brief Whether start has been called. */
  bool start_called_ = false;
  /** @brief Whether shutdown has been called. */
  bool shutdown_called_ = false;
  /** @brief Logical completion accounting used by synchronous callbacks. */
  std::atomic<int> tasks_to_complete_{0};
  /** @brief Latest submitted priority. */
  SchedulerTaskPriority last_priority_{SchedulerTaskPriority::Normal};
  /** @brief Latest any-thread callback epoch. */
  uint64_t last_epoch_{0};
  /** @brief Latest trace action. */
  SchedulerTraceAction last_action_{SchedulerTraceAction::Execute};
  /** @brief Latest trace node id. */
  int last_node_id_{-1};
  /** @brief First callback exception retained for wait publication. */
  std::exception_ptr first_exception_;
};

TEST(M32InterfaceAbstraction, MockSchedulerLifecycle) {
  auto scheduler = std::make_unique<MockScheduler>();
  StubSchedulerHostContext host;

  EXPECT_FALSE(scheduler->is_running());
  EXPECT_EQ(scheduler->name(), "MockScheduler");
  EXPECT_FALSE(scheduler->was_attach_called());
  EXPECT_FALSE(scheduler->was_start_called());

  scheduler->attach(host);
  EXPECT_TRUE(scheduler->was_attach_called());
  EXPECT_EQ(scheduler->attached_host(), &host);

  scheduler->start();
  EXPECT_TRUE(scheduler->was_start_called());
  EXPECT_TRUE(scheduler->is_running());
  EXPECT_EQ(scheduler->host_at_start(), &host);

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

  CallbackTaskExecutor executor;
  const int ready_task_id = executor.add([&]() {
    counter.fetch_add(10);
    scheduler.dec_tasks_to_complete();
  });
  const int initial_task_id = executor.add([&]() {
    counter.fetch_add(1);
    scheduler.submit_ready_task_handles_from_worker(
        {{&executor, ready_task_id, 2}});
    scheduler.dec_tasks_to_complete();
  });

  scheduler.submit_initial_task_handles({{&executor, initial_task_id, 1}}, 2,
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
  EXPECT_EQ(hp_ptr->attached_host(), &runtime);

  auto rt_scheduler = std::make_unique<MockScheduler>();
  MockScheduler* rt_ptr = rt_scheduler.get();
  runtime.set_scheduler(ComputeIntent::RealTimeUpdate, std::move(rt_scheduler));

  EXPECT_TRUE(runtime.has_scheduler(ComputeIntent::RealTimeUpdate));
  EXPECT_EQ(runtime.get_scheduler(ComputeIntent::RealTimeUpdate), rt_ptr);
  EXPECT_TRUE(runtime.has_scheduler(ComputeIntent::GlobalHighPrecision));
}

TEST_F(GraphRuntimeSchedulerTest, StartStartsAttachedSchedulers) {
  GraphRuntime::Info info{"scheduler_test", "sessions/scheduler_test_session",
                          "", ""};

  GraphRuntime runtime(info);

  auto scheduler = std::make_unique<MockScheduler>();
  MockScheduler* scheduler_ptr = scheduler.get();
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::move(scheduler));

  EXPECT_TRUE(scheduler_ptr->was_attach_called());
  EXPECT_FALSE(scheduler_ptr->was_start_called());
  EXPECT_FALSE(scheduler_ptr->is_running());

  runtime.start();

  EXPECT_TRUE(scheduler_ptr->was_start_called());
  EXPECT_TRUE(scheduler_ptr->is_running());
  EXPECT_EQ(scheduler_ptr->host_at_start(), &runtime);

  runtime.stop();

  EXPECT_TRUE(scheduler_ptr->was_shutdown_called());
  EXPECT_FALSE(scheduler_ptr->is_running());
}

TEST_F(GraphRuntimeSchedulerTest,
       SetSchedulerOnRunningRuntimeStartsAfterAttach) {
  GraphRuntime::Info info{"scheduler_test", "sessions/scheduler_test_session",
                          "", ""};

  GraphRuntime runtime(info);
  runtime.start();

  auto scheduler = std::make_unique<MockScheduler>();
  MockScheduler* scheduler_ptr = scheduler.get();
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::move(scheduler));

  EXPECT_TRUE(scheduler_ptr->was_attach_called());
  EXPECT_TRUE(scheduler_ptr->was_start_called());
  EXPECT_TRUE(scheduler_ptr->is_running());
  EXPECT_EQ(scheduler_ptr->attached_host(), &runtime);
  EXPECT_EQ(scheduler_ptr->host_at_start(), &runtime);

  runtime.stop();
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

/**
 * @brief Mock scheduler whose start can fail once with resource exhaustion.
 *
 * @note The object remains owned by GraphRuntime across the failed call so the
 * test can disable injection and prove a clean retry.
 */
class RetryableStartMockScheduler final : public MockScheduler {
 public:
  /**
   * @brief Starts normally or fails after publishing scheduler-local running.
   * @throws std::bad_alloc while failure injection is enabled.
   * @note The intentionally hostile ordering proves GraphRuntime tracks the
   * candidate before invoking start and rolls it back even when start throws
   * after a partial lifecycle publication.
   */
  void start() override {
    if (fail_start_) {
      MockScheduler::start();
      throw std::bad_alloc();
    }
    MockScheduler::start();
  }

  /**
   * @brief Enables or disables deterministic start failure.
   * @param fail True to throw from the next start attempt.
   * @throws Nothing.
   */
  void set_fail_start(bool fail) noexcept { fail_start_ = fail; }

 private:
  /** @brief Whether start currently injects resource exhaustion. */
  bool fail_start_ = true;
};

/**
 * @brief Mock that stops its local state before surfacing a lifecycle error.
 * @note GraphRuntime uses it to prove one hostile plugin-like scheduler cannot
 * skip cleanup of the remaining registered scheduler set.
 */
class ThrowingShutdownMockScheduler final : public MockScheduler {
 public:
  /**
   * @brief Stops normally and then reports one deterministic failure.
   * @throws std::runtime_error after local running state becomes false.
   * @note The post-transition throw models a hostile explicit plugin
   * lifecycle call while keeping destructor cleanup repeatable.
   */
  void shutdown() override {
    MockScheduler::shutdown();
    throw std::runtime_error("tracked shutdown failure");
  }
};

/**
 * @brief Mock whose running-state query rethrows one stable exception object.
 *
 * The failure is disabled while `GraphRuntime::start()` performs its state
 * query, then enabled immediately before `GraphRuntime::stop()`. Its inherited
 * shutdown remains successful so the test can prove a failed query does not
 * suppress cleanup of the same scheduler.
 *
 * @note The stored `std::exception_ptr` lets the test compare exception object
 * identity after the runtime completes its best-effort scheduler sweep.
 */
class ThrowingRunningQueryMockScheduler final : public MockScheduler {
 public:
  /**
   * @brief Creates the stable running-query failure used by the regression.
   * @throws std::bad_alloc if exception payload allocation fails.
   */
  ThrowingRunningQueryMockScheduler()
      : running_query_error_(std::make_exception_ptr(
            std::runtime_error("tracked running query failure"))) {}

  /**
   * @brief Reports local running state or rethrows the configured query error.
   * @return The inherited scheduler state while failure injection is disabled.
   * @throws std::runtime_error with the exact stored exception identity while
   * failure injection is enabled.
   * @note Query failure does not mutate local state; inherited `shutdown()`
   * remains available for the runtime's best-effort cleanup attempt.
   */
  bool is_running() const override {
    if (throw_on_running_query_) {
      std::rethrow_exception(running_query_error_);
    }
    return MockScheduler::is_running();
  }

  /**
   * @brief Enables or disables deterministic running-query failure.
   * @param enabled True to rethrow from subsequent `is_running()` calls.
   * @return Nothing.
   * @throws Nothing.
   */
  void set_throw_on_running_query(bool enabled) noexcept {
    throw_on_running_query_ = enabled;
  }

  /**
   * @brief Returns the stable exception identity used by `is_running()`.
   * @return Borrowed reference valid for this scheduler's lifetime.
   * @throws Nothing.
   */
  const std::exception_ptr& running_query_error() const noexcept {
    return running_query_error_;
  }

 private:
  /** @brief Whether `is_running()` currently rethrows the stored failure. */
  bool throw_on_running_query_ = false;
  /** @brief Stable exception object used to verify first-error identity. */
  std::exception_ptr running_query_error_;
};

/**
 * @brief Candidate lifecycle phase that deterministically fails in replacement.
 */
enum class ReplacementFailureStage {
  /** @brief Candidate preparation succeeds. */
  None,
  /** @brief Candidate attach publishes local attachment, then fails. */
  Attach,
  /** @brief Candidate start publishes local running state, then fails. */
  Start,
};

/**
 * @brief External observations retained after a failed candidate is destroyed.
 *
 * @note Tests use this state to verify lifecycle order and exception identity
 * without dereferencing the candidate after ownership rollback.
 */
struct ReplacementCandidateState {
  /**
   * @brief Creates stable preparation and cleanup exception objects.
   * @param stage Candidate preparation phase that should fail.
   * @param fail_shutdown Whether rollback shutdown should throw.
   * @param fail_detach Whether rollback detach should throw.
   * @throws std::bad_alloc If exception payload construction cannot allocate.
   */
  ReplacementCandidateState(ReplacementFailureStage stage, bool fail_shutdown,
                            bool fail_detach)
      : failure_stage(stage),
        shutdown_fails(fail_shutdown),
        detach_fails(fail_detach),
        preparation_error(std::make_exception_ptr(
            std::runtime_error("replacement candidate preparation failure"))),
        shutdown_error(std::make_exception_ptr(
            std::runtime_error("replacement candidate shutdown failure"))),
        detach_error(std::make_exception_ptr(
            std::runtime_error("replacement candidate detach failure"))) {}

  /** @brief Preparation phase selected for deterministic failure. */
  ReplacementFailureStage failure_stage;
  /** @brief Whether rollback shutdown rethrows its stable exception. */
  bool shutdown_fails;
  /** @brief Whether rollback detach rethrows its stable exception. */
  bool detach_fails;
  /** @brief Number of candidate attach calls. */
  int attach_calls = 0;
  /** @brief Number of candidate start calls. */
  int start_calls = 0;
  /** @brief Number of candidate shutdown calls. */
  int shutdown_calls = 0;
  /** @brief Number of candidate detach calls. */
  int detach_calls = 0;
  /** @brief Number of candidate destructor calls. */
  int destructor_calls = 0;
  /** @brief Whether the old scheduler was observable during candidate attach.
   */
  bool old_observed_during_attach = false;
  /** @brief Whether the old scheduler remained running during candidate attach.
   */
  bool old_running_during_attach = false;
  /** @brief Whether the old scheduler was observable during candidate start. */
  bool old_observed_during_start = false;
  /** @brief Whether the old scheduler remained running during candidate start.
   */
  bool old_running_during_start = false;
  /** @brief Candidate running state after rollback shutdown. */
  bool running_after_shutdown = true;
  /** @brief Whether rollback detach cleared the candidate runtime pointer. */
  bool detached_after_cleanup = false;
  /** @brief Exact candidate preparation failure. */
  std::exception_ptr preparation_error;
  /** @brief Exact secondary rollback shutdown failure. */
  std::exception_ptr shutdown_error;
  /** @brief Exact secondary rollback detach failure. */
  std::exception_ptr detach_error;
};

/**
 * @brief Scheduler candidate with observable attach/start rollback behavior.
 *
 * @note Preparation failures occur after inherited state publication so the
 * runtime must run both rollback stages before destroying the candidate.
 */
class TransactionalReplacementCandidate final : public MockScheduler {
 public:
  /**
   * @brief Binds external observations and the expected old scheduler.
   * @param state Shared state retained by the test after candidate destruction.
   * @param expected_old Old scheduler that must survive preparation.
   * @throws Nothing.
   */
  TransactionalReplacementCandidate(
      std::shared_ptr<ReplacementCandidateState> state,
      const MockScheduler* expected_old) noexcept
      : state_(std::move(state)), expected_old_(expected_old) {}

  /**
   * @brief Records destruction after rollback or displaced-owner cleanup.
   * @throws Nothing.
   */
  ~TransactionalReplacementCandidate() noexcept override {
    ++state_->destructor_calls;
  }

  /**
   * @brief Attaches locally, then optionally rethrows the stable preparation
   * error.
   * @param runtime Runtime supplied by the replacement transaction.
   * @return Nothing.
   * @throws std::runtime_error With exact stored identity for attach injection.
   */
  void attach(SchedulerHostContext& host) override {
    ++state_->attach_calls;
    state_->old_observed_during_attach = expected_old_ != nullptr;
    state_->old_running_during_attach =
        expected_old_ && expected_old_->is_running();
    MockScheduler::attach(host);
    if (state_->failure_stage == ReplacementFailureStage::Attach) {
      std::rethrow_exception(state_->preparation_error);
    }
  }

  /**
   * @brief Starts locally, then optionally rethrows the stable preparation
   * error.
   * @return Nothing.
   * @throws std::runtime_error With exact stored identity for start injection.
   */
  void start() override {
    ++state_->start_calls;
    state_->old_observed_during_start = expected_old_ != nullptr;
    state_->old_running_during_start =
        expected_old_ && expected_old_->is_running();
    MockScheduler::start();
    if (state_->failure_stage == ReplacementFailureStage::Start) {
      std::rethrow_exception(state_->preparation_error);
    }
  }

  /**
   * @brief Stops locally, then optionally reports a secondary rollback failure.
   * @return Nothing.
   * @throws std::runtime_error With exact stored cleanup identity when enabled.
   */
  void shutdown() override {
    ++state_->shutdown_calls;
    MockScheduler::shutdown();
    state_->running_after_shutdown = MockScheduler::is_running();
    if (state_->shutdown_fails) {
      std::rethrow_exception(state_->shutdown_error);
    }
  }

  /**
   * @brief Detaches locally, then optionally reports a secondary rollback
   * failure.
   * @return Nothing.
   * @throws std::runtime_error With exact stored cleanup identity when enabled.
   */
  void detach() override {
    ++state_->detach_calls;
    MockScheduler::detach();
    state_->detached_after_cleanup = attached_host() == nullptr;
    if (state_->detach_fails) {
      std::rethrow_exception(state_->detach_error);
    }
  }

 private:
  /** @brief External observation state owned jointly with the test. */
  std::shared_ptr<ReplacementCandidateState> state_;
  /** @brief Non-owning old scheduler pointer valid throughout preparation. */
  const MockScheduler* expected_old_;
};

/**
 * @brief GraphRuntime rolls back earlier schedulers when a later start fails.
 */
TEST_F(GraphRuntimeSchedulerTest, StartFailureRollsBackAndRetrySucceeds) {
  GraphRuntime::Info info{"scheduler_start_rollback",
                          "sessions/scheduler_test_session", "", ""};
  GraphRuntime runtime(info);

  auto hp_scheduler = std::make_unique<MockScheduler>();
  MockScheduler* hp_ptr = hp_scheduler.get();
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::move(hp_scheduler));
  auto rt_scheduler = std::make_unique<RetryableStartMockScheduler>();
  RetryableStartMockScheduler* rt_ptr = rt_scheduler.get();
  runtime.set_scheduler(ComputeIntent::RealTimeUpdate, std::move(rt_scheduler));

  EXPECT_THROW(runtime.start(), std::bad_alloc);
  EXPECT_FALSE(runtime.running());
  EXPECT_TRUE(hp_ptr->was_start_called());
  EXPECT_TRUE(hp_ptr->was_shutdown_called());
  EXPECT_FALSE(hp_ptr->is_running());
  EXPECT_FALSE(rt_ptr->is_running());
  EXPECT_TRUE(rt_ptr->was_shutdown_called());
  EXPECT_NO_THROW(runtime.stop());

  rt_ptr->set_fail_start(false);
  EXPECT_NO_THROW(runtime.start());
  EXPECT_TRUE(runtime.running());
  EXPECT_TRUE(hp_ptr->is_running());
  EXPECT_TRUE(rt_ptr->is_running());
  runtime.stop();
  EXPECT_FALSE(runtime.running());
}

/**
 * @brief GraphRuntime stops every scheduler before rethrowing the first error.
 * @throws Nothing when the stopped publication and best-effort sweep hold.
 * @note The runtime remains reusable for destruction after this explicit
 * failure because the throwing scheduler transitions to stopped first.
 */
TEST_F(GraphRuntimeSchedulerTest,
       StopSweepsRemainingSchedulersAndPreservesFirstFailure) {
  GraphRuntime::Info info{"scheduler_stop_sweep",
                          "sessions/scheduler_test_session", "", ""};
  GraphRuntime runtime(info);

  auto hp_scheduler = std::make_unique<ThrowingShutdownMockScheduler>();
  ThrowingShutdownMockScheduler* hp_ptr = hp_scheduler.get();
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::move(hp_scheduler));
  auto rt_scheduler = std::make_unique<MockScheduler>();
  MockScheduler* rt_ptr = rt_scheduler.get();
  runtime.set_scheduler(ComputeIntent::RealTimeUpdate, std::move(rt_scheduler));
  runtime.start();

  try {
    runtime.stop();
    FAIL() << "hostile shutdown did not propagate";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "tracked shutdown failure");
  }
  EXPECT_FALSE(runtime.running());
  EXPECT_FALSE(hp_ptr->is_running());
  EXPECT_FALSE(rt_ptr->is_running());
  EXPECT_TRUE(rt_ptr->was_shutdown_called());
}

/**
 * @brief A failed running-state query cannot skip any scheduler shutdown.
 * @return Nothing.
 * @throws Nothing when the runtime stops both schedulers, publishes stopped
 * state, and rethrows the exact first query exception after the sweep.
 * @note The later scheduler also throws after its own shutdown, proving that
 * the earlier query error retains both identity and diagnostic message.
 */
TEST_F(GraphRuntimeSchedulerTest,
       StopRunningQueryFailureStillSweepsAndPreservesFirstError) {
  GraphRuntime::Info info{"scheduler_stop_query_failure",
                          "sessions/scheduler_test_session", "", ""};
  GraphRuntime runtime(info);

  auto hp_scheduler = std::make_unique<ThrowingRunningQueryMockScheduler>();
  ThrowingRunningQueryMockScheduler* hp_ptr = hp_scheduler.get();
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::move(hp_scheduler));
  auto rt_scheduler = std::make_unique<ThrowingShutdownMockScheduler>();
  ThrowingShutdownMockScheduler* rt_ptr = rt_scheduler.get();
  runtime.set_scheduler(ComputeIntent::RealTimeUpdate, std::move(rt_scheduler));
  runtime.start();
  hp_ptr->set_throw_on_running_query(true);

  try {
    runtime.stop();
    FAIL() << "running-state query failure did not propagate";
  } catch (const std::runtime_error& error) {
    EXPECT_EQ(std::current_exception(), hp_ptr->running_query_error());
    EXPECT_STREQ(error.what(), "tracked running query failure");
  }

  EXPECT_FALSE(runtime.running());
  EXPECT_TRUE(hp_ptr->was_shutdown_called());
  EXPECT_FALSE(hp_ptr->is_running());
  EXPECT_TRUE(rt_ptr->was_shutdown_called());
  EXPECT_FALSE(rt_ptr->is_running());
}

/**
 * @brief Attach failure preserves a running old scheduler and cleans candidate.
 * @return Nothing.
 * @throws Nothing when the transaction retains old ownership and exact error.
 */
TEST_F(GraphRuntimeSchedulerTest,
       ReplaceAttachFailurePreservesRunningOldScheduler) {
  GraphRuntime::Info info{"scheduler_replace_attach_failure",
                          "sessions/scheduler_test_session", "", ""};
  GraphRuntime runtime(info);
  auto old_scheduler = std::make_unique<MockScheduler>();
  MockScheduler* old_ptr = old_scheduler.get();
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::move(old_scheduler));
  runtime.start();

  auto state = std::make_shared<ReplacementCandidateState>(
      ReplacementFailureStage::Attach, false, false);
  auto candidate =
      std::make_unique<TransactionalReplacementCandidate>(state, old_ptr);
  try {
    runtime.replace_scheduler(ComputeIntent::GlobalHighPrecision,
                              std::move(candidate));
    FAIL() << "replacement attach failure did not propagate";
  } catch (const std::runtime_error& error) {
    EXPECT_EQ(std::current_exception(), state->preparation_error);
    EXPECT_STREQ(error.what(), "replacement candidate preparation failure");
  }

  EXPECT_TRUE(runtime.running());
  EXPECT_EQ(runtime.get_scheduler(ComputeIntent::GlobalHighPrecision), old_ptr);
  EXPECT_TRUE(old_ptr->is_running());
  EXPECT_TRUE(state->old_observed_during_attach);
  EXPECT_TRUE(state->old_running_during_attach);
  EXPECT_EQ(state->attach_calls, 1);
  EXPECT_EQ(state->start_calls, 0);
  EXPECT_EQ(state->shutdown_calls, 1);
  EXPECT_EQ(state->detach_calls, 1);
  EXPECT_EQ(state->destructor_calls, 1);
  EXPECT_FALSE(state->running_after_shutdown);
  EXPECT_TRUE(state->detached_after_cleanup);
  runtime.stop();
}

/**
 * @brief Partial candidate start failure survives hostile rollback cleanup.
 * @return Nothing.
 * @throws Nothing when shutdown/detach are both attempted and the exact
 * preparation exception wins over both secondary failures.
 */
TEST_F(GraphRuntimeSchedulerTest,
       ReplaceStartFailurePreservesOriginalAcrossCleanupFailures) {
  GraphRuntime::Info info{"scheduler_replace_start_failure",
                          "sessions/scheduler_test_session", "", ""};
  GraphRuntime runtime(info);
  auto old_scheduler = std::make_unique<MockScheduler>();
  MockScheduler* old_ptr = old_scheduler.get();
  runtime.set_scheduler(ComputeIntent::RealTimeUpdate,
                        std::move(old_scheduler));
  runtime.start();

  auto state = std::make_shared<ReplacementCandidateState>(
      ReplacementFailureStage::Start, true, true);
  auto candidate =
      std::make_unique<TransactionalReplacementCandidate>(state, old_ptr);
  try {
    runtime.replace_scheduler(ComputeIntent::RealTimeUpdate,
                              std::move(candidate));
    FAIL() << "replacement start failure did not propagate";
  } catch (const std::runtime_error& error) {
    EXPECT_EQ(std::current_exception(), state->preparation_error);
    EXPECT_NE(std::current_exception(), state->shutdown_error);
    EXPECT_NE(std::current_exception(), state->detach_error);
    EXPECT_STREQ(error.what(), "replacement candidate preparation failure");
  }

  EXPECT_TRUE(runtime.running());
  EXPECT_EQ(runtime.get_scheduler(ComputeIntent::RealTimeUpdate), old_ptr);
  EXPECT_TRUE(old_ptr->is_running());
  EXPECT_TRUE(state->old_observed_during_attach);
  EXPECT_TRUE(state->old_running_during_attach);
  EXPECT_TRUE(state->old_observed_during_start);
  EXPECT_TRUE(state->old_running_during_start);
  EXPECT_EQ(state->attach_calls, 1);
  EXPECT_EQ(state->start_calls, 1);
  EXPECT_EQ(state->shutdown_calls, 1);
  EXPECT_EQ(state->detach_calls, 1);
  EXPECT_EQ(state->destructor_calls, 1);
  EXPECT_FALSE(state->running_after_shutdown);
  EXPECT_TRUE(state->detached_after_cleanup);
  runtime.stop();
}

/**
 * @brief set_scheduler shares replacement rollback for an existing owner.
 * @return Nothing.
 * @throws Nothing when attach failure keeps the stopped old owner published.
 */
TEST_F(GraphRuntimeSchedulerTest,
       SetSchedulerReplacementFailurePreservesStoppedOldScheduler) {
  GraphRuntime::Info info{"scheduler_set_replacement_failure",
                          "sessions/scheduler_test_session", "", ""};
  GraphRuntime runtime(info);
  auto old_scheduler = std::make_unique<MockScheduler>();
  MockScheduler* old_ptr = old_scheduler.get();
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::move(old_scheduler));

  auto state = std::make_shared<ReplacementCandidateState>(
      ReplacementFailureStage::Attach, false, false);
  auto candidate =
      std::make_unique<TransactionalReplacementCandidate>(state, old_ptr);
  EXPECT_THROW(runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                                     std::move(candidate)),
               std::runtime_error);

  EXPECT_FALSE(runtime.running());
  EXPECT_EQ(runtime.get_scheduler(ComputeIntent::GlobalHighPrecision), old_ptr);
  EXPECT_EQ(old_ptr->attached_host(), &runtime);
  EXPECT_FALSE(old_ptr->is_running());
  EXPECT_TRUE(state->old_observed_during_attach);
  EXPECT_FALSE(state->old_running_during_attach);
  EXPECT_EQ(state->shutdown_calls, 1);
  EXPECT_EQ(state->detach_calls, 1);
  EXPECT_EQ(state->destructor_calls, 1);
}

/**
 * @brief Successful replacement on a stopped runtime publishes without start.
 * @return Nothing.
 * @throws Nothing when the old owner is shut down then detached after publish.
 */
TEST_F(GraphRuntimeSchedulerTest,
       ReplaceOnStoppedRuntimePublishesAttachedUnstartedCandidate) {
  GraphRuntime::Info info{"scheduler_replace_stopped",
                          "sessions/scheduler_test_session", "", ""};
  GraphRuntime runtime(info);
  SchedulerLifecycleTracker::reset();

  auto old_scheduler = std::make_unique<TrackedMockScheduler>();
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::move(old_scheduler));
  auto candidate = std::make_unique<MockScheduler>();
  MockScheduler* candidate_ptr = candidate.get();
  runtime.replace_scheduler(ComputeIntent::GlobalHighPrecision,
                            std::move(candidate));

  EXPECT_FALSE(runtime.running());
  EXPECT_EQ(runtime.get_scheduler(ComputeIntent::GlobalHighPrecision),
            candidate_ptr);
  EXPECT_TRUE(candidate_ptr->was_attach_called());
  EXPECT_FALSE(candidate_ptr->was_start_called());
  EXPECT_FALSE(candidate_ptr->is_running());
  EXPECT_EQ(SchedulerLifecycleTracker::shutdown_count.load(), 1);
  EXPECT_EQ(SchedulerLifecycleTracker::detach_count.load(), 1);
}

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
