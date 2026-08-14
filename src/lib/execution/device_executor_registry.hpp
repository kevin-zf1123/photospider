#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "execution/device_completion.hpp"
#include "execution/residency_manager.hpp"
#include "photospider/core/device.hpp"
#include "runtime/resource_ledger.hpp"

/**
 * @file device_executor_registry.hpp
 * @brief Private fixed process-domain device executor ownership.
 */

namespace ps::execution {

/**
 * @brief Source-private checkpoints governed by one executor invocation
 * deadline.
 *
 * @throws Nothing for value construction, copying, and comparison.
 * @note The checkpoints identify semantic observations rather than independent
 * timeout budgets. An invocation that supplies a deadline MUST use the same
 * absolute steady-clock point at every checkpoint.
 */
enum class DeviceExecutorDeadlineCheckpoint : std::uint32_t {
  /** @brief Serialized executor callback admission. */
  Admission = 0U,
  /** @brief Native upload validation, planning, allocation, or encoding. */
  UploadPreparation = 1U,
  /** @brief One bounded host-memory upload-copy chunk. */
  UploadCopy = 2U,
  /** @brief Last semantic observation immediately before native commit. */
  NativeCommit = 3U,
};

/**
 * @brief Copied observational diagnostics for one registered executor.
 *
 * @throws Nothing for value construction, copying, and destruction.
 * @note Counters grant no native resource or `ResourceLedger` authority.
 */
struct DeviceExecutorDiagnostics final {
  /** @brief Device label owned by the observed executor. */
  Device device = Device::CPU;

  /** @brief Whether the executor owns a usable native command queue. */
  bool queue_ready = false;

  /**
   * @brief Calls that reached the concrete executor admission boundary.
   *
   * The counter advances after the thread-local executor-identity guard and
   * before a call waits for serialized callback entry. It includes calls whose
   * callback later throws, never decreases, and never wraps. Same-executor
   * callback re-entry is rejected before this counter advances. A saturated
   * executor rejects the next submission explicitly. A deadline rejection
   * after reaching concrete admission advances only this counter and never
   * `invocation_count`.
   */
  std::uint64_t submission_count = 0U;

  /**
   * @brief Calls that crossed serialized callback admission.
   *
   * The counter includes callbacks that later return or throw.
   * `submission_count - invocation_count` is the number of submitted calls
   * that have not yet crossed callback admission; a serialized executor
   * exposes those calls as queued waiters while another callback is active.
   */
  std::uint64_t invocation_count = 0U;

  /** @brief Cumulative invocation-owned native allocations. */
  std::uint64_t total_allocations = 0U;

  /** @brief Allocations retained by the currently active invocation. */
  std::uint64_t live_allocations = 0U;

  /** @brief Persistent executor-owned compute pipeline entries. */
  std::uint64_t pipeline_cache_entries = 0U;
};

/**
 * @brief Stack-bounded callback entered by one matching device executor.
 *
 * @throws `run()` propagates provider exceptions unchanged.
 * @note The registry and executor borrow this object only for `execute()`.
 */
class DeviceExecutorInvocation {
 public:
  /**
   * @brief Releases a concrete stack invocation.
   * @throws Nothing under the concrete invocation contract.
   */
  virtual ~DeviceExecutorInvocation() = default;

  /**
   * @brief Enters the owned provider callback exactly once.
   * @return Nothing.
   * @throws Any provider exception unchanged.
   */
  virtual void run() = 0;

  /**
   * @brief Returns the service-owned device resource authority.
   * @return Stable ledger borrowed through synchronous executor entry.
   * @throws Nothing.
   * @note Executors may mint plan reservations and transfer exact leases to
   * native/completion owners. The invocation itself owns no ledger capacity.
   */
  virtual ResourceLedger& resource_ledger() noexcept = 0;

  /**
   * @brief Returns exact asynchronous completion lineage when available.
   * @return Run/task seed for repository-owned native publication, or nullopt
   * for diagnostics and direct executor tests without a ComputeRun.
   * @throws Standard validation exceptions from concrete seed construction.
   * @note The default preserves source-private direct invocations. A missing
   * seed forbids publishing a reusable asynchronous Value replica.
   */
  virtual std::optional<DeviceCompletionSeed> completion_seed() const {
    return std::nullopt;
  }

  /**
   * @brief Returns the exclusive absolute execution deadline when constrained.
   * @return One steady-clock point shared by every executor checkpoint, or
   * nullopt for ordinary invocations without a deadline.
   * @throws Nothing.
   * @note The default preserves unconstrained operation invocations. Concrete
   * callers MUST NOT construct a relative fallback or refresh this point.
   */
  virtual std::optional<std::chrono::steady_clock::time_point>
  execution_deadline() const noexcept {
    return std::nullopt;
  }

  /**
   * @brief Samples monotonic time at one deadline checkpoint.
   * @param checkpoint Semantic executor boundary being observed.
   * @return Current steady-clock time.
   * @throws Nothing.
   * @note Production invocations use the default clock. Source-private tests
   * may override the observation to make an exact tie deterministic; the
   * returned value never changes the absolute deadline supplied by
   * `execution_deadline()`.
   */
  virtual std::chrono::steady_clock::time_point observe_execution_time(
      DeviceExecutorDeadlineCheckpoint checkpoint) const noexcept {
    (void)checkpoint;
    return std::chrono::steady_clock::now();
  }
};

/**
 * @brief Owns physical resources for one non-CPU device label.
 *
 * @throws Concrete execution and diagnostics operations document native,
 * allocation, and synchronization failures.
 * @note Implementations are process-domain resources and own no Run, Graph,
 * ready queue, or completion route. They borrow the invocation ledger and may
 * transfer exact device leases only to native/completion lifetime owners.
 * Synchronous callback entry is non-reentrant for the same executor object.
 */
class DeviceExecutor {
 public:
  /**
   * @brief Releases native resources after service workers have joined.
   * @throws Nothing.
   */
  virtual ~DeviceExecutor() noexcept = default;

  /**
   * @brief Returns the fixed non-CPU device label.
   * @return Immutable executor device.
   * @throws Nothing.
   */
  virtual Device device() const noexcept = 0;

  /**
   * @brief Runs one borrowed invocation inside the native executor scope.
   * @param invocation Stack-bounded callback borrowed until return.
   * @return Nothing.
   * @throws std::logic_error before concrete admission or diagnostic-counter
   * mutation when a callback synchronously re-enters this exact executor on
   * the current thread.
   * @throws std::overflow_error before callback entry when a monotonic
   * diagnostic counter is exhausted.
   * @throws std::runtime_error when an invocation-supplied exclusive absolute
   * deadline is observed at serialized admission or a concrete native
   * checkpoint.
   * @throws Provider, synchronization, or native executor failures unchanged.
   * @note Calls through a different executor object remain permitted. The
   * thread-local identity guard is restored after normal return or any
   * exception. Implementations invoke `run()` exactly once and create no
   * second ready/completion queue. Submission diagnostics advance before
   * waiting for serialized callback admission. Implementations use the same
   * invocation-supplied absolute point throughout and never create a relative
   * fallback.
   */
  void execute(DeviceExecutorInvocation& invocation);

  /**
   * @brief Copies thread-safe observational executor diagnostics.
   * @return Value snapshot containing no native handles.
   * @throws std::system_error when synchronization fails.
   * @note A waiting submission remains observable while another callback is
   * active; implementations MUST NOT require that callback to return before
   * copying the snapshot. Observation is not synchronized with destruction.
   */
  virtual DeviceExecutorDiagnostics diagnostics() const = 0;

 protected:
  /**
   * @brief Runs one invocation after exact-executor re-entry validation.
   * @param invocation Stack-bounded callback borrowed until return.
   * @return Nothing.
   * @throws Concrete admission, provider, synchronization, allocation, or
   * native executor failures unchanged.
   * @note Only `execute()` calls this hook. Same-executor callback re-entry has
   * already been rejected, so concrete submission counters may advance here.
   */
  virtual void execute_impl(DeviceExecutorInvocation& invocation) = 0;
};

/**
 * @brief Fixed registry of process-owned non-CPU device executors.
 *
 * Composition builds and validates the registry before moving it into
 * `ExecutionService`. The service exposes no mutation surface after that move.
 *
 * @throws std::bad_alloc when executor ownership or copied device inventories
 * cannot allocate.
 * @note This class is source-private, move-only, and contains no singleton.
 */
class DeviceExecutorRegistry final {
 public:
  /**
   * @brief Creates an empty platform-neutral registry.
   * @throws std::bad_alloc when shared residency ownership cannot allocate.
   */
  DeviceExecutorRegistry();

  /**
   * @brief Releases all registered executors.
   * @throws Nothing.
   */
  ~DeviceExecutorRegistry() noexcept = default;

  /**
   * @brief Transfers complete executor ownership.
   * @param other Registry whose fixed slots move into this value.
   * @throws Nothing.
   */
  DeviceExecutorRegistry(DeviceExecutorRegistry&& other) noexcept = default;

  /**
   * @brief Replaces this unobserved registry by moving another value.
   * @param other Registry whose fixed slots move into this value.
   * @return This registry.
   * @throws Nothing.
   * @note Callers use assignment only during single-threaded composition.
   */
  DeviceExecutorRegistry& operator=(DeviceExecutorRegistry&& other) noexcept =
      default;

  /**
   * @brief Prevents copying native executor ownership.
   * @param other Unused source because copying is forbidden.
   * @throws Nothing because this operation is deleted.
   */
  DeviceExecutorRegistry(const DeviceExecutorRegistry& other) = delete;

  /**
   * @brief Prevents assigning duplicate native executor ownership.
   * @param other Unused source because assignment is forbidden.
   * @return No value because this operation is deleted.
   * @throws Nothing because this operation is deleted.
   */
  DeviceExecutorRegistry& operator=(const DeviceExecutorRegistry& other) =
      delete;

  /**
   * @brief Registers one complete non-CPU executor during composition.
   * @param executor Non-null executor whose ownership moves into the slot.
   * @return Nothing.
   * @throws std::invalid_argument for null, CPU, unknown, or duplicate device.
   * @note Callers complete all registrations before concurrent observation.
   */
  void register_executor(std::unique_ptr<DeviceExecutor> executor);

  /**
   * @brief Tests whether one device has a registered executor.
   * @param device Device label to inspect.
   * @return True only for a populated valid non-CPU slot.
   * @throws Nothing.
   */
  bool contains(Device device) const noexcept;

  /**
   * @brief Returns the number of registered non-CPU executors.
   * @return Populated slot count.
   * @throws Nothing.
   */
  std::size_t size() const noexcept;

  /**
   * @brief Copies registered device labels in stable preference order.
   * @return `GPU_METAL`, `GPU_CUDA`, and `ASIC_NPU` when present.
   * @throws std::bad_alloc when result storage cannot allocate.
   */
  std::vector<Device> available_devices() const;

  /**
   * @brief Dispatches one invocation to the exact registered executor.
   * @param device Required non-CPU device.
   * @param invocation Stack-bounded callback borrowed until return.
   * @return Nothing.
   * @throws std::invalid_argument when no matching executor exists.
   * @throws std::logic_error before concrete executor admission when the
   * current callback synchronously re-enters the same registered executor.
   * @throws Provider or concrete executor failures unchanged.
   * @note Nested dispatch through a different executor object is permitted;
   * the identity guard is restored after return or exception.
   */
  void execute(Device device, DeviceExecutorInvocation& invocation);

  /**
   * @brief Copies diagnostics for one exact registered executor.
   * @param device Required non-CPU device.
   * @return Thread-safe observational snapshot.
   * @throws std::invalid_argument when no matching executor exists.
   * @throws std::system_error from concrete synchronization.
   * @note A submitted call waiting for serialized callback admission is
   * visible without waiting for the active callback to return.
   */
  DeviceExecutorDiagnostics diagnostics(Device device) const;

  /**
   * @brief Returns the shared process-domain residency manager.
   * @return Non-null owner shared with repository native executors.
   * @throws Nothing.
   * @note The manager owns replica identity/publication only, never
   * device-memory or scratch ledger authority.
   */
  std::shared_ptr<ResidencyManager> residency_manager() const noexcept {
    return residency_manager_;
  }

  /**
   * @brief Observes one admitted Run generation before ready publication.
   * @param seed Representative Run/task seed for its canonical lineage.
   * @return Nothing.
   * @throws ResidencyManager synchronization or allocation errors unchanged.
   * @note Standalone lineages retain the numeric maximum. Coordinator-managed
   * lineages retain the exact generation selected by product publication, so
   * Run observation cannot change currentness.
   */
  void observe_generation(const DeviceCompletionSeed& seed);

  /**
   * @brief Preallocates one residency lineage before Graph publication.
   * @param graph_instance_id Nonzero live Graph identity scalar.
   * @param target_node_id Canonical nonnegative request target.
   * @param request_intent Canonical request intent.
   * @return Nothing.
   * @throws ResidencyManager validation, allocation, or synchronization errors.
   * @note This fallible step runs before the coordinator publication is
   * submitted, marks the lineage as coordinator-managed, and assigns no
   * current identity.
   */
  void track_lineage(std::uint64_t graph_instance_id, int target_node_id,
                     ComputeIntent request_intent);

  /**
   * @brief Publishes one pretracked residency lineage's current generation.
   * @param graph_instance_id Nonzero live Graph identity scalar.
   * @param target_node_id Canonical nonnegative request target.
   * @param request_intent Canonical request intent.
   * @param supersession_generation Nonzero exact newly current generation.
   * @return Nothing.
   * @throws Nothing; invariant or synchronization failure terminates.
   * @note This no-allocation path is called while the Graph request
   * coordinator still excludes currentness observation. Accepted-coordinate
   * ordering may make the exact managed generation move numerically backward.
   */
  void publish_current_generation(
      std::uint64_t graph_instance_id, int target_node_id,
      ComputeIntent request_intent,
      std::uint64_t supersession_generation) noexcept;

  /**
   * @brief Retires canonical generation rows after exact Graph close drain.
   * @param graph_instance_id Nonzero irreversibly closed Graph identity.
   * @return Number of lineage rows removed from the shared manager.
   * @throws std::logic_error when the shared manager is absent or a transfer
   * for this Graph remains pending.
   * @throws ResidencyManager validation or synchronization errors unchanged.
   * @note Ready resident replicas are not cleared. The caller owns the
   * Graph-close ordering proof and must prevent later generation observation.
   */
  std::size_t retire_graph_lineages(std::uint64_t graph_instance_id);

 private:
  /** @brief Number of public Device enum slots currently recognized. */
  static constexpr std::size_t kDeviceSlotCount = 4U;

  /**
   * @brief Resolves one validated enum to its fixed slot.
   * @param device Candidate public device label.
   * @return Slot index, or `kDeviceSlotCount` for an unknown value.
   * @throws Nothing.
   */
  static std::size_t slot_for(Device device) noexcept;

  /** @brief Unique executor owners indexed by stable Device value. */
  std::array<std::unique_ptr<DeviceExecutor>, kDeviceSlotCount> executors_{};

  /** @brief Replica publication authority shared by concrete executors. */
  std::shared_ptr<ResidencyManager> residency_manager_;
};

/**
 * @brief Builds the production platform device executor registry.
 * @return Registry containing Metal only when native device/queue creation
 * succeeds.
 * @throws std::bad_alloc when executor allocation fails.
 * @note Unsupported platforms and unavailable devices return an empty value.
 */
DeviceExecutorRegistry make_default_device_executor_registry();

}  // namespace ps::execution
