#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "compute/compute_run.hpp"
#include "compute/execution_service.hpp"
#include "execution/execution_task_runtime.hpp"
#include "photospider/host/host.hpp"
#include "policy/policy_registry.hpp"
#include "support/execution_service_test_access.hpp"
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
          SupersessionGeneration(1U)}};
}

/**
 * @brief Minimal thread-safe Host observation context for route integration.
 * @throws std::bad_alloc only when copied worker-id observations grow.
 * @note Tests opt into a deterministic fake Metal capability; no native
 * backend, command queue, or platform probe is required.
 */
class TestHostContext final : public ExecutionHostContext {
 public:
  /**
   * @brief Creates a Host context with optional fake Metal availability.
   * @param metal_available Whether `GPU_METAL` should be reported available.
   * @throws Nothing.
   */
  explicit TestHostContext(bool metal_available = false) noexcept
      : metal_available_(metal_available) {}

  /** @copydoc ExecutionHostContext::is_device_available */
  bool is_device_available(Device device) const noexcept override {
    return device == Device::CPU ||
           (metal_available_ && device == Device::GPU_METAL);
  }

  /** @copydoc ExecutionHostContext::set_task_context */
  void set_task_context(int worker_id, std::uint64_t epoch) noexcept override {
    (void)epoch;
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
  void log_event(ExecutionTraceAction, int, int,
                 std::uint64_t) noexcept override {}

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
  /** @brief Deterministic fake Metal capability. */
  bool metal_available_ = false;

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
 * @brief Builds one ready submission whose callback owns completion release.
 * @param lease Strong matching Run lease transferred into the submission.
 * @param local_task_id Dense Run-local task id.
 * @param trace_node_id Diagnostic trace node.
 * @param executable Test callback entered by a physical route.
 * @param device Private lane selected for the callback.
 * @return Move-owned ready submission.
 * @throws Standard callback, metadata, or function ownership exceptions.
 */
ReadyTaskSubmission make_ready(ComputeRunLease lease,
                               std::uint64_t local_task_id, int trace_node_id,
                               ReadyTaskSubmission::Executable executable,
                               Device device = Device::CPU) {
  const ComputeRunTaskIdentity identity = lease.task_identity(local_task_id);
  return ReadyTaskSubmission(
      std::move(lease), identity, trace_node_id, true, std::move(executable),
      ExecutionTaskPriority::Normal,
      ReadyTaskSubmission::default_resource_demand(), device);
}

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
                            Device device = Device::CPU) {
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
 * @brief Verifies route-aware device discovery has deterministic fallback.
 */
TEST(PhysicalExecutionIntegration, PublishesRouteAwareDeviceInventory) {
  ExecutionService service(2U);
  TestHostContext cpu_only_host;
  TestHostContext metal_host(true);

  EXPECT_EQ(service.available_devices(cpu_only_host, "cpu"),
            (std::vector<Device>{Device::CPU}));
  EXPECT_EQ(service.available_devices(metal_host, "serial_debug"),
            (std::vector<Device>{Device::CPU}));
  EXPECT_EQ(service.available_devices(cpu_only_host, "gpu_pipeline"),
            (std::vector<Device>{Device::CPU}));
  EXPECT_EQ(service.available_devices(metal_host, "gpu_pipeline"),
            (std::vector<Device>{Device::GPU_METAL, Device::CPU}));
  EXPECT_THROW(service.available_devices(metal_host, "heterogeneous"),
               GraphError);
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
      Device::GPU_METAL));

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
  ExecutionService service(2U);
  TestHostContext host(true);
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
      Device::GPU_METAL));

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
  ExecutionService service(2U);
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

  TestHostContext failure_host(true);
  ComputeRun failing(make_submission("gpu-failure", revision++, 1));
  std::vector<ReadyTaskSubmission> failing_ready;
  failing_ready.push_back(make_ready(
      failing.acquire_lease(), 0U, 1,
      [](ComputeRunLease&, const ComputeRunTaskIdentity&,
         ExecutionTaskRuntime&) {
        throw std::runtime_error("exact gpu-pipeline failure");
      },
      Device::GPU_METAL));
  try {
    service.execute_run(failure_host, "gpu_pipeline", std::move(failing_ready),
                        1);
    FAIL() << "Expected GPU-pipeline callback failure";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "exact gpu-pipeline failure");
  }
  std::atomic_int recovered{0};
  TestHostContext recovery_host(true);
  EXPECT_NO_THROW(execute_successful_run(service, "gpu_pipeline",
                                         "gpu-recovery", revision++, recovered,
                                         recovery_host, Device::GPU_METAL));
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
