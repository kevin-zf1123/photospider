#include <gtest/gtest.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "runtime/resource_ledger.hpp"
#include "scheduler/scheduler_factory.hpp"
#include "scheduler/scheduler_plugin_loader.hpp"
#include "scheduler/scheduler_reservation_owner.hpp"

namespace ps {
namespace {

/** @brief Registered test type whose factory deliberately returns null. */
constexpr char kNullSchedulerType[] = "issue_43_null_scheduler";

/** @brief Registered test type whose factory deliberately throws. */
constexpr char kThrowingSchedulerType[] = "issue_43_throwing_scheduler";

/**
 * @brief Records the complete scheduler surface observed behind an owner.
 * @throws std::bad_alloc When dynamic evidence storage cannot allocate.
 * @note Tests retain this state after the concrete probe has been destroyed.
 */
struct ProbeSchedulerState final {
  /** @brief Ordered lifecycle and destruction evidence. */
  std::vector<std::string> lifecycle_events;
  /** @brief Last host context forwarded through attach. */
  SchedulerHostContext* attached_host = nullptr;
  /** @brief True between forwarded start and shutdown calls. */
  bool running = false;
  /** @brief Initial handle batch forwarded by the owner. */
  std::vector<TaskHandle> initial_handles;
  /** @brief Total task count paired with the initial batch. */
  int initial_total_task_count = 0;
  /** @brief Priority paired with the initial batch. */
  SchedulerTaskPriority initial_priority = SchedulerTaskPriority::Normal;
  /** @brief Worker-origin handle batch forwarded by the owner. */
  std::vector<TaskHandle> worker_handles;
  /** @brief Priority paired with the worker-origin batch. */
  SchedulerTaskPriority worker_priority = SchedulerTaskPriority::Normal;
  /** @brief Priority paired with the any-thread callback. */
  SchedulerTaskPriority callback_priority = SchedulerTaskPriority::Normal;
  /** @brief Epoch paired with the any-thread callback. */
  std::optional<std::uint64_t> callback_epoch;
  /** @brief True after the forwarded callback executes. */
  bool callback_executed = false;
  /** @brief True after wait_for_completion reaches the probe. */
  bool waited = false;
  /** @brief Whether forwarded shutdown deliberately raises an exception. */
  bool throw_on_shutdown = false;
  /** @brief Whether forwarded detach deliberately raises an exception. */
  bool throw_on_detach = false;
  /** @brief Exact exception identity forwarded to set_exception. */
  std::exception_ptr exception;
  /** @brief Aggregate positive completion increments forwarded to the probe. */
  int task_increment = 0;
  /** @brief Number of completion decrements forwarded to the probe. */
  int task_decrements = 0;
  /** @brief Last trace action forwarded to the probe. */
  SchedulerTraceAction trace_action = SchedulerTraceAction::AssignInitial;
  /** @brief Last trace node forwarded to the probe. */
  int trace_node_id = -1;
  /** @brief Optional ledger inspected from the concrete destructor. */
  ResourceLedger* destruction_ledger = nullptr;
  /** @brief Whether capacity was already free during concrete destruction. */
  bool capacity_available_during_destroy = true;
};

/**
 * @brief Minimal host context whose address proves attach forwarding.
 * @throws Nothing.
 * @note Capability and observation behavior are irrelevant to this owner test.
 */
class ProbeHostContext final : public SchedulerHostContext {
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
};

/**
 * @brief Task executor used to preserve borrowed handle identity in batches.
 * @throws Nothing.
 * @note The forwarding test never asks the probe scheduler to run a handle.
 */
class ProbeTaskExecutor final : public TaskExecutor {
 public:
  /** @copydoc TaskExecutor::run_task */
  void run_task(int task_id) override { last_task_id = task_id; }

  /** @brief Last task id executed directly through a retained handle. */
  int last_task_id = -1;
};

/**
 * @brief Complete scheduler probe used to verify transparent owner forwarding.
 * @throws std::bad_alloc If evidence recording cannot allocate.
 * @note Its destructor probes admission before the wrapper reservation may
 * release, turning ownership order into deterministic behavioral evidence.
 */
class ProbeScheduler final : public IScheduler {
 public:
  /**
   * @brief Creates a probe backed by state retained by the test.
   * @param state Shared observation state.
   * @throws Nothing.
   */
  explicit ProbeScheduler(std::shared_ptr<ProbeSchedulerState> state) noexcept
      : state_(std::move(state)) {}

  /**
   * @brief Records destruction and tests whether reservation capacity leaked.
   * @throws Nothing; admission exceptions terminate at this test destructor.
   */
  ~ProbeScheduler() noexcept override {
    state_->lifecycle_events.push_back("destroy");
    if (state_->destruction_ledger != nullptr) {
      state_->capacity_available_during_destroy =
          state_->destruction_ledger->try_reserve(ResourceVector{1U})
              .has_value();
    }
  }

  /** @copydoc IScheduler::attach */
  void attach(SchedulerHostContext& host) override {
    state_->attached_host = &host;
    state_->lifecycle_events.push_back("attach");
  }

  /** @copydoc IScheduler::detach */
  void detach() override {
    state_->attached_host = nullptr;
    state_->lifecycle_events.push_back("detach");
    if (state_->throw_on_detach) {
      throw std::runtime_error("injected detach failure");
    }
  }

  /** @copydoc IScheduler::start */
  void start() override {
    state_->running = true;
    state_->lifecycle_events.push_back("start");
  }

  /** @copydoc IScheduler::shutdown */
  void shutdown() override {
    state_->running = false;
    state_->lifecycle_events.push_back("shutdown");
    if (state_->throw_on_shutdown) {
      throw std::runtime_error("injected shutdown failure");
    }
  }

  /** @copydoc IScheduler::name */
  std::string name() const override { return "reservation_owner_probe"; }

  /** @copydoc IScheduler::get_stats */
  std::string get_stats() const override { return "probe_stats"; }

  /** @copydoc IScheduler::is_running */
  bool is_running() const override { return state_->running; }

  /** @copydoc SchedulerTaskRuntime::available_devices */
  std::vector<Device> available_devices() const override {
    return {Device::GPU_METAL, Device::CPU};
  }

  /** @copydoc SchedulerTaskRuntime::submit_initial_task_handles */
  void submit_initial_task_handles(
      std::vector<TaskHandle>&& handles, int total_task_count,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
    state_->initial_handles = std::move(handles);
    state_->initial_total_task_count = total_task_count;
    state_->initial_priority = priority;
  }

  /** @copydoc SchedulerTaskRuntime::submit_ready_task_handles_from_worker */
  void submit_ready_task_handles_from_worker(
      std::vector<TaskHandle>&& handles,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
    state_->worker_handles = std::move(handles);
    state_->worker_priority = priority;
  }

  /** @copydoc SchedulerTaskRuntime::submit_ready_task_any_thread */
  void submit_ready_task_any_thread(
      Task&& task,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal,
      std::optional<std::uint64_t> epoch = std::nullopt) override {
    state_->callback_priority = priority;
    state_->callback_epoch = epoch;
    task();
  }

  /** @copydoc SchedulerTaskRuntime::wait_for_completion */
  void wait_for_completion() override { state_->waited = true; }

  /** @copydoc SchedulerTaskRuntime::set_exception */
  void set_exception(std::exception_ptr error) override {
    state_->exception = error;
  }

  /** @copydoc SchedulerTaskRuntime::inc_tasks_to_complete */
  void inc_tasks_to_complete(int delta) override {
    state_->task_increment += delta;
  }

  /** @copydoc SchedulerTaskRuntime::dec_tasks_to_complete */
  void dec_tasks_to_complete() override { ++state_->task_decrements; }

  /** @copydoc SchedulerTaskRuntime::log_event */
  void log_event(SchedulerTraceAction action, int node_id) override {
    state_->trace_action = action;
    state_->trace_node_id = node_id;
  }

 private:
  /** @brief State retained beyond this concrete scheduler lifetime. */
  std::shared_ptr<ProbeSchedulerState> state_;
};

/**
 * @brief Registers null and throwing host-owned factories once per process.
 * @return Nothing.
 * @throws std::bad_alloc If registry/callable storage cannot allocate.
 */
void ensure_failure_factories_registered() {
  static std::once_flag once;
  std::call_once(once, [] {
    SchedulerPluginLoader::instance().register_builtin(
        kNullSchedulerType, "returns null for reservation rollback testing",
        [](unsigned int) -> std::unique_ptr<IScheduler> { return nullptr; });
    SchedulerPluginLoader::instance().register_builtin(
        kThrowingSchedulerType, "throws for reservation rollback testing",
        [](unsigned int) -> std::unique_ptr<IScheduler> {
          throw std::runtime_error("injected scheduler construction failure");
        });
  });
}

/**
 * @brief Proves the owner forwards the complete inherited scheduler surface.
 * @throws Nothing when every argument, result, and moved value is preserved.
 */
TEST(SchedulerReservationOwner, ForwardsCompleteSchedulerSurface) {
  ResourceLedger ledger(ResourceVector{1U});
  auto reservation = ledger.try_reserve(ResourceVector{1U});
  ASSERT_TRUE(reservation.has_value());
  auto state = std::make_shared<ProbeSchedulerState>();
  auto scheduler = make_reservation_owned_scheduler(
      std::make_unique<ProbeScheduler>(state), std::move(*reservation));
  ASSERT_NE(scheduler, nullptr);

  ProbeHostContext host;
  ProbeTaskExecutor executor;
  scheduler->attach(host);
  scheduler->start();
  EXPECT_EQ(scheduler->name(), "reservation_owner_probe");
  EXPECT_EQ(scheduler->get_stats(), "probe_stats");
  EXPECT_TRUE(scheduler->is_running());
  EXPECT_EQ(scheduler->available_devices(),
            (std::vector<Device>{Device::GPU_METAL, Device::CPU}));

  std::vector<TaskHandle> initial{{&executor, 3, 30}, {&executor, 4, 40}};
  scheduler->submit_initial_task_handles(std::move(initial), 7,
                                         SchedulerTaskPriority::High);
  ASSERT_EQ(state->initial_handles.size(), 2U);
  EXPECT_EQ(state->initial_handles[0].executor, &executor);
  EXPECT_EQ(state->initial_handles[0].task_id, 3);
  EXPECT_EQ(state->initial_handles[1].node_id, 40);
  EXPECT_EQ(state->initial_total_task_count, 7);
  EXPECT_EQ(state->initial_priority, SchedulerTaskPriority::High);

  std::vector<TaskHandle> worker{{&executor, 5, 50}};
  scheduler->submit_ready_task_handles_from_worker(std::move(worker),
                                                   SchedulerTaskPriority::High);
  ASSERT_EQ(state->worker_handles.size(), 1U);
  EXPECT_EQ(state->worker_handles[0].task_id, 5);
  EXPECT_EQ(state->worker_priority, SchedulerTaskPriority::High);

  scheduler->submit_ready_task_any_thread(
      [&state] { state->callback_executed = true; },
      SchedulerTaskPriority::High, 42U);
  EXPECT_TRUE(state->callback_executed);
  EXPECT_EQ(state->callback_priority, SchedulerTaskPriority::High);
  EXPECT_EQ(state->callback_epoch, 42U);

  const std::exception_ptr error =
      std::make_exception_ptr(std::runtime_error("probe task failure"));
  scheduler->set_exception(error);
  scheduler->inc_tasks_to_complete(6);
  scheduler->dec_tasks_to_complete();
  scheduler->log_event(SchedulerTraceAction::ExecuteTile, 91);
  scheduler->wait_for_completion();
  EXPECT_EQ(state->exception, error);
  EXPECT_EQ(state->task_increment, 6);
  EXPECT_EQ(state->task_decrements, 1);
  EXPECT_EQ(state->trace_action, SchedulerTraceAction::ExecuteTile);
  EXPECT_EQ(state->trace_node_id, 91);
  EXPECT_TRUE(state->waited);

  scheduler->shutdown();
  scheduler->detach();
  EXPECT_FALSE(scheduler->is_running());
  scheduler.reset();
  EXPECT_EQ(state->lifecycle_events,
            (std::vector<std::string>{"attach", "start", "shutdown", "detach",
                                      "destroy"}));
}

/**
 * @brief Proves fallback teardown and concrete destruction precede release.
 * @throws Nothing when the exact slot remains charged until destruction ends.
 */
TEST(SchedulerReservationOwner,
     TearsDownAndDestroysConcreteSchedulerBeforeReservationRelease) {
  ResourceLedger ledger(ResourceVector{1U});
  auto reservation = ledger.try_reserve(ResourceVector{1U});
  ASSERT_TRUE(reservation.has_value());
  auto state = std::make_shared<ProbeSchedulerState>();
  state->destruction_ledger = &ledger;
  auto scheduler = make_reservation_owned_scheduler(
      std::make_unique<ProbeScheduler>(state), std::move(*reservation));
  ASSERT_NE(scheduler, nullptr);

  ProbeHostContext host;
  scheduler->attach(host);
  scheduler->start();
  EXPECT_FALSE(ledger.try_reserve(ResourceVector{1U}).has_value());
  scheduler->shutdown();
  scheduler->detach();
  scheduler.reset();

  EXPECT_EQ(state->lifecycle_events,
            (std::vector<std::string>{"attach", "start", "shutdown", "detach",
                                      "destroy"}));
  EXPECT_FALSE(state->capacity_available_during_destroy);
  EXPECT_TRUE(ledger.try_reserve(ResourceVector{1U}).has_value());
}

/**
 * @brief Proves lifecycle failures remain exact and cannot release capacity.
 * @throws Nothing when the owner transparently propagates both probe errors.
 */
TEST(SchedulerReservationOwner,
     LifecycleExceptionsPreserveReservationUntilConcreteDestruction) {
  ResourceLedger ledger(ResourceVector{1U});
  auto reservation = ledger.try_reserve(ResourceVector{1U});
  ASSERT_TRUE(reservation.has_value());
  auto state = std::make_shared<ProbeSchedulerState>();
  state->destruction_ledger = &ledger;
  state->throw_on_shutdown = true;
  state->throw_on_detach = true;
  auto scheduler = make_reservation_owned_scheduler(
      std::make_unique<ProbeScheduler>(state), std::move(*reservation));
  ASSERT_NE(scheduler, nullptr);

  ProbeHostContext host;
  scheduler->attach(host);
  scheduler->start();
  EXPECT_THROW(scheduler->shutdown(), std::runtime_error);
  EXPECT_FALSE(ledger.try_reserve(ResourceVector{1U}).has_value());
  EXPECT_THROW(scheduler->detach(), std::runtime_error);
  EXPECT_FALSE(ledger.try_reserve(ResourceVector{1U}).has_value());
  scheduler.reset();

  EXPECT_EQ(state->lifecycle_events,
            (std::vector<std::string>{"attach", "start", "shutdown", "detach",
                                      "destroy"}));
  EXPECT_FALSE(state->capacity_available_during_destroy);
  EXPECT_TRUE(ledger.try_reserve(ResourceVector{1U}).has_value());
}

/**
 * @brief Proves null and throwing scheduler factories return candidate slots.
 * @throws Nothing when both failure modes immediately recover exact capacity.
 */
TEST(SchedulerReservationOwner, FactoryFailureReturnsCandidateReservation) {
  ensure_failure_factories_registered();
  ResourceLedger ledger(ResourceVector{1U});

  const auto null_plan =
      SchedulerFactory::plan_for_hardware(kNullSchedulerType, 1U, 1U);
  ASSERT_TRUE(null_plan.has_value());
  auto null_reservation = ledger.try_reserve(ResourceVector{1U});
  ASSERT_TRUE(null_reservation.has_value());
  EXPECT_EQ(SchedulerFactory::create(*null_plan, std::move(*null_reservation)),
            nullptr);
  EXPECT_TRUE(ledger.try_reserve(ResourceVector{1U}).has_value());

  const auto throwing_plan =
      SchedulerFactory::plan_for_hardware(kThrowingSchedulerType, 1U, 1U);
  ASSERT_TRUE(throwing_plan.has_value());
  auto throwing_reservation = ledger.try_reserve(ResourceVector{1U});
  ASSERT_TRUE(throwing_reservation.has_value());
  EXPECT_THROW((void)SchedulerFactory::create(*throwing_plan,
                                              std::move(*throwing_reservation)),
               std::runtime_error);
  EXPECT_TRUE(ledger.try_reserve(ResourceVector{1U}).has_value());
}

/**
 * @brief Proves reserved construction rejects inactive or mismatched owners.
 * @throws Nothing when validation happens before concrete construction.
 */
TEST(SchedulerReservationOwner, FactoryRejectsInvalidReservationOwnership) {
  const auto plan =
      SchedulerFactory::plan_for_hardware("cpu_work_stealing", 1U, 1U);
  ASSERT_TRUE(plan.has_value());

  ResourceLedger ledger(ResourceVector{1U});
  {
    auto inactive_source = ledger.try_reserve(ResourceVector{1U});
    ASSERT_TRUE(inactive_source.has_value());
    ResourceLedger::Reservation active_owner(std::move(*inactive_source));
    EXPECT_THROW(
        (void)SchedulerFactory::create(*plan, std::move(*inactive_source)),
        std::invalid_argument);
    EXPECT_TRUE(active_owner.active());
  }

  auto wrong_size = ledger.try_reserve(ResourceVector{});
  ASSERT_TRUE(wrong_size.has_value());
  EXPECT_THROW((void)SchedulerFactory::create(*plan, std::move(*wrong_size)),
               std::invalid_argument);
  EXPECT_TRUE(ledger.try_reserve(ResourceVector{1U}).has_value());
}

}  // namespace
}  // namespace ps
