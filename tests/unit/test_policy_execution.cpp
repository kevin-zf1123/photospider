#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "compute/compute_run.hpp"
#include "compute/execution/execution_service.hpp"
#include "execution/execution_task_runtime.hpp"
#include "photospider/host/host.hpp"
#include "policy/policy_registry.hpp"
#include "support/execution_service_test_access.hpp"
#include "support/fake_device_executor.hpp"
#include "support/policy_fixture_controller.hpp"

#ifndef PS_TEST_POLICY_PLUGIN_PATH
#error "PS_TEST_POLICY_PLUGIN_PATH must identify the compatible policy fixture"
#endif

namespace ps::compute {
namespace {

using ps::test::PolicyFixtureController;

/** @brief Maximum wait for a controlled callback or Run settlement. */
constexpr std::chrono::seconds kTestTimeout{5};

/**
 * @brief Builds one deterministic Throughput or Interactive Run submission.
 * @param label Stable Graph policy key.
 * @param revision Nonzero strong Graph identity and revision value.
 * @param target_node_id Graph-local trace target.
 * @param service_class Explicit policy class.
 * @return Complete valid Run submission with two-way parallelism allowed.
 * @throws std::bad_alloc when `label` ownership cannot allocate.
 */
ComputeRunSubmission make_submission(
    std::string label, std::uint64_t revision, int target_node_id,
    ComputeRunQosClass service_class = ComputeRunQosClass::Throughput) {
  return ComputeRunSubmission{
      std::move(label),
      GraphInstanceId{revision},
      GraphRevision{revision},
      target_node_id,
      ComputeIntent::GlobalHighPrecision,
      ComputeRunQuality::Full,
      ComputeRunQos{service_class, std::nullopt, 1U, 2U},
      SupersessionIdentity{
          SupersessionKey(target_node_id, ComputeIntent::GlobalHighPrecision),
          SupersessionGeneration(1U)},
      nullptr};
}

/**
 * @brief Minimal thread-safe Host observation context for route integration.
 * @throws std::bad_alloc only when copied worker-id observations grow.
 * @note DeviceBackend inventory belongs to the injected execution service. This
 * object observes callback attribution only.
 */
class TestHostContext final : public ExecutionHostContext {
 public:
  /** @copydoc ExecutionHostContext::set_task_context */
  void set_task_context(int worker_id, std::uint64_t epoch,
                        std::optional<ExecutionTaskAuditIdentity>
                            task_identity) noexcept override {
    (void)epoch;
    (void)task_identity;
    const int active =
        active_contexts_.fetch_add(1, std::memory_order_acq_rel) + 1;
    int observed = maximum_contexts_.load(std::memory_order_relaxed);
    while (observed < active && !maximum_contexts_.compare_exchange_weak(
                                    observed, active, std::memory_order_relaxed,
                                    std::memory_order_relaxed)) {
    }
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      worker_ids_.push_back(worker_id);
    } catch (...) {
      observation_failed_.store(true, std::memory_order_relaxed);
    }
  }

  /** @copydoc ExecutionHostContext::clear_task_context */
  void clear_task_context() noexcept override {
    active_contexts_.fetch_sub(1, std::memory_order_acq_rel);
    exits_.fetch_add(1, std::memory_order_relaxed);
  }

  /** @copydoc ExecutionHostContext::log_event */
  void log_event(ExecutionTraceAction, int, int, std::uint64_t,
                 std::optional<ExecutionTaskAuditIdentity>) noexcept override {}

  /**
   * @brief Copies all worker ids observed at callback entry.
   * @return Worker ids in observation order.
   * @throws std::bad_alloc when snapshot storage cannot allocate.
   * @throws std::system_error when locking fails.
   */
  std::vector<int> worker_ids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return worker_ids_;
  }

  /**
   * @brief Returns the largest simultaneous callback-context count.
   * @return Maximum concurrent entered contexts.
   * @throws Nothing.
   */
  int maximum_contexts() const noexcept {
    return maximum_contexts_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Returns balanced callback-context exits.
   * @return Number of clear-task-context calls.
   * @throws Nothing.
   */
  int exits() const noexcept { return exits_.load(std::memory_order_relaxed); }

  /**
   * @brief Reports whether a noexcept observation lost worker data.
   * @return True when worker-id storage failed during a callback.
   * @throws Nothing.
   */
  bool observation_failed() const noexcept {
    return observation_failed_.load(std::memory_order_relaxed);
  }

 private:
  /** @brief Current entered callback contexts. */
  std::atomic_int active_contexts_{0};

  /** @brief Maximum concurrently entered callback contexts. */
  std::atomic_int maximum_contexts_{0};

  /** @brief Number of balanced callback-context exits. */
  std::atomic_int exits_{0};

  /** @brief Serializes worker-id observations. */
  mutable std::mutex mutex_;

  /** @brief Worker ids captured at context entry. */
  std::vector<int> worker_ids_;

  /** @brief Whether noexcept observation failed. */
  std::atomic_bool observation_failed_{false};
};

/**
 * @brief Records one Run's committed service-start evidence without blocking.
 *
 * @throws Nothing for construction, callbacks, or scalar inspection.
 * @note Tests attach this sink only to single-task Runs. Release/acquire
 * publication makes the three observation fields visible before the count.
 */
class ServiceStartObservationSink final : public ComputeRunObservationSink {
 public:
  /** @copydoc ComputeRunObservationSink::reserve_causal_coordinate */
  ComputeRunObservationCoordinate reserve_causal_coordinate() noexcept
      override {
    return ComputeRunObservationCoordinate{
        std::chrono::steady_clock::now(),
        next_sequence_.fetch_add(1U, std::memory_order_relaxed)};
  }

  /** @copydoc ComputeRunObservationSink::on_current_generation */
  void on_current_generation(
      const SupersessionIdentity& identity,
      ComputeRunObservationCoordinate coordinate) noexcept override {
    (void)identity;
    (void)coordinate;
  }

  /** @copydoc ComputeRunObservationSink::on_service_start */
  void on_service_start(
      const ComputeRunDescriptor& descriptor,
      ComputeRunTaskIdentity task_identity, std::uint64_t service_charge,
      const ComputeRunServiceStartObservation& observation,
      ComputeRunObservationCoordinate coordinate) noexcept override {
    (void)descriptor;
    (void)task_identity;
    (void)service_charge;
    (void)coordinate;
    interactive_startable_.store(observation.interactive_candidate_startable,
                                 std::memory_order_relaxed);
    throughput_startable_.store(observation.throughput_candidate_startable,
                                std::memory_order_relaxed);
    execution_grant_committed_.store(observation.execution_grant_committed,
                                     std::memory_order_relaxed);
    service_start_count_.fetch_add(1U, std::memory_order_release);
  }

  /** @copydoc ComputeRunObservationSink::on_cancellation */
  void on_cancellation(
      const ComputeRunDescriptor& descriptor,
      ComputeRunCancellationReason reason,
      ComputeRunObservationCoordinate coordinate) noexcept override {
    (void)descriptor;
    (void)reason;
    (void)coordinate;
  }

  /** @copydoc ComputeRunObservationSink::on_terminal */
  void on_terminal(
      const ComputeRunDescriptor& descriptor, ComputeRunTerminalKind kind,
      ComputeRunObservationCoordinate coordinate) noexcept override {
    (void)descriptor;
    (void)kind;
    (void)coordinate;
  }

  /** @copydoc ComputeRunObservationSink::on_current_visible */
  void on_current_visible(
      const ComputeRunDescriptor& descriptor, Value output,
      ComputeRunObservationCoordinate coordinate) noexcept override {
    (void)descriptor;
    (void)output;
    (void)coordinate;
  }

  /** @copydoc ComputeRunObservationSink::on_run_quiescent */
  void on_run_quiescent(
      const ComputeRunDescriptor& descriptor,
      ComputeRunObservationCoordinate coordinate) noexcept override {
    (void)descriptor;
    (void)coordinate;
  }

  /** @copydoc ComputeRunObservationSink::on_run_resource_settled */
  void on_run_resource_settled(
      const ComputeRunDescriptor& descriptor,
      ComputeRunObservationCoordinate coordinate) noexcept override {
    (void)descriptor;
    (void)coordinate;
  }

  /** @copydoc ComputeRunObservationSink::on_host_settled */
  void on_host_settled(
      ComputeRunObservationCoordinate coordinate) noexcept override {
    (void)coordinate;
  }

  /**
   * @brief Returns the number of committed service starts observed so far.
   * @return Exact callback count published with acquire ordering.
   * @throws Nothing.
   */
  std::uint32_t service_start_count() const noexcept {
    return service_start_count_.load(std::memory_order_acquire);
  }

  /**
   * @brief Copies the latest committed service-start applicability facts.
   * @return Immutable three-field observation.
   * @throws Nothing.
   * @note Callers first verify `service_start_count()` is nonzero so its
   * acquire pairs with callback publication.
   */
  ComputeRunServiceStartObservation service_start_observation() const noexcept {
    return ComputeRunServiceStartObservation{
        interactive_startable_.load(std::memory_order_relaxed),
        throughput_startable_.load(std::memory_order_relaxed),
        execution_grant_committed_.load(std::memory_order_relaxed)};
  }

 private:
  /** @brief Nonzero observer-local causal sequence source. */
  std::atomic_uint64_t next_sequence_{1U};

  /** @brief Latest Interactive evidence-startable fact. */
  std::atomic_bool interactive_startable_{false};

  /** @brief Latest Throughput evidence-startable fact. */
  std::atomic_bool throughput_startable_{false};

  /** @brief Latest selected-start grant-commit fact. */
  std::atomic_bool execution_grant_committed_{false};

  /** @brief Release-published number of service-start callbacks. */
  std::atomic_uint32_t service_start_count_{0U};
};

/**
 * @brief Builds one ready submission whose callback owns completion release.
 * @param lease Strong matching Run lease transferred into the submission.
 * @param local_task_id Dense Run-local task id.
 * @param trace_node_id Diagnostic trace node.
 * @param executable Test callback entered by a physical route.
 * @param device Private lane selected for the callback.
 * @param priority Ready-order hint carried by the submission.
 * @param is_initial_ready Whether publication belongs to the initial batch.
 * @return Move-owned ready submission.
 * @throws Standard callback, metadata, or function ownership exceptions.
 */
ReadyTaskSubmission make_ready(
    ComputeRunLease lease, std::uint64_t local_task_id, int trace_node_id,
    ReadyTaskSubmission::Executable executable,
    DeviceBackend device = DeviceBackend::CPU,
    ExecutionTaskPriority priority = ExecutionTaskPriority::Normal,
    bool is_initial_ready = true) {
  const ComputeRunTaskIdentity identity = lease.task_identity(local_task_id);
  return ReadyTaskSubmission(std::move(lease), identity, trace_node_id,
                             is_initial_ready, std::move(executable), priority,
                             ReadyTaskSubmission::default_resource_demand(),
                             device);
}

/**
 * @brief Waits until the policy fixture observed a minimum selection count.
 * @param control Thread-safe fixture counter source.
 * @param minimum_count Inclusive target count.
 * @return True when the target was reached before `kTestTimeout`.
 * @throws Nothing.
 * @note Polling observes only test instrumentation; production worker waits
 * remain driven by condition-variable notifications and bounded backoff.
 */
bool wait_for_select_count(const PolicyFixtureController& control,
                           std::uint64_t minimum_count) noexcept {
  const auto deadline = std::chrono::steady_clock::now() + kTestTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (control.select_count() >= minimum_count) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return control.select_count() >= minimum_count;
}

/**
 * @brief Waits for one exact class-partitioned ready-store state.
 * @param service Execution domain exposing source-private diagnostics.
 * @param interactive_entries Expected Interactive entry count.
 * @param throughput_entries Expected Throughput entry count.
 * @return True when both counts and their total match before timeout.
 * @throws std::system_error when service synchronization fails.
 * @note The helper polls only immutable diagnostics while the deliberately
 * blocked GPU callback prevents either queued candidate from starting.
 */
bool wait_for_ready_entries(ExecutionService& service,
                            std::uint64_t interactive_entries,
                            std::uint64_t throughput_entries) {
  const auto deadline = std::chrono::steady_clock::now() + kTestTimeout;
  do {
    const ExecutionReadyClassSnapshot snapshot = service.ready_class_snapshot();
    if (snapshot.valid && snapshot.interactive_entries == interactive_entries &&
        snapshot.throughput_entries == throughput_entries &&
        snapshot.total_entries == interactive_entries + throughput_entries) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < deadline);
  const ExecutionReadyClassSnapshot snapshot = service.ready_class_snapshot();
  return snapshot.valid &&
         snapshot.interactive_entries == interactive_entries &&
         snapshot.throughput_entries == throughput_entries &&
         snapshot.total_entries == interactive_entries + throughput_entries;
}

/**
 * @brief Guarantees one callback gate opens before asynchronous cleanup.
 *
 * @throws Nothing for construction and destruction; explicit release may
 * propagate `std::future_error` from a violated test invariant.
 * @note The guard is intentionally declared after the owning async future so
 * reverse destruction opens the gate before that future waits for completion.
 */
class ScopedPromiseRelease final {
 public:
  /**
   * @brief Borrows one promise owned by the enclosing test.
   * @param promise Single-use callback release promise.
   * @throws Nothing.
   */
  explicit ScopedPromiseRelease(std::promise<void>& promise) noexcept
      : promise_(&promise) {}

  /** @brief Opens an unreleased gate while suppressing cleanup failures. */
  ~ScopedPromiseRelease() noexcept {
    try {
      release();
    } catch (...) {
    }
  }

  /** @brief Prevents duplicate promise ownership. */
  ScopedPromiseRelease(const ScopedPromiseRelease&) = delete;

  /** @brief Prevents assigning duplicate promise ownership. */
  ScopedPromiseRelease& operator=(const ScopedPromiseRelease&) = delete;

  /**
   * @brief Opens the callback gate exactly once.
   * @return Nothing.
   * @throws std::future_error when the borrowed promise state is invalid.
   */
  void release() {
    if (released_) {
      return;
    }
    promise_->set_value();
    released_ = true;
  }

 private:
  /** @brief Borrowed single-use promise. */
  std::promise<void>* promise_;

  /** @brief Whether this guard already published the release. */
  bool released_ = false;
};

/**
 * @brief Executes one successful single-task Run on an exact route.
 * @param service Configured execution domain.
 * @param route Exact private physical route.
 * @param label Stable Graph identity.
 * @param revision Nonzero Graph revision.
 * @param entered Callback entry counter.
 * @param host Host observation destination.
 * @param device Private lane selected for the task.
 * @return Nothing after synchronous settlement.
 * @throws Any service or callback exception unchanged.
 */
void execute_successful_run(ExecutionService& service, const std::string& route,
                            const std::string& label, std::uint64_t revision,
                            std::atomic_int& entered, TestHostContext& host,
                            DeviceBackend device = DeviceBackend::CPU) {
  ComputeRun run(make_submission(label, revision, 1));
  std::vector<ReadyTaskSubmission> ready;
  ready.push_back(make_ready(
      run.acquire_lease(), 0U, 1,
      [&entered](ComputeRunLease&, const ComputeRunTaskIdentity&,
                 ExecutionTaskRuntime& runtime) {
        entered.fetch_add(1, std::memory_order_relaxed);
        runtime.dec_tasks_to_complete();
      },
      device));
  service.execute_run(host, route, std::move(ready), 1);
}

/**
 * @brief Coordinates repeated policy-hook entry with a controlling test.
 * @throws Nothing for construction; synchronization may throw system errors.
 */
struct CyclingHookState final {
  /** @brief Serializes entry and release generations. */
  std::mutex mutex;

  /** @brief Publishes callback entry and replacement release. */
  std::condition_variable condition;

  /** @brief Number of selection callbacks currently entered in sequence. */
  unsigned int entries = 0U;

  /** @brief Highest callback entry released by the controlling test. */
  unsigned int released = 0U;

  /**
   * @brief Waits until the requested callback entry is visible.
   * @param expected One-based callback entry number.
   * @return True before timeout, otherwise false.
   * @throws std::system_error when synchronization fails.
   */
  bool wait_for_entry(unsigned int expected) {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, kTestTimeout,
                              [this, expected] { return entries >= expected; });
  }

  /**
   * @brief Releases one exact callback entry.
   * @param generation One-based entry generation to release.
   * @return Nothing.
   * @throws std::system_error when synchronization fails.
   */
  void release(unsigned int generation) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      released = std::max(released, generation);
    }
    condition.notify_all();
  }

  /**
   * @brief Releases every current or future controlled entry during cleanup.
   * @return Nothing.
   * @throws Nothing; synchronization failure is suppressed for cleanup.
   */
  void release_all() noexcept {
    try {
      {
        std::lock_guard<std::mutex> lock(mutex);
        released = std::numeric_limits<unsigned int>::max();
      }
      condition.notify_all();
    } catch (...) {
    }
  }
};

/**
 * @brief Blocks each controlled select until its replacement/cancel step ends.
 * @param context Nonnull `CyclingHookState` owned through callback return.
 * @param event Controlled fixture event; only selection blocks.
 * @return Zero, ignored by the fixture.
 * @throws Nothing.
 */
std::uint32_t PS_POLICY_CALL cycling_select_hook(
    void* context, ps_policy_fixture_hook_event event) noexcept {
  auto* state = static_cast<CyclingHookState*>(context);
  if (state == nullptr || event != PS_POLICY_FIXTURE_HOOK_SELECT) {
    return 0U;
  }
  try {
    std::unique_lock<std::mutex> lock(state->mutex);
    const unsigned int generation = ++state->entries;
    state->condition.notify_all();
    state->condition.wait(
        lock, [state, generation] { return state->released >= generation; });
  } catch (...) {
  }
  return 0U;
}

/**
 * @brief Records one shutdown attempt made from a fixture policy callback.
 *
 * @throws Nothing for construction and atomic observations.
 * @note `target` is borrowed and must outlive the installed fixture hook.
 */
struct PolicyShutdownHookState final {
  /** @brief Service selected as the callback's shutdown target. */
  ExecutionService* target = nullptr;

  /** @brief True when shutdown returned normally. */
  std::atomic_bool returned{false};

  /** @brief True when same-service shutdown was rejected as required. */
  std::atomic_bool logic_error{false};

  /** @brief True when any unexpected exception crossed the attempted call. */
  std::atomic_bool unexpected_error{false};
};

/**
 * @brief Attempts service shutdown from one entered fixture selection callback.
 * @param context Nonnull `PolicyShutdownHookState` retained by the test.
 * @param event Fixture callback phase; only selection attempts shutdown.
 * @return Zero so the fixture preserves its configured selection result.
 * @throws Nothing; every shutdown outcome is captured into atomic flags.
 */
std::uint32_t PS_POLICY_CALL policy_shutdown_hook(
    void* context, ps_policy_fixture_hook_event event) noexcept {
  auto* state = static_cast<PolicyShutdownHookState*>(context);
  if (state == nullptr || state->target == nullptr ||
      event != PS_POLICY_FIXTURE_HOOK_SELECT) {
    return 0U;
  }
  try {
    state->target->shutdown();
    state->returned.store(true, std::memory_order_relaxed);
  } catch (const std::logic_error&) {
    state->logic_error.store(true, std::memory_order_relaxed);
  } catch (...) {
    state->unexpected_error.store(true, std::memory_order_relaxed);
  }
  return 0U;
}

/**
 * @brief Tests whether all fifteen lifecycle counters are exactly zero.
 * @param counters Complete source-private counter snapshot.
 * @return True only when every registry and physical owner count is zero.
 * @throws Nothing.
 */
bool all_lifecycle_counters_zero(
    const ExecutionLifecycleCounters& counters) noexcept {
  return counters.registered_graph_count == 0U &&
         counters.open_graph_count == 0U &&
         counters.closing_graph_count == 0U &&
         counters.pending_candidate_count == 0U &&
         counters.admitted_standalone_run_count == 0U &&
         counters.admitted_run_group_count == 0U &&
         counters.admitted_child_run_count == 0U &&
         counters.terminal_not_quiescent_run_count == 0U &&
         counters.finalizing_run_count == 0U &&
         counters.ready_entry_count == 0U &&
         counters.entered_callback_count == 0U &&
         counters.live_root_reservation_count == 0U &&
         counters.live_child_grant_count == 0U &&
         counters.live_policy_invocation_count == 0U &&
         counters.live_policy_binding_count == 0U;
}

/**
 * @brief Guarantees a controlled selection hook cannot strand a test future.
 */
class ScopedHookRelease final {
 public:
  /**
   * @brief Borrows state that outlives this guard.
   * @param state Controlled hook state released during destruction.
   * @throws Nothing.
   */
  explicit ScopedHookRelease(CyclingHookState& state) noexcept
      : state_(&state) {}

  /**
   * @brief Releases every waiting hook during normal or exceptional cleanup.
   * @throws Nothing; cleanup suppresses synchronization failures.
   */
  ~ScopedHookRelease() noexcept { state_->release_all(); }

  /** @brief Prevents duplicate release ownership. */
  ScopedHookRelease(const ScopedHookRelease&) = delete;

  /** @brief Prevents assigning duplicate release ownership. */
  ScopedHookRelease& operator=(const ScopedHookRelease&) = delete;

 private:
  /** @brief Borrowed controlled hook state. */
  CyclingHookState* state_;
};

/**
 * @brief Disarms the separate test-product rollback probe on every exit.
 */
class ScopedReservedStartProbe final {
 public:
  /**
   * @brief Arms the probe for one isolated service.
   * @param service Service whose next reserved start is inspected.
   * @throws Nothing.
   */
  explicit ScopedReservedStartProbe(ExecutionService& service) noexcept
      : service_(&service) {
    ::ps::testing::ExecutionServiceTestAccess::
        arm_reserved_start_rollback_probe(service);
  }

  /** @brief Disarms the probe without invoking production callbacks. */
  ~ScopedReservedStartProbe() noexcept {
    ::ps::testing::ExecutionServiceTestAccess::
        disarm_reserved_start_rollback_probe(*service_);
  }

  /** @brief Prevents duplicate disarm ownership. */
  ScopedReservedStartProbe(const ScopedReservedStartProbe&) = delete;

  /** @brief Prevents duplicate disarm assignment. */
  ScopedReservedStartProbe& operator=(const ScopedReservedStartProbe&) = delete;

 private:
  /** @brief Borrowed isolated service used only for test ownership. */
  ExecutionService* service_;
};

/**
 * @brief Isolates process-registry policy fixture state for every test body.
 */
class PolicyExecutionFixture : public ::testing::Test {
 protected:
  /** @brief Loads a reset compatible fixture before each test. */
  void SetUp() override {
    (void)policy::PolicyRegistry::process_instance().unload_all_plugins();
    control_.reset();
    policy::PolicyRegistry::process_instance().load(PS_TEST_POLICY_PLUGIN_PATH);
  }

  /** @brief Clears hooks and removes fixture visibility after local services.
   */
  void TearDown() override {
    control_.set_hook(nullptr, nullptr);
    (void)policy::PolicyRegistry::process_instance().unload_all_plugins();
  }

  /** @brief Native control handle kept through every fixture callback. */
  PolicyFixtureController control_{PS_TEST_POLICY_PLUGIN_PATH};
};

/**
 * @brief Verifies worker self-shutdown rejects before mutation and finalizes.
 *
 * @throws ExecutionService or telemetry failures unchanged outside the worker
 * callback; the callback records unexpected outcomes without throwing.
 * @note One configured CPU worker plus the dedicated Metal worker must join
 * before both built-in bindings retire and the sole ServiceStopped event
 * freezes all fifteen counters at zero.
 */
TEST(ExecutionServiceShutdown,
     WorkerSelfShutdownRejectsAndRepeatedControlShutdownIsExact) {
  ExecutionService service(1U);
  TestHostContext host;
  ComputeRun run(make_submission("worker-self-shutdown", 80U, 1));
  std::atomic_bool rejected{false};
  std::atomic_bool unexpected{false};
  std::vector<ReadyTaskSubmission> ready;
  ready.push_back(make_ready(
      run.acquire_lease(), 0U, 1,
      [&service, &rejected, &unexpected](ComputeRunLease&,
                                         const ComputeRunTaskIdentity&,
                                         ExecutionTaskRuntime& runtime) {
        try {
          service.shutdown();
          unexpected.store(true, std::memory_order_relaxed);
        } catch (const std::logic_error&) {
          rejected.store(true, std::memory_order_relaxed);
        } catch (...) {
          unexpected.store(true, std::memory_order_relaxed);
        }
        runtime.dec_tasks_to_complete();
      }));

  service.execute_run(host, "cpu", std::move(ready), 1);
  EXPECT_TRUE(rejected.load(std::memory_order_relaxed));
  EXPECT_FALSE(unexpected.load(std::memory_order_relaxed));
  EXPECT_EQ(service.lifecycle_snapshot(0U, 64U).service_state,
            ExecutionLifecycleServiceState::Accepting);

  const std::uint64_t shutdown_generation = service.begin_shutdown();
  ASSERT_NE(shutdown_generation, 0U);
  service.shutdown();
  service.shutdown();
  EXPECT_EQ(service.begin_shutdown(), shutdown_generation);

  const ExecutionLifecyclePage page = service.lifecycle_snapshot(0U, 64U);
  EXPECT_EQ(page.service_state, ExecutionLifecycleServiceState::Stopped);
  EXPECT_EQ(page.shutdown_generation, shutdown_generation);
  EXPECT_TRUE(all_lifecycle_counters_zero(page.counters));

  std::size_t worker_joined_count = 0U;
  std::size_t binding_retired_count = 0U;
  std::size_t service_stopped_count = 0U;
  std::uint64_t last_worker_sequence = 0U;
  std::uint64_t first_binding_sequence =
      std::numeric_limits<std::uint64_t>::max();
  std::uint64_t last_binding_sequence = 0U;
  std::uint64_t service_stopped_sequence = 0U;
  for (const ExecutionLifecycleEvent& event : page.records) {
    switch (event.kind) {
      case ExecutionLifecycleEventKind::WorkerJoined:
        ++worker_joined_count;
        last_worker_sequence = event.sequence;
        break;
      case ExecutionLifecycleEventKind::BindingRetired:
        ++binding_retired_count;
        first_binding_sequence =
            std::min(first_binding_sequence, event.sequence);
        last_binding_sequence = event.sequence;
        break;
      case ExecutionLifecycleEventKind::ServiceStopped:
        ++service_stopped_count;
        service_stopped_sequence = event.sequence;
        EXPECT_TRUE(all_lifecycle_counters_zero(event.counters));
        break;
      default:
        break;
    }
  }
  EXPECT_EQ(worker_joined_count, 2U);
  EXPECT_EQ(binding_retired_count, 2U);
  EXPECT_EQ(service_stopped_count, 1U);
  EXPECT_LT(last_worker_sequence, first_binding_sequence);
  EXPECT_LT(last_binding_sequence, service_stopped_sequence);
}

/**
 * @brief Verifies a policy callback cannot shut down its owning service.
 *
 * @throws Fixture loading or execution failures unchanged.
 * @note The same-service identity check occurs before admission changes, so
 * the Run completes and telemetry remains Accepting until control-thread
 * shutdown.
 */
#if defined(__linux__)
// These cases exercise a signed native policy callback; Darwin is fail-closed.
TEST_F(PolicyExecutionFixture,
       SameServicePolicyCallbackRejectsShutdownBeforeMutation) {
  ExecutionService service(1U);
  service.replace_policy(PolicyClass::Throughput, "fixture_policy");
  PolicyShutdownHookState hook{&service};
  control_.set_hook(&policy_shutdown_hook, &hook);
  control_.set_select_mode(PS_POLICY_FIXTURE_SELECT_HOOK_LAST);

  std::atomic_int entered{0};
  TestHostContext host;
  execute_successful_run(service, "cpu", "policy-self-shutdown", 81U, entered,
                         host);
  EXPECT_TRUE(hook.logic_error.load(std::memory_order_relaxed));
  EXPECT_FALSE(hook.returned.load(std::memory_order_relaxed));
  EXPECT_FALSE(hook.unexpected_error.load(std::memory_order_relaxed));
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(service.lifecycle_snapshot(0U, 64U).service_state,
            ExecutionLifecycleServiceState::Accepting);

  control_.set_hook(nullptr, nullptr);
  service.shutdown();
}

/**
 * @brief Verifies service identity does not over-reject cross-service control.
 *
 * @throws Fixture loading or execution failures unchanged.
 * @note A policy callback entered by one service may synchronously shut down a
 * distinct idle service; only the callback-owning service remains protected.
 */
TEST_F(PolicyExecutionFixture,
       PolicyCallbackMayShutdownADifferentExecutionService) {
  ExecutionService callback_owner(1U);
  ExecutionService shutdown_target(1U);
  callback_owner.replace_policy(PolicyClass::Throughput, "fixture_policy");
  PolicyShutdownHookState hook{&shutdown_target};
  control_.set_hook(&policy_shutdown_hook, &hook);
  control_.set_select_mode(PS_POLICY_FIXTURE_SELECT_HOOK_LAST);

  std::atomic_int entered{0};
  TestHostContext host;
  execute_successful_run(callback_owner, "cpu", "cross-service-shutdown", 82U,
                         entered, host);
  EXPECT_TRUE(hook.returned.load(std::memory_order_relaxed));
  EXPECT_FALSE(hook.logic_error.load(std::memory_order_relaxed));
  EXPECT_FALSE(hook.unexpected_error.load(std::memory_order_relaxed));
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(shutdown_target.lifecycle_snapshot(0U, 64U).service_state,
            ExecutionLifecycleServiceState::Stopped);
  EXPECT_EQ(callback_owner.lifecycle_snapshot(0U, 64U).service_state,
            ExecutionLifecycleServiceState::Accepting);

  control_.set_hook(nullptr, nullptr);
  callback_owner.shutdown();
}
#endif

/**
 * @brief Verifies route-aware device discovery has deterministic fallback.
 */
TEST(PhysicalExecutionIntegration, PublishesRouteAwareDeviceInventory) {
  execution::DeviceExecutorRegistry empty_registry;
  ExecutionService cpu_service(ExecutionService::default_resource_limits(),
                               std::move(empty_registry));
  cpu_service.configure_worker_count(2U);
  ExecutionService metal_service(
      ExecutionService::default_resource_limits(),
      ::ps::testing::make_fake_metal_executor_registry());
  metal_service.configure_worker_count(2U);

  EXPECT_EQ(cpu_service.available_devices("cpu"),
            (std::vector<DeviceBackend>{DeviceBackend::CPU}));
  EXPECT_EQ(metal_service.available_devices("serial_debug"),
            (std::vector<DeviceBackend>{DeviceBackend::CPU}));
  EXPECT_EQ(cpu_service.available_devices("gpu_pipeline"),
            (std::vector<DeviceBackend>{DeviceBackend::CPU}));
  EXPECT_EQ(
      metal_service.available_devices("gpu_pipeline"),
      (std::vector<DeviceBackend>{DeviceBackend::Metal, DeviceBackend::CPU}));
  EXPECT_THROW(metal_service.available_devices("heterogeneous"), GraphError);
}

/**
 * @brief Verifies an explicitly empty registry creates no phantom account.
 *
 * @throws ExecutionService construction or diagnostic failures unchanged.
 * @note Default candidate limits include Metal, so this regression fails if
 * ledger construction ignores the fixed empty registry.
 */
TEST(PhysicalExecutionIntegration, EmptyRegistryCreatesNoMetalAccount) {
  execution::DeviceExecutorRegistry empty_registry;
  ExecutionService service(ExecutionService::default_resource_limits(),
                           std::move(empty_registry));

  EXPECT_FALSE(service.has_device_executor(DeviceBackend::Metal));
  EXPECT_FALSE(service.device_resource_snapshot(DeviceId(DeviceBackend::Metal))
                   .has_value());
  EXPECT_EQ(service.available_devices("gpu_pipeline"),
            (std::vector<DeviceBackend>{DeviceBackend::CPU}));
}

/**
 * @brief Verifies default composition publishes exactly its Metal capability.
 *
 * @throws Platform registry discovery, construction, or diagnostic failures
 * unchanged.
 * @note On non-Apple builds the factory is a null stub, so this directly proves
 * CPU-only default service construction has no fake Metal account.
 */
TEST(PhysicalExecutionIntegration,
     DefaultServiceMatchesMetalAccountToPlatformRegistry) {
  ExecutionService service;
  const bool has_metal_executor =
      service.has_device_executor(DeviceBackend::Metal);
  const bool has_metal_account =
      service.device_resource_snapshot(DeviceId(DeviceBackend::Metal))
          .has_value();

  EXPECT_EQ(has_metal_account, has_metal_executor);
#if defined(__linux__)
  EXPECT_FALSE(has_metal_executor);
  EXPECT_FALSE(has_metal_account);
#endif
}

/**
 * @brief Verifies a fixed Metal executor retains its exact configured budget.
 *
 * @throws ExecutionService construction or diagnostic failures unchanged.
 * @note The additional unexecutable CUDA candidate proves construction keeps
 * only the frozen registry intersection without changing Metal byte limits.
 */
TEST(PhysicalExecutionIntegration,
     RegisteredMetalExecutorRetainsConfiguredBudget) {
  ExecutionResourceLimits limits = ExecutionService::default_resource_limits();
  const DeviceResourceVector metal_budget{12345U, 6789U};
  limits.device_limits = {
      DeviceResourceLimit{DeviceId(DeviceBackend::Metal), metal_budget},
      DeviceResourceLimit{DeviceId(DeviceBackend::CUDA),
                          DeviceResourceVector{111U, 222U}},
  };
  ExecutionService service(std::move(limits),
                           ::ps::testing::make_fake_metal_executor_registry());

  ASSERT_TRUE(service.has_device_executor(DeviceBackend::Metal));
  const std::optional<ResourceLedger::DeviceSnapshot> metal =
      service.device_resource_snapshot(DeviceId(DeviceBackend::Metal));
  ASSERT_TRUE(metal.has_value());
  EXPECT_EQ(metal->limits, metal_budget);
  EXPECT_EQ(metal->reserved, DeviceResourceVector{});
  EXPECT_EQ(metal->available, metal_budget);
  EXPECT_FALSE(service.device_resource_snapshot(DeviceId(DeviceBackend::CUDA))
                   .has_value());
}

/**
 * @brief Verifies unavailable submission devices fail before Run publication.
 */
TEST(PhysicalExecutionIntegration, RejectsDeviceOutsideRouteInventory) {
  ExecutionService service(1U);
  TestHostContext host;
  ComputeRun run(make_submission("invalid-device-route", 71U, 1));
  std::atomic_int entered{0};
  std::vector<ReadyTaskSubmission> ready;
  ready.push_back(make_ready(
      run.acquire_lease(), 0U, 1,
      [&entered](ComputeRunLease&, const ComputeRunTaskIdentity&,
                 ExecutionTaskRuntime& runtime) {
        entered.fetch_add(1, std::memory_order_relaxed);
        runtime.dec_tasks_to_complete();
      },
      DeviceBackend::Metal));

  EXPECT_THROW(service.execute_run(host, "cpu", std::move(ready), 1),
               std::invalid_argument);
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(host.exits(), 0);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies CPU fallback and Metal work overlap on distinct fixed lanes.
 */
TEST(PhysicalExecutionIntegration, CpuAndGpuPipelineLanesOverlapAndDrain) {
  ExecutionService service(ExecutionService::default_resource_limits(),
                           ::ps::testing::make_fake_metal_executor_registry());
  service.configure_worker_count(2U);
  TestHostContext host;
  ComputeRun run(make_submission("cpu-gpu-overlap", 70U, 1));
  std::promise<void> cpu_entered_promise;
  std::future<void> cpu_entered = cpu_entered_promise.get_future();
  std::promise<void> gpu_entered_promise;
  std::future<void> gpu_entered = gpu_entered_promise.get_future();
  std::promise<void> release_promise;
  const std::shared_future<void> release = release_promise.get_future().share();

  std::vector<ReadyTaskSubmission> ready;
  ready.push_back(make_ready(
      run.acquire_lease(), 0U, 1,
      [&cpu_entered_promise, release](ComputeRunLease&,
                                      const ComputeRunTaskIdentity&,
                                      ExecutionTaskRuntime& runtime) {
        cpu_entered_promise.set_value();
        release.wait();
        runtime.dec_tasks_to_complete();
      }));
  ready.push_back(make_ready(
      run.acquire_lease(), 1U, 1,
      [&gpu_entered_promise, release](ComputeRunLease&,
                                      const ComputeRunTaskIdentity&,
                                      ExecutionTaskRuntime& runtime) {
        gpu_entered_promise.set_value();
        release.wait();
        runtime.dec_tasks_to_complete();
      },
      DeviceBackend::Metal));

  std::future<void> completion = std::async(
      std::launch::async,
      [&service, &host, ready = std::move(ready)]() mutable {
        service.execute_run(host, "gpu_pipeline", std::move(ready), 2);
      });
  const bool cpu_started =
      cpu_entered.wait_for(kTestTimeout) == std::future_status::ready;
  const bool gpu_started =
      gpu_entered.wait_for(kTestTimeout) == std::future_status::ready;
  release_promise.set_value();
  const std::future_status completion_status =
      completion.wait_for(kTestTimeout);
  if (completion_status == std::future_status::ready) {
    EXPECT_NO_THROW(completion.get());
  }

  EXPECT_TRUE(cpu_started);
  EXPECT_TRUE(gpu_started);
  ASSERT_EQ(completion_status, std::future_status::ready);
  EXPECT_EQ(host.maximum_contexts(), 2);
  EXPECT_EQ(host.exits(), 2);
  EXPECT_FALSE(host.observation_failed());
  std::vector<int> workers = host.worker_ids();
  std::sort(workers.begin(), workers.end());
  ASSERT_EQ(workers.size(), 2U);
  EXPECT_LT(workers.front(), 2);
  EXPECT_EQ(workers.back(), 2);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies all three routes execute repeatedly and recover after fault.
 */
TEST(PhysicalExecutionIntegration, ExecutesAndReusesEveryPrivateRoute) {
  ExecutionService service(ExecutionService::default_resource_limits(),
                           ::ps::testing::make_fake_metal_executor_registry());
  service.configure_worker_count(2U);
  std::uint64_t revision = 1U;
  for (const std::string& route :
       std::vector<std::string>{"cpu", "gpu_pipeline", "serial_debug"}) {
    SCOPED_TRACE(route);
    std::atomic_int entered{0};
    TestHostContext first_host;
    TestHostContext second_host;
    execute_successful_run(service, route, route + "-first", revision++,
                           entered, first_host);
    execute_successful_run(service, route, route + "-second", revision++,
                           entered, second_host);
    EXPECT_EQ(entered.load(std::memory_order_relaxed), 2);
    EXPECT_EQ(first_host.exits(), 1);
    EXPECT_EQ(second_host.exits(), 1);
  }

  TestHostContext failure_host;
  ComputeRun failing(make_submission("gpu-failure", revision++, 1));
  std::vector<ReadyTaskSubmission> failing_ready;
  failing_ready.push_back(make_ready(
      failing.acquire_lease(), 0U, 1,
      [](ComputeRunLease&, const ComputeRunTaskIdentity&,
         ExecutionTaskRuntime&) {
        throw std::runtime_error("exact gpu-pipeline failure");
      },
      DeviceBackend::Metal));
  try {
    service.execute_run(failure_host, "gpu_pipeline", std::move(failing_ready),
                        1);
    FAIL() << "Expected GPU-pipeline callback failure";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "exact gpu-pipeline failure");
  }
  std::atomic_int recovered{0};
  TestHostContext recovery_host;
  EXPECT_NO_THROW(execute_successful_run(service, "gpu_pipeline",
                                         "gpu-recovery", revision++, recovered,
                                         recovery_host, DeviceBackend::Metal));
  EXPECT_EQ(recovered.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies serial-debug uses worker zero and one callback globally.
 */
TEST(PhysicalExecutionIntegration, SerialDebugIsWorkerZeroSingleFlight) {
  ExecutionService service(2U);
  TestHostContext host;
  ComputeRun run(make_submission("serial-single-flight", 20U, 1));
  std::atomic_int active{0};
  std::atomic_int maximum{0};
  std::vector<ReadyTaskSubmission> ready;
  for (std::uint64_t task_id = 0U; task_id < 2U; ++task_id) {
    ready.push_back(make_ready(
        run.acquire_lease(), task_id, 1,
        [&active, &maximum](ComputeRunLease&, const ComputeRunTaskIdentity&,
                            ExecutionTaskRuntime& runtime) {
          const int current =
              active.fetch_add(1, std::memory_order_acq_rel) + 1;
          int observed = maximum.load(std::memory_order_relaxed);
          while (observed < current &&
                 !maximum.compare_exchange_weak(observed, current,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          active.fetch_sub(1, std::memory_order_acq_rel);
          runtime.dec_tasks_to_complete();
        }));
  }
  service.execute_run(host, "serial_debug", std::move(ready), 2);
  EXPECT_EQ(maximum.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(host.maximum_contexts(), 1);
  EXPECT_EQ(host.exits(), 2);
  EXPECT_FALSE(host.observation_failed());
  const std::vector<int> workers = host.worker_ids();
  ASSERT_EQ(workers.size(), 2U);
  EXPECT_TRUE(std::all_of(workers.begin(), workers.end(),
                          [](int worker) { return worker == 0; }));
}

/**
 * @brief Verifies invalid decisions fault one generation until replacement.
 */
#if defined(__linux__)
// These cases exercise a signed native policy callback; Darwin is fail-closed.
TEST_F(PolicyExecutionFixture,
       InvalidDecisionFaultIsStickyAndSameTypeReplacementRecovers) {
  ExecutionService service(1U);
  control_.set_select_mode(PS_POLICY_FIXTURE_SELECT_RESERVED_NONZERO);
  service.replace_policy(PolicyClass::Throughput, "fixture_policy");
  const std::uint64_t first_generation =
      service.policy_info(PolicyClass::Throughput).binding_generation;

  std::atomic_int entered{0};
  TestHostContext first_host;
  execute_successful_run(service, "cpu", "invalid-fallback", 30U, entered,
                         first_host);
  ASSERT_EQ(entered.load(std::memory_order_relaxed), 1);
  ASSERT_EQ(control_.select_count(), 1U);
  const PolicyInfoSnapshot faulted =
      service.policy_info(PolicyClass::Throughput);
  ASSERT_TRUE(faulted.fault.has_value());
  EXPECT_EQ(faulted.fault->reason, PolicyFaultReason::MalformedDecision);

  control_.set_select_mode(PS_POLICY_FIXTURE_SELECT_LAST);
  TestHostContext bypass_host;
  execute_successful_run(service, "cpu", "sticky-bypass", 31U, entered,
                         bypass_host);
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 2);
  EXPECT_EQ(control_.select_count(), 1U);

  service.replace_policy(PolicyClass::Throughput, "fixture_policy");
  const PolicyInfoSnapshot replacement =
      service.policy_info(PolicyClass::Throughput);
  EXPECT_GT(replacement.binding_generation, first_generation);
  EXPECT_FALSE(replacement.fault.has_value());
  TestHostContext recovered_host;
  execute_successful_run(service, "cpu", "replacement-recovery", 32U, entered,
                         recovered_host);
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 3);
  EXPECT_EQ(control_.select_count(), 2U);
}

/**
 * @brief Verifies three obsolete plugin decisions fall back without faulting.
 */
TEST_F(PolicyExecutionFixture,
       ThreeConcurrentReplacementsExhaustRetryBudgetWithoutFault) {
  ExecutionService service(1U);
  service.replace_policy(PolicyClass::Throughput, "fixture_policy");
  CyclingHookState hook;
  ScopedHookRelease release_guard(hook);
  control_.set_hook(&cycling_select_hook, &hook);
  control_.set_select_mode(PS_POLICY_FIXTURE_SELECT_HOOK_LAST);
  TestHostContext host;
  ComputeRun run(make_submission("obsolete-replacements", 40U, 1));
  std::atomic_int entered{0};
  std::vector<ReadyTaskSubmission> ready;
  ready.push_back(
      make_ready(run.acquire_lease(), 0U, 1,
                 [&entered](ComputeRunLease&, const ComputeRunTaskIdentity&,
                            ExecutionTaskRuntime& runtime) {
                   entered.fetch_add(1, std::memory_order_relaxed);
                   runtime.dec_tasks_to_complete();
                 }));
  std::future<void> completion =
      std::async(std::launch::async,
                 [&service, &host, ready = std::move(ready)]() mutable {
                   service.execute_run(host, "cpu", std::move(ready), 1);
                 });

  bool observed_all = true;
  for (unsigned int attempt = 1U; attempt <= 3U; ++attempt) {
    if (!hook.wait_for_entry(attempt)) {
      observed_all = false;
      break;
    }
    service.replace_policy(PolicyClass::Throughput, "fixture_policy");
    hook.release(attempt);
  }
  if (!observed_all) {
    hook.release_all();
  }
  const std::future_status status = completion.wait_for(kTestTimeout);
  if (status == std::future_status::ready) {
    EXPECT_NO_THROW(completion.get());
  }
  ASSERT_TRUE(observed_all);
  ASSERT_EQ(status, std::future_status::ready);
  EXPECT_EQ(control_.select_count(), 3U);
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 1);
  EXPECT_FALSE(service.policy_info(PolicyClass::Throughput).fault.has_value());
  control_.set_hook(nullptr, nullptr);
}

/**
 * @brief Verifies cancellation during selection purges the pin without ABA.
 */
TEST_F(PolicyExecutionFixture,
       CancellationDuringSelectionPurgesEntryAndNextRunReusesCapacity) {
  ExecutionService service(1U);
  service.replace_policy(PolicyClass::Throughput, "fixture_policy");
  CyclingHookState hook;
  ScopedHookRelease release_guard(hook);
  control_.set_hook(&cycling_select_hook, &hook);
  control_.set_select_mode(PS_POLICY_FIXTURE_SELECT_HOOK_LAST);
  TestHostContext host;
  ComputeRun run(make_submission("cancelled-selection", 50U, 1));
  const ComputeRunCancellationSource cancellation = run.cancellation_source();
  std::atomic_int entered{0};
  std::vector<ReadyTaskSubmission> ready;
  ready.push_back(
      make_ready(run.acquire_lease(), 0U, 1,
                 [&entered](ComputeRunLease&, const ComputeRunTaskIdentity&,
                            ExecutionTaskRuntime& runtime) {
                   entered.fetch_add(1, std::memory_order_relaxed);
                   runtime.dec_tasks_to_complete();
                 }));
  std::future<void> completion =
      std::async(std::launch::async,
                 [&service, &host, ready = std::move(ready)]() mutable {
                   service.execute_run(host, "cpu", std::move(ready), 1);
                 });

  const bool callback_entered = hook.wait_for_entry(1U);
  if (callback_entered) {
    EXPECT_TRUE(cancellation.request_cancellation(
        ComputeRunCancellationReason::ExplicitRequest));
  }
  hook.release_all();
  const std::future_status status = completion.wait_for(kTestTimeout);
  if (status == std::future_status::ready) {
    EXPECT_NO_THROW(completion.get());
  }
  ASSERT_TRUE(callback_entered);
  ASSERT_EQ(status, std::future_status::ready);
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(control_.select_count(), 1U);
  EXPECT_FALSE(service.policy_info(PolicyClass::Throughput).fault.has_value());
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});

  control_.set_hook(nullptr, nullptr);
  control_.set_select_mode(PS_POLICY_FIXTURE_SELECT_LAST);
  TestHostContext recovery_host;
  execute_successful_run(service, "cpu", "cancelled-selection", 51U, entered,
                         recovery_host);
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(control_.select_count(), 2U);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}
#endif

/**
 * @brief Verifies evidence startability excludes an exhausted child grant.
 *
 * A dedicated Metal callback first holds the physical worker. While it is
 * blocked, a Throughput Run holds its sole execution grant in a CPU callback,
 * publishes a Metal dependent, and an independent Interactive Metal Run also
 * queues. Releasing the worker selects Interactive before any grant-block mark
 * can hide Throughput; the committed observation must therefore report the
 * real Throughput candidate as not evidence-startable solely from capacity.
 *
 * @throws Service admission, synchronization, or callback failures unchanged.
 * @note The blocked Throughput entry remains ready and later executes exactly
 * once; the evidence fact therefore cannot be explained by queue absence.
 */
TEST(ExecutionServiceStartObservation,
     ChildGrantExhaustionMakesRealCandidateNotEvidenceStartable) {
  ExecutionService service(ExecutionService::default_resource_limits(),
                           ::ps::testing::make_fake_metal_executor_registry());
  service.configure_worker_count(1U);

  TestHostContext blocker_host;
  ComputeRun blocker_run(make_submission("evidence-capacity-blocker", 64U, 1,
                                         ComputeRunQosClass::Interactive));
  std::atomic_int blocker_entries{0};
  std::promise<void> blocker_entered_promise;
  std::future<void> blocker_entered = blocker_entered_promise.get_future();
  std::promise<void> release_blocker_promise;
  const std::shared_future<void> release_blocker =
      release_blocker_promise.get_future().share();
  std::vector<ReadyTaskSubmission> blocker_ready;
  blocker_ready.push_back(make_ready(
      blocker_run.acquire_lease(), 0U, 1,
      [&blocker_entries, &blocker_entered_promise, release_blocker](
          ComputeRunLease&, const ComputeRunTaskIdentity&,
          ExecutionTaskRuntime& runtime) {
        blocker_entries.fetch_add(1, std::memory_order_relaxed);
        blocker_entered_promise.set_value();
        release_blocker.wait();
        runtime.dec_tasks_to_complete();
      },
      DeviceBackend::Metal));

  TestHostContext throughput_host;
  ComputeRun throughput_run(
      make_submission("evidence-capacity-blocked", 65U, 1));
  std::atomic_int throughput_cpu_entries{0};
  std::atomic_int throughput_gpu_entries{0};
  std::promise<void> throughput_cpu_entered_promise;
  std::future<void> throughput_cpu_entered =
      throughput_cpu_entered_promise.get_future();
  std::promise<void> throughput_gpu_entered_promise;
  std::future<void> throughput_gpu_entered =
      throughput_gpu_entered_promise.get_future();
  std::promise<void> release_throughput_cpu_promise;
  const std::shared_future<void> release_throughput_cpu =
      release_throughput_cpu_promise.get_future().share();
  std::vector<ReadyTaskSubmission> throughput_ready;
  throughput_ready.push_back(make_ready(
      throughput_run.acquire_lease(), 0U, 1,
      [&throughput_cpu_entries, &throughput_gpu_entries,
       &throughput_cpu_entered_promise, &throughput_gpu_entered_promise,
       release_throughput_cpu](ComputeRunLease& lease,
                               const ComputeRunTaskIdentity&,
                               ExecutionTaskRuntime& runtime) {
        auto* ready_runtime =
            dynamic_cast<ReadyTaskSubmissionRuntime*>(&runtime);
        if (ready_runtime == nullptr) {
          throw std::logic_error(
              "Evidence-capacity regression requires ready runtime.");
        }
        ready_runtime->submit_ready_submission(make_ready(
            lease, 1U, 2,
            [&throughput_gpu_entries, &throughput_gpu_entered_promise](
                ComputeRunLease&, const ComputeRunTaskIdentity&,
                ExecutionTaskRuntime& gpu_runtime) {
              throughput_gpu_entries.fetch_add(1, std::memory_order_relaxed);
              throughput_gpu_entered_promise.set_value();
              gpu_runtime.dec_tasks_to_complete();
            },
            DeviceBackend::Metal, ExecutionTaskPriority::High, false));
        throughput_cpu_entries.fetch_add(1, std::memory_order_relaxed);
        throughput_cpu_entered_promise.set_value();
        release_throughput_cpu.wait();
        runtime.dec_tasks_to_complete();
      }));

  std::future<void> blocker_completion = std::async(
      std::launch::async,
      [&service, &blocker_host, ready = std::move(blocker_ready)]() mutable {
        service.execute_run(blocker_host, "gpu_pipeline", std::move(ready), 1);
      });
  std::future<void> throughput_completion;
  std::future<void> interactive_completion;
  ScopedPromiseRelease release_throughput_guard(release_throughput_cpu_promise);
  ScopedPromiseRelease release_blocker_guard(release_blocker_promise);
  ASSERT_EQ(blocker_entered.wait_for(kTestTimeout), std::future_status::ready);

  throughput_completion = std::async(
      std::launch::async, [&service, &throughput_host,
                           ready = std::move(throughput_ready)]() mutable {
        service.execute_run(throughput_host, "gpu_pipeline", std::move(ready),
                            2);
      });
  ASSERT_EQ(throughput_cpu_entered.wait_for(kTestTimeout),
            std::future_status::ready);

  auto observation_sink = std::make_shared<ServiceStartObservationSink>();
  ComputeRunSubmission interactive_submission =
      make_submission("evidence-independent-interactive", 66U, 1,
                      ComputeRunQosClass::Interactive);
  interactive_submission.observation_sink = observation_sink;
  ComputeRun interactive_run(std::move(interactive_submission));
  TestHostContext interactive_host;
  std::atomic_int interactive_entries{0};
  std::vector<ReadyTaskSubmission> interactive_ready;
  interactive_ready.push_back(make_ready(
      interactive_run.acquire_lease(), 0U, 1,
      [&interactive_entries](ComputeRunLease&, const ComputeRunTaskIdentity&,
                             ExecutionTaskRuntime& runtime) {
        interactive_entries.fetch_add(1, std::memory_order_relaxed);
        runtime.dec_tasks_to_complete();
      },
      DeviceBackend::Metal));
  interactive_completion = std::async(
      std::launch::async, [&service, &interactive_host,
                           ready = std::move(interactive_ready)]() mutable {
        service.execute_run(interactive_host, "gpu_pipeline", std::move(ready),
                            1);
      });
  ASSERT_TRUE(wait_for_ready_entries(service, 1U, 1U));
  release_blocker_guard.release();

  ASSERT_EQ(interactive_completion.wait_for(kTestTimeout),
            std::future_status::ready);
  ASSERT_EQ(blocker_completion.wait_for(kTestTimeout),
            std::future_status::ready);
  EXPECT_NO_THROW(interactive_completion.get());
  EXPECT_NO_THROW(blocker_completion.get());
  ASSERT_EQ(observation_sink->service_start_count(), 1U);
  const ComputeRunServiceStartObservation observation =
      observation_sink->service_start_observation();
  EXPECT_TRUE(observation.interactive_candidate_startable);
  EXPECT_FALSE(observation.throughput_candidate_startable);
  EXPECT_TRUE(observation.execution_grant_committed);
  EXPECT_EQ(interactive_entries.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(throughput_gpu_entered.wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);
  EXPECT_EQ(service.ready_class_snapshot().throughput_entries, 1U);

  release_throughput_guard.release();
  ASSERT_EQ(throughput_gpu_entered.wait_for(kTestTimeout),
            std::future_status::ready);
  ASSERT_EQ(throughput_completion.wait_for(kTestTimeout),
            std::future_status::ready);
  EXPECT_NO_THROW(throughput_completion.get());
  EXPECT_EQ(throughput_cpu_entries.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(throughput_gpu_entries.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(blocker_entries.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(blocker_host.exits(), 1);
  EXPECT_EQ(throughput_host.exits(), 2);
  EXPECT_EQ(interactive_host.exits(), 1);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies evidence startability includes both capacity-ready classes.
 *
 * A dedicated Metal callback holds the physical worker while one Throughput
 * and one Interactive Run publish independent Metal candidates. Once released,
 * class arbitration selects Interactive and its committed observation samples
 * both live child reservations as capacity-ready.
 *
 * @throws Service admission, synchronization, or callback failures unchanged.
 * @note The physical blocker owns a separate Run, so neither observed class
 * candidate consumes its own reservation before the applicability cut.
 */
TEST(ExecutionServiceStartObservation,
     CapacityReadyCandidatesAreBothEvidenceStartable) {
  ExecutionService service(ExecutionService::default_resource_limits(),
                           ::ps::testing::make_fake_metal_executor_registry());
  service.configure_worker_count(1U);

  TestHostContext blocker_host;
  ComputeRun blocker_run(make_submission("evidence-gpu-blocker", 66U, 1,
                                         ComputeRunQosClass::Interactive));
  std::atomic_int blocker_entries{0};
  std::promise<void> blocker_entered_promise;
  std::future<void> blocker_entered = blocker_entered_promise.get_future();
  std::promise<void> release_blocker_promise;
  const std::shared_future<void> release_blocker =
      release_blocker_promise.get_future().share();
  std::vector<ReadyTaskSubmission> blocker_ready;
  blocker_ready.push_back(make_ready(
      blocker_run.acquire_lease(), 0U, 1,
      [&blocker_entries, &blocker_entered_promise, release_blocker](
          ComputeRunLease&, const ComputeRunTaskIdentity&,
          ExecutionTaskRuntime& runtime) {
        blocker_entries.fetch_add(1, std::memory_order_relaxed);
        blocker_entered_promise.set_value();
        release_blocker.wait();
        runtime.dec_tasks_to_complete();
      },
      DeviceBackend::Metal));

  TestHostContext throughput_host;
  ComputeRun throughput_run(
      make_submission("evidence-throughput-ready", 67U, 1));
  std::atomic_int throughput_entries{0};
  std::vector<ReadyTaskSubmission> throughput_ready;
  throughput_ready.push_back(make_ready(
      throughput_run.acquire_lease(), 0U, 1,
      [&throughput_entries](ComputeRunLease&, const ComputeRunTaskIdentity&,
                            ExecutionTaskRuntime& runtime) {
        throughput_entries.fetch_add(1, std::memory_order_relaxed);
        runtime.dec_tasks_to_complete();
      },
      DeviceBackend::Metal));

  auto observation_sink = std::make_shared<ServiceStartObservationSink>();
  ComputeRunSubmission interactive_submission = make_submission(
      "evidence-interactive-ready", 68U, 1, ComputeRunQosClass::Interactive);
  interactive_submission.observation_sink = observation_sink;
  ComputeRun interactive_run(std::move(interactive_submission));
  TestHostContext interactive_host;
  std::atomic_int interactive_entries{0};
  std::vector<ReadyTaskSubmission> interactive_ready;
  interactive_ready.push_back(make_ready(
      interactive_run.acquire_lease(), 0U, 1,
      [&interactive_entries](ComputeRunLease&, const ComputeRunTaskIdentity&,
                             ExecutionTaskRuntime& runtime) {
        interactive_entries.fetch_add(1, std::memory_order_relaxed);
        runtime.dec_tasks_to_complete();
      },
      DeviceBackend::Metal));

  std::future<void> blocker_completion = std::async(
      std::launch::async,
      [&service, &blocker_host, ready = std::move(blocker_ready)]() mutable {
        service.execute_run(blocker_host, "gpu_pipeline", std::move(ready), 1);
      });
  std::future<void> throughput_completion;
  std::future<void> interactive_completion;
  ScopedPromiseRelease release_guard(release_blocker_promise);
  ASSERT_EQ(blocker_entered.wait_for(kTestTimeout), std::future_status::ready);

  throughput_completion = std::async(
      std::launch::async, [&service, &throughput_host,
                           ready = std::move(throughput_ready)]() mutable {
        service.execute_run(throughput_host, "gpu_pipeline", std::move(ready),
                            1);
      });
  interactive_completion = std::async(
      std::launch::async, [&service, &interactive_host,
                           ready = std::move(interactive_ready)]() mutable {
        service.execute_run(interactive_host, "gpu_pipeline", std::move(ready),
                            1);
      });
  ASSERT_TRUE(wait_for_ready_entries(service, 1U, 1U));

  release_guard.release();
  ASSERT_EQ(interactive_completion.wait_for(kTestTimeout),
            std::future_status::ready);
  ASSERT_EQ(throughput_completion.wait_for(kTestTimeout),
            std::future_status::ready);
  ASSERT_EQ(blocker_completion.wait_for(kTestTimeout),
            std::future_status::ready);
  EXPECT_NO_THROW(interactive_completion.get());
  EXPECT_NO_THROW(throughput_completion.get());
  EXPECT_NO_THROW(blocker_completion.get());
  ASSERT_EQ(observation_sink->service_start_count(), 1U);
  const ComputeRunServiceStartObservation observation =
      observation_sink->service_start_observation();
  EXPECT_TRUE(observation.interactive_candidate_startable);
  EXPECT_TRUE(observation.throughput_candidate_startable);
  EXPECT_TRUE(observation.execution_grant_committed);
  EXPECT_EQ(blocker_entries.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(interactive_entries.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(throughput_entries.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(blocker_host.exits(), 1);
  EXPECT_EQ(interactive_host.exits(), 1);
  EXPECT_EQ(throughput_host.exits(), 1);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies a grant-blocked high-priority Run cannot starve another Run.
 *
 * Run A holds its only execution child grant in a CPU callback and publishes a
 * high-priority Metal dependent. The GPU worker must retain that exact ready
 * entry without charging dispatch, then select lower-priority independent Run
 * B. Releasing A must subsequently execute its retained dependent exactly once.
 */
#if defined(__linux__)
// These cases exercise a signed native policy callback on the Linux profile.
TEST_F(PolicyExecutionFixture,
       GrantBlockedGpuCandidateDoesNotStarveIndependentRun) {
  ExecutionService service(ExecutionService::default_resource_limits(),
                           ::ps::testing::make_fake_metal_executor_registry());
  service.configure_worker_count(1U);
  control_.set_select_mode(PS_POLICY_FIXTURE_SELECT_FIRST);
  service.replace_policy(PolicyClass::Throughput, "fixture_policy");

  TestHostContext host_a;
  ComputeRun run_a(make_submission("grant-blocked-a", 61U, 1));
  std::atomic_int cpu_a_entries{0};
  std::atomic_int gpu_a_entries{0};
  std::promise<void> cpu_a_entered_promise;
  std::future<void> cpu_a_entered = cpu_a_entered_promise.get_future();
  std::promise<void> gpu_a_entered_promise;
  std::future<void> gpu_a_entered = gpu_a_entered_promise.get_future();
  std::promise<void> release_cpu_a_promise;
  const std::shared_future<void> release_cpu_a =
      release_cpu_a_promise.get_future().share();
  std::vector<ReadyTaskSubmission> ready_a;
  ready_a.push_back(make_ready(
      run_a.acquire_lease(), 0U, 1,
      [&cpu_a_entries, &gpu_a_entries, &cpu_a_entered_promise,
       &gpu_a_entered_promise,
       release_cpu_a](ComputeRunLease& lease, const ComputeRunTaskIdentity&,
                      ExecutionTaskRuntime& runtime) {
        auto* ready_runtime =
            dynamic_cast<ReadyTaskSubmissionRuntime*>(&runtime);
        if (ready_runtime == nullptr) {
          throw std::logic_error(
              "Grant-block regression requires ready-submission runtime.");
        }
        ready_runtime->submit_ready_submission(make_ready(
            lease, 1U, 2,
            [&gpu_a_entries, &gpu_a_entered_promise](
                ComputeRunLease&, const ComputeRunTaskIdentity&,
                ExecutionTaskRuntime& gpu_runtime) {
              gpu_a_entries.fetch_add(1, std::memory_order_relaxed);
              gpu_a_entered_promise.set_value();
              gpu_runtime.dec_tasks_to_complete();
            },
            DeviceBackend::Metal, ExecutionTaskPriority::High, false));
        cpu_a_entries.fetch_add(1, std::memory_order_relaxed);
        cpu_a_entered_promise.set_value();
        release_cpu_a.wait();
        runtime.dec_tasks_to_complete();
      }));

  TestHostContext host_b;
  ComputeRun run_b(make_submission("grant-blocked-b", 62U, 1));
  std::atomic_int gpu_b_entries{0};
  std::promise<void> gpu_b_entered_promise;
  std::future<void> gpu_b_entered = gpu_b_entered_promise.get_future();
  std::future<void> completion_a = std::async(
      std::launch::async,
      [&service, &host_a, ready = std::move(ready_a)]() mutable {
        service.execute_run(host_a, "gpu_pipeline", std::move(ready), 2);
      });
  std::future<void> completion_b;
  ScopedPromiseRelease release_guard(release_cpu_a_promise);

  ASSERT_EQ(cpu_a_entered.wait_for(kTestTimeout), std::future_status::ready);
  ASSERT_TRUE(wait_for_select_count(control_, 2U));
  EXPECT_EQ(gpu_a_entered.wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);

  std::vector<ReadyTaskSubmission> ready_b;
  ready_b.push_back(make_ready(
      run_b.acquire_lease(), 0U, 1,
      [&gpu_b_entries, &gpu_b_entered_promise](ComputeRunLease&,
                                               const ComputeRunTaskIdentity&,
                                               ExecutionTaskRuntime& runtime) {
        gpu_b_entries.fetch_add(1, std::memory_order_relaxed);
        gpu_b_entered_promise.set_value();
        runtime.dec_tasks_to_complete();
      },
      DeviceBackend::Metal));
  completion_b = std::async(
      std::launch::async,
      [&service, &host_b, ready = std::move(ready_b)]() mutable {
        service.execute_run(host_b, "gpu_pipeline", std::move(ready), 1);
      });

  ASSERT_EQ(gpu_b_entered.wait_for(kTestTimeout), std::future_status::ready);
  ASSERT_EQ(completion_b.wait_for(kTestTimeout), std::future_status::ready);
  EXPECT_NO_THROW(completion_b.get());
  EXPECT_EQ(gpu_b_entries.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(gpu_a_entries.load(std::memory_order_relaxed), 0);
  EXPECT_NE(service.get_stats().find("Ready tasks: 1"), std::string::npos);

  release_guard.release();
  ASSERT_EQ(gpu_a_entered.wait_for(kTestTimeout), std::future_status::ready);
  ASSERT_EQ(completion_a.wait_for(kTestTimeout), std::future_status::ready);
  EXPECT_NO_THROW(completion_a.get());
  EXPECT_EQ(cpu_a_entries.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(gpu_a_entries.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(host_a.exits(), 2);
  EXPECT_EQ(host_b.exits(), 1);
  EXPECT_FALSE(host_a.observation_failed());
  EXPECT_FALSE(host_b.observation_failed());
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies a sole grant-blocked candidate waits without retry spinning.
 *
 * The fixture selection counter bounds retries while Run A's CPU callback owns
 * its only execution grant. Accepted cancellation must purge the retained GPU
 * ready entry, wake the blocked worker, and leave no callback or resource leak;
 * service destruction then wakes both idle physical workers.
 */
TEST_F(PolicyExecutionFixture,
       SoleGrantBlockedCandidateWaitsAndCancellationWakesWorker) {
  ExecutionService service(ExecutionService::default_resource_limits(),
                           ::ps::testing::make_fake_metal_executor_registry());
  service.configure_worker_count(1U);
  control_.set_select_mode(PS_POLICY_FIXTURE_SELECT_FIRST);
  service.replace_policy(PolicyClass::Throughput, "fixture_policy");

  TestHostContext host;
  ComputeRun run(make_submission("grant-blocked-cancel", 63U, 1));
  const ComputeRunCancellationSource cancellation = run.cancellation_source();
  std::atomic_int cpu_entries{0};
  std::atomic_int gpu_entries{0};
  std::promise<void> cpu_entered_promise;
  std::future<void> cpu_entered = cpu_entered_promise.get_future();
  std::promise<void> release_cpu_promise;
  const std::shared_future<void> release_cpu =
      release_cpu_promise.get_future().share();
  std::vector<ReadyTaskSubmission> ready;
  ready.push_back(make_ready(
      run.acquire_lease(), 0U, 1,
      [&cpu_entries, &gpu_entries, &cpu_entered_promise, release_cpu](
          ComputeRunLease& lease, const ComputeRunTaskIdentity&,
          ExecutionTaskRuntime& runtime) {
        auto* ready_runtime =
            dynamic_cast<ReadyTaskSubmissionRuntime*>(&runtime);
        if (ready_runtime == nullptr) {
          throw std::logic_error(
              "Grant-block regression requires ready-submission runtime.");
        }
        ready_runtime->submit_ready_submission(make_ready(
            lease, 1U, 2,
            [&gpu_entries](ComputeRunLease&, const ComputeRunTaskIdentity&,
                           ExecutionTaskRuntime& gpu_runtime) {
              gpu_entries.fetch_add(1, std::memory_order_relaxed);
              gpu_runtime.dec_tasks_to_complete();
            },
            DeviceBackend::Metal, ExecutionTaskPriority::High, false));
        cpu_entries.fetch_add(1, std::memory_order_relaxed);
        cpu_entered_promise.set_value();
        release_cpu.wait();
        runtime.dec_tasks_to_complete();
      }));
  std::future<void> completion = std::async(
      std::launch::async,
      [&service, &host, ready = std::move(ready)]() mutable {
        service.execute_run(host, "gpu_pipeline", std::move(ready), 2);
      });
  ScopedPromiseRelease release_guard(release_cpu_promise);

  ASSERT_EQ(cpu_entered.wait_for(kTestTimeout), std::future_status::ready);
  ASSERT_TRUE(wait_for_select_count(control_, 2U));
  const std::uint32_t before_wait = control_.select_count();
  std::this_thread::sleep_for(std::chrono::milliseconds(180));
  const std::uint32_t after_wait = control_.select_count();
  EXPECT_LE(after_wait - before_wait, 6U);
  EXPECT_NE(service.get_stats().find("Ready tasks: 1"), std::string::npos);

  EXPECT_TRUE(cancellation.request_cancellation(
      ComputeRunCancellationReason::ExplicitRequest));
  EXPECT_NE(service.get_stats().find("Ready tasks: 0"), std::string::npos);
  release_guard.release();
  ASSERT_EQ(completion.wait_for(kTestTimeout), std::future_status::ready);
  EXPECT_NO_THROW(completion.get());
  EXPECT_EQ(cpu_entries.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(gpu_entries.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(host.exits(), 1);
  EXPECT_FALSE(host.observation_failed());
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies displaced plugin destroy failure is recorded after one try.
 *
 * @return Nothing; GoogleTest assertions report duplicate destruction,
 * missing retirement telemetry, or a leaked binding counter.
 * @throws Policy replacement, snapshot, or synchronization failures unchanged.
 * @note Returned destroy failure cannot roll back the already published
 * replacement and is represented by one FailureOther BindingRetired record.
 */
TEST_F(PolicyExecutionFixture,
       DestroyFailureRetiresDisplacedBindingOnceAndRecordsTelemetry) {
  ExecutionService service(1U);
  service.replace_policy(PolicyClass::Throughput, "fixture_policy");
  control_.set_destroy_mode(PS_POLICY_FIXTURE_DESTROY_STATUS_INTERNAL);

  service.replace_policy(PolicyClass::Throughput, "throughput");
  EXPECT_EQ(control_.destroy_count(), 1U);

  const ExecutionLifecyclePage page = service.lifecycle_snapshot(0U, 64U);
  EXPECT_EQ(page.counters.live_policy_binding_count, 2U);
  const auto failure = std::find_if(
      page.records.begin(), page.records.end(),
      [](const ExecutionLifecycleEvent& event) {
        return event.kind == ExecutionLifecycleEventKind::BindingRetired &&
               event.generation == 2U &&
               event.category == ExecutionLifecycleCategory::FailureOther;
      });
  EXPECT_NE(failure, page.records.end());
}
#endif

/**
 * @brief Verifies child-grant rollback leaves the exact entry safely retryable.
 */
TEST(ExecutionServiceReservedStart,
     RollsBackGrantWithoutCandidateVersionAbaOrResourceLeak) {
  ExecutionService service(1U);
  ScopedReservedStartProbe probe_guard(service);
  std::atomic_int entered{0};
  TestHostContext host;
  execute_successful_run(service, "cpu", "reserved-start-rollback", 60U,
                         entered, host);
  const compute::testing::ReservedStartRollbackProbeSnapshot probe =
      ::ps::testing::ExecutionServiceTestAccess::
          reserved_start_rollback_probe_snapshot(service);

  ASSERT_EQ(probe.calls, 2U);
  EXPECT_NE(probe.candidate_ids[0], 0U);
  EXPECT_EQ(probe.candidate_ids[0], probe.candidate_ids[1]);
  EXPECT_NE(probe.entry_versions[0], 0U);
  EXPECT_EQ(probe.entry_versions[0], probe.entry_versions[1]);
  EXPECT_EQ(probe.route_generations[0], 1U);
  EXPECT_EQ(probe.route_generations[0], probe.route_generations[1]);
  EXPECT_EQ(probe.resources[0],
            (ResourceVector{1U, probe.resources[0].retained_memory_bytes,
                            probe.resources[0].scratch_bytes, 0U, 0U}));
  EXPECT_EQ(probe.resources[0], probe.resources[1]);
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(host.exits(), 1);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

}  // namespace
}  // namespace ps::compute
