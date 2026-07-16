#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "photospider/scheduler/scheduler.hpp"
#include "scheduler/cpu_work_stealing_scheduler.hpp"
#include "scheduler/gpu_pipeline_scheduler.hpp"
#include "scheduler/scheduler_exception_test_hooks.hpp"
#include "scheduler/scheduler_factory.hpp"
#include "scheduler/scheduler_worker_budget.hpp"

namespace ps::testing {
namespace {

/**
 * @brief Records deterministic scheduler resource/thread hook entrances.
 * @throws Nothing for construction and atomic inspection.
 * @note A selected point may throw one tagged system error for rollback tests.
 */
struct LifecycleFailureProbe final {
  /** @brief Lifecycle point selected for failure injection. */
  SchedulerFailurePoint selected_point =
      SchedulerFailurePoint::StartResourceAllocation;
  /** @brief One-based selected point attempt that throws, or zero for none. */
  std::size_t fail_on_attempt = 0U;
  /** @brief Total resource/thread hook entrances across all point kinds. */
  std::atomic<std::size_t> total_attempts{0U};
};

/**
 * @brief Records one hook entrance and optionally injects thread creation loss.
 * @param context Borrowed `LifecycleFailureProbe` owned by the active test.
 * @param point Scheduler lifecycle point about to execute.
 * @param attempt One-based point-local attempt.
 * @return Nothing when the selected attempt should proceed.
 * @throws std::system_error At the exact configured point and attempt.
 */
void observe_or_fail_lifecycle(void* context, SchedulerFailurePoint point,
                               std::size_t attempt) {
  auto* probe = static_cast<LifecycleFailureProbe*>(context);
  probe->total_attempts.fetch_add(1U, std::memory_order_relaxed);
  if (point == probe->selected_point && attempt == probe->fail_on_attempt) {
    throw std::system_error(
        std::make_error_code(std::errc::resource_unavailable_try_again),
        "issue-43 injected scheduler thread creation failure");
  }
}

/** @brief Scheduler-specific deterministic failure-hook setter type. */
using FailureHookSetter = decltype(&set_cpu_scheduler_failure_injection_hook);

/**
 * @brief Installs and clears one borrowed scheduler failure hook.
 * @throws Nothing.
 * @note The probe and hook values outlive this guard.
 */
class ScopedFailureHook final {
 public:
  /**
   * @brief Installs one scheduler-specific hook.
   * @param setter CPU or GPU hook setter retained by value.
   * @param hook Borrowed hook value retained by scheduler test storage.
   * @throws Nothing.
   */
  ScopedFailureHook(FailureHookSetter setter,
                    const SchedulerFailureInjectionHook* hook) noexcept
      : setter_(setter) {
    setter_(hook);
  }

  /**
   * @brief Clears the hook before borrowed test storage is destroyed.
   * @throws Nothing.
   * @note The scheduler operation has returned before this guard is destroyed.
   */
  ~ScopedFailureHook() noexcept { setter_(nullptr); }

  /**
   * @brief Prevents duplicate ownership of the installed global hook.
   * @param other Guard that remains the sole cleanup owner.
   * @throws Nothing because the operation is deleted.
   */
  ScopedFailureHook(const ScopedFailureHook&) = delete;

  /**
   * @brief Prevents replacing one installed-hook guard by assignment.
   * @param other Guard that remains unchanged.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ScopedFailureHook& operator=(const ScopedFailureHook&) = delete;

 private:
  /** @brief Scheduler-specific non-owning hook setter. */
  FailureHookSetter setter_;
};

/**
 * @brief Enables deterministic GPU availability and clears it at scope exit.
 * @throws Nothing.
 */
class ScopedForcedGpuRoute final {
 public:
  /**
   * @brief Enables the BUILD_TESTING-only GPU availability override.
   * @throws Nothing.
   * @note The override affects only scheduler code compiled for tests.
   */
  ScopedForcedGpuRoute() noexcept { set_gpu_scheduler_force_gpu_route(true); }

  /**
   * @brief Clears the GPU override before another test begins.
   * @throws Nothing.
   * @note All workers using the override are joined before scope exit.
   */
  ~ScopedForcedGpuRoute() noexcept { set_gpu_scheduler_force_gpu_route(false); }

  /**
   * @brief Prevents duplicate ownership of the global override.
   * @param other Guard that remains the sole cleanup owner.
   * @throws Nothing because the operation is deleted.
   */
  ScopedForcedGpuRoute(const ScopedForcedGpuRoute&) = delete;

  /**
   * @brief Prevents assignment across global override guards.
   * @param other Guard that remains unchanged.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ScopedForcedGpuRoute& operator=(const ScopedForcedGpuRoute&) = delete;
};

/**
 * @brief Mutable host capability used to trigger GPU availability after start.
 * @throws Nothing.
 * @note Lifecycle calls are serialized by each focused test.
 */
class MutableGpuHostContext final : public SchedulerHostContext {
 public:
  /**
   * @brief Publishes whether a Metal GPU is currently available.
   * @param available New capability value.
   * @return Nothing.
   * @throws Nothing.
   */
  void set_gpu_available(bool available) noexcept {
    gpu_available_.store(available, std::memory_order_release);
  }

  /** @copydoc SchedulerHostContext::is_device_available */
  bool is_device_available(Device device) const noexcept override {
    return device == Device::CPU ||
           (device == Device::GPU_METAL &&
            gpu_available_.load(std::memory_order_acquire));
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

 private:
  /** @brief Acquire/release-published mutable Metal capability. */
  std::atomic<bool> gpu_available_{false};
};

/**
 * @brief Expects complete stopped-state rollback after lifecycle failure.
 * @param state Concrete scheduler snapshot captured after the exception.
 * @return Nothing.
 * @throws GoogleTest assertion failures only.
 */
void expect_empty_stopped_state(
    const SchedulerTransactionalStateSnapshot& state) {
  EXPECT_FALSE(state.running);
  EXPECT_FALSE(state.worker_loop_active);
  EXPECT_EQ(state.worker_threads, 0U);
  EXPECT_EQ(state.local_queues, 0U);
  EXPECT_EQ(state.queued_tasks, 0U);
  EXPECT_EQ(state.ready_tasks, 0);
  EXPECT_EQ(state.tasks_to_complete, 0);
  EXPECT_EQ(state.in_flight_tasks, 0U);
  EXPECT_FALSE(state.exception_claimed);
  EXPECT_FALSE(state.has_exception);
  EXPECT_FALSE(state.has_exception_ptr);
}

/**
 * @brief Proves direct invalid constructors reject before start-time hooks.
 * @return Nothing.
 * @throws Nothing when every oversized direct configuration is invalid.
 */
TEST(SchedulerInstanceLimits,
     DirectConstructorsRejectOversizedRequestsBeforeLifecycleResources) {
  LifecycleFailureProbe cpu_probe;
  const SchedulerFailureInjectionHook cpu_hook{&cpu_probe,
                                               observe_or_fail_lifecycle};
  {
    ScopedFailureHook guard(set_cpu_scheduler_failure_injection_hook,
                            &cpu_hook);
    EXPECT_THROW(
        (void)CpuWorkStealingScheduler(kSchedulerWorkerRequestMax + 1U),
        std::invalid_argument);
  }
  EXPECT_EQ(cpu_probe.total_attempts.load(std::memory_order_relaxed), 0U);

  LifecycleFailureProbe gpu_probe;
  const SchedulerFailureInjectionHook gpu_hook{&gpu_probe,
                                               observe_or_fail_lifecycle};
  {
    ScopedFailureHook guard(set_gpu_scheduler_failure_injection_hook,
                            &gpu_hook);
    GpuPipelineScheduler::Config excessive_cpu;
    excessive_cpu.cpu_workers = kSchedulerWorkerRequestMax + 1U;
    excessive_cpu.gpu_workers = 0U;
    EXPECT_THROW((void)GpuPipelineScheduler(excessive_cpu),
                 std::invalid_argument);

    GpuPipelineScheduler::Config excessive_gpu;
    excessive_gpu.cpu_workers = kSchedulerWorkerRequestMax;
    excessive_gpu.gpu_workers = 2U;
    EXPECT_THROW((void)GpuPipelineScheduler(excessive_gpu),
                 std::invalid_argument);
  }
  EXPECT_EQ(gpu_probe.total_attempts.load(std::memory_order_relaxed), 0U);

  EXPECT_NO_THROW((void)CpuWorkStealingScheduler(kSchedulerWorkerRequestMax));
  GpuPipelineScheduler::Config exact_gpu_limit;
  exact_gpu_limit.cpu_workers = kSchedulerWorkerRequestMax;
  exact_gpu_limit.gpu_workers = 1U;
  EXPECT_NO_THROW((void)GpuPipelineScheduler(exact_gpu_limit));
}

/**
 * @brief Proves exact direct limits create only the admitted worker maxima.
 * @return Nothing.
 * @throws Nothing when CPU publishes eight and GPU publishes eight plus one.
 */
TEST(SchedulerInstanceLimits, DirectExactLimitsStartAtPublishedCeilings) {
  CpuWorkStealingScheduler cpu_scheduler(kSchedulerWorkerRequestMax);
  cpu_scheduler.start();
  EXPECT_EQ(cpu_scheduler_transactional_snapshot(&cpu_scheduler).worker_threads,
            kSchedulerWorkerRequestMax);
  cpu_scheduler.shutdown();

  ScopedForcedGpuRoute force_gpu;
  GpuPipelineScheduler::Config config;
  config.cpu_workers = kSchedulerWorkerRequestMax;
  config.gpu_workers = 1U;
  GpuPipelineScheduler gpu_scheduler(config);
  gpu_scheduler.start();
  EXPECT_EQ(gpu_scheduler_transactional_snapshot(&gpu_scheduler).worker_threads,
            kGpuSchedulerWorkerInstanceMax);
  gpu_scheduler.shutdown();
}

/**
 * @brief Proves direct automatic requests resolve to one-through-eight workers.
 * @return Nothing.
 * @throws Nothing when both concrete built-ins start within the public ceiling.
 */
TEST(SchedulerInstanceLimits, DirectAutomaticCpuGrantsStayWithinCeiling) {
  CpuWorkStealingScheduler cpu_scheduler(0U);
  cpu_scheduler.start();
  const auto cpu_state = cpu_scheduler_transactional_snapshot(&cpu_scheduler);
  EXPECT_GE(cpu_state.worker_threads, 1U);
  EXPECT_LE(cpu_state.worker_threads, kSchedulerWorkerRequestMax);
  cpu_scheduler.shutdown();

  GpuPipelineScheduler::Config gpu_config;
  gpu_config.cpu_workers = 0U;
  gpu_config.gpu_workers = 0U;
  GpuPipelineScheduler gpu_scheduler(gpu_config);
  gpu_scheduler.start();
  const auto gpu_state = gpu_scheduler_transactional_snapshot(&gpu_scheduler);
  EXPECT_GE(gpu_state.worker_threads, 1U);
  EXPECT_LE(gpu_state.worker_threads, kSchedulerWorkerRequestMax);
  gpu_scheduler.shutdown();
}

/**
 * @brief Proves late GPU availability adds exactly one configured worker.
 * @return Nothing.
 * @throws Nothing when repeated late attachment cannot grow worker ownership.
 */
TEST(SchedulerInstanceLimits, DirectLateGpuStartCannotExceedInstanceCeiling) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 1U;
  config.gpu_workers = 1U;

  MutableGpuHostContext attach_host;
  GpuPipelineScheduler attach_scheduler(config);
  attach_scheduler.attach(attach_host);
  attach_scheduler.start();
  EXPECT_EQ(
      gpu_scheduler_transactional_snapshot(&attach_scheduler).worker_threads,
      1U);
  attach_host.set_gpu_available(true);
  attach_scheduler.attach(attach_host);
  EXPECT_EQ(
      gpu_scheduler_transactional_snapshot(&attach_scheduler).worker_threads,
      2U);
  attach_scheduler.attach(attach_host);
  EXPECT_EQ(
      gpu_scheduler_transactional_snapshot(&attach_scheduler).worker_threads,
      2U);
  attach_scheduler.shutdown();
  attach_scheduler.detach();

  MutableGpuHostContext restart_host;
  GpuPipelineScheduler restart_scheduler(config);
  restart_scheduler.attach(restart_host);
  restart_scheduler.start();
  EXPECT_EQ(
      gpu_scheduler_transactional_snapshot(&restart_scheduler).worker_threads,
      1U);
  restart_host.set_gpu_available(true);
  restart_scheduler.start();
  EXPECT_EQ(
      gpu_scheduler_transactional_snapshot(&restart_scheduler).worker_threads,
      2U);
  restart_scheduler.start();
  EXPECT_EQ(
      gpu_scheduler_transactional_snapshot(&restart_scheduler).worker_threads,
      2U);
  restart_scheduler.shutdown();
  restart_scheduler.detach();
}

/**
 * @brief Proves GPU thread-creation loss joins CPU workers and supports retry.
 * @return Nothing.
 * @throws Nothing when the exact injected system error leaves empty state.
 */
TEST(SchedulerInstanceLimits,
     DirectGpuThreadCreationFailureRollsBackWithinConfiguredCeiling) {
  ScopedForcedGpuRoute force_gpu;
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 2U;
  config.gpu_workers = 1U;
  GpuPipelineScheduler scheduler(config);

  LifecycleFailureProbe probe;
  probe.selected_point = SchedulerFailurePoint::GpuThreadCreate;
  probe.fail_on_attempt = 1U;
  const SchedulerFailureInjectionHook hook{&probe, observe_or_fail_lifecycle};
  {
    ScopedFailureHook guard(set_gpu_scheduler_failure_injection_hook, &hook);
    EXPECT_THROW(scheduler.start(), std::system_error);
  }
  expect_empty_stopped_state(gpu_scheduler_transactional_snapshot(&scheduler));

  scheduler.start();
  const auto retried = gpu_scheduler_transactional_snapshot(&scheduler);
  EXPECT_TRUE(retried.running);
  EXPECT_EQ(retried.worker_threads, 3U);
  scheduler.shutdown();
  expect_empty_stopped_state(gpu_scheduler_transactional_snapshot(&scheduler));
}

/**
 * @brief Proves late GPU creation failure cannot release admitted capacity.
 * @return Nothing.
 * @throws Nothing when retry succeeds and owner destruction recovers capacity.
 * @note Constructor-ceiling rules are covered above through concrete classes;
 * this separate test intentionally exercises reservation lifetime composition.
 */
TEST(SchedulerInstanceLimits,
     LateGpuThreadFailureRetainsReservationUntilOwnerDestruction) {
  const auto plan = SchedulerFactory::plan_for_hardware("gpu_pipeline", 1U, 1U);
  ASSERT_TRUE(plan.has_value());
  ASSERT_EQ(plan->reservation_slots(), 2U);
  SchedulerWorkerBudget budget(2U);
  auto reservation = budget.try_reserve(plan->reservation_slots());
  ASSERT_TRUE(reservation.has_value());
  auto scheduler = SchedulerFactory::create(*plan, std::move(*reservation));
  ASSERT_NE(scheduler, nullptr);

  MutableGpuHostContext host;
  scheduler->attach(host);
  scheduler->start();
  ASSERT_TRUE(scheduler->is_running());

  LifecycleFailureProbe probe;
  probe.selected_point = SchedulerFailurePoint::GpuThreadCreate;
  probe.fail_on_attempt = 1U;
  const SchedulerFailureInjectionHook hook{&probe, observe_or_fail_lifecycle};
  host.set_gpu_available(true);
  {
    ScopedFailureHook guard(set_gpu_scheduler_failure_injection_hook, &hook);
    EXPECT_THROW(scheduler->attach(host), std::system_error);
  }
  EXPECT_FALSE(scheduler->is_running());
  EXPECT_FALSE(budget.try_reserve(1U).has_value());
  EXPECT_FALSE(budget.try_reserve(2U).has_value());

  scheduler->start();
  EXPECT_TRUE(scheduler->is_running());
  scheduler->shutdown();
  scheduler->detach();
  scheduler.reset();
  EXPECT_TRUE(budget.try_reserve(2U).has_value());
}

/**
 * @brief Proves CPU thread-creation rollback retains the complete reservation.
 * @return Nothing.
 * @throws Nothing when retry succeeds and destruction restores both slots.
 * @note Direct constructor validation remains independently covered without
 * this reservation wrapper.
 */
TEST(SchedulerInstanceLimits,
     CpuThreadFailureRetainsReservationUntilOwnerDestruction) {
  const auto plan =
      SchedulerFactory::plan_for_hardware("cpu_work_stealing", 2U, 2U);
  ASSERT_TRUE(plan.has_value());
  ASSERT_EQ(plan->reservation_slots(), 2U);
  SchedulerWorkerBudget budget(2U);
  auto reservation = budget.try_reserve(plan->reservation_slots());
  ASSERT_TRUE(reservation.has_value());
  auto scheduler = SchedulerFactory::create(*plan, std::move(*reservation));
  ASSERT_NE(scheduler, nullptr);

  MutableGpuHostContext host;
  scheduler->attach(host);
  LifecycleFailureProbe probe;
  probe.selected_point = SchedulerFailurePoint::CpuThreadCreate;
  probe.fail_on_attempt = 2U;
  const SchedulerFailureInjectionHook hook{&probe, observe_or_fail_lifecycle};
  {
    ScopedFailureHook guard(set_cpu_scheduler_failure_injection_hook, &hook);
    EXPECT_THROW(scheduler->start(), std::system_error);
  }
  EXPECT_FALSE(scheduler->is_running());
  EXPECT_FALSE(budget.try_reserve(1U).has_value());
  EXPECT_FALSE(budget.try_reserve(2U).has_value());

  scheduler->start();
  EXPECT_TRUE(scheduler->is_running());
  scheduler->shutdown();
  scheduler->detach();
  scheduler.reset();
  EXPECT_TRUE(budget.try_reserve(2U).has_value());
}

}  // namespace
}  // namespace ps::testing
