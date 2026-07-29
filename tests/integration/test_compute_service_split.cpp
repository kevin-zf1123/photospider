#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <opencv2/imgproc.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "adapters/opencv/buffer_adapter_opencv.hpp"
#include "benchmark/benchmark_types.hpp"
#include "compute/compute_cache_policy.hpp"
#include "compute/compute_geometry.hpp"
#include "compute/compute_metrics_recorder.hpp"
#include "compute/compute_run.hpp"
#include "compute/compute_service.hpp"
#include "compute/dirty_region_planner.hpp"
#include "compute/dirty_update_executor_test_access.hpp"
#include "compute/dirty_write_buffers.hpp"
#include "compute/downsample_executor.hpp"
#include "compute/execution_service.hpp"
#include "compute/intent_update_coordinator.hpp"
#include "compute/node_executor.hpp"
#include "compute/node_input_resolver.hpp"
#include "compute/realtime_proxy_graph.hpp"
#include "compute/task_graph_planning.hpp"
#include "compute/task_population_strategy.hpp"
#include "compute/tiled_input_normalizer.hpp"
#include "core/param_utils.hpp"
#include "core/value_image_adapter.hpp"
#include "graph/graph_cache_service.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_traversal_service.hpp"
#include "graph/roi_propagation_service.hpp"
#include "photospider/host/host.hpp"
#include "plugin/operation_host_adapter.hpp"
#include "providers/configured_image_artifact_codec.hpp"
#include "providers/configured_operation_providers.hpp"
#include "runtime/graph_event_service.hpp"
#include "runtime/graph_runtime.hpp"
#include "runtime/interaction.hpp"
#include "support/execution_service_test_access.hpp"
#include "support/fake_image_artifact_codec.hpp"
#include "support/kernel_test_access.hpp"
#include "support/kernel_test_dependencies.hpp"
#include "support/scoped_execution_graph_lifecycle.hpp"

namespace ps {
namespace {

/**
 * @brief Counts real tile invocations for the disk-cache guard regression.
 *
 * @note The counter is reset by the focused test before execution submission.
 * Any positive value means at least one tile executed after a whole-node disk
 * cache hit should have satisfied the node.
 */
std::atomic_int g_disk_cache_guard_tile_calls{0};

/**
 * @brief Counts recomputations of a producer with partial persistent validity.
 *
 * @note The owning regression resets the counter after publishing the partial
 * dirty state and reads it only after synchronous parallel settlement.
 */
std::atomic_int g_partial_cache_producer_calls{0};

/**
 * @brief Counts whole-output consumers reached after producer recomputation.
 *
 * @note Exactly one call proves dependency release occurred after the planned
 * producer task completed.
 */
std::atomic_int g_partial_cache_consumer_calls{0};

/**
 * @brief Records the first producer pixel observed by the whole-output
 * consumer.
 *
 * @note The regression distinguishes the recomputed value 5 from stale partial
 * state 91 without retaining a borrowed NodeOutput pointer.
 */
std::atomic_int g_partial_cache_consumer_observed_value{0};

/** @brief Counts selected tiled sibling callbacks in exact-metadata tests. */
std::atomic_int g_exact_sibling_tiled_calls{0};

/**
 * @brief Counts tiled callbacks whose input ROI did not cover the full source.
 * @note Exact RandomAccess metadata keeps this value zero; a generic
 * same-key metadata lookup would instead expose spatially aligned tile ROIs.
 */
std::atomic_int g_exact_sibling_input_roi_mismatches{0};

/**
 * @brief Counts monolithic sibling entries that route selection must avoid.
 * @note The operation shares a key with the selected device-tiled callback.
 */
std::atomic_int g_exact_sibling_monolithic_calls{0};

/**
 * @brief Request-visible kernel size emitted by the dynamic blur parameter op.
 * @note Tests update the value only between completed compute requests.
 */
std::atomic_int g_dynamic_blur_ksize{3};

/** @brief Counts dynamic parameter producer invocations per request. */
std::atomic_int g_dynamic_parameter_calls{0};

/** @brief Forces the dynamic parameter producer to fail during preflight. */
std::atomic_bool g_dynamic_parameter_fail{false};

/** @brief Request-visible width emitted by the dynamic extent producer. */
std::atomic_int g_dynamic_extent_width{64};

/** @brief Counts HP image-plus-parameter producer executions. */
std::atomic_int g_image_parameter_hp_calls{0};

/** @brief Counts RT-domain image producer tile executions. */
std::atomic_int g_image_parameter_rt_calls{0};

/**
 * @brief Counts HP tiles emitted by the spatial-aligned source fixture.
 * @note The owning test resets the counter before planning and reads it only
 * after synchronous compute completion.
 */
std::atomic_int g_spatial_generator_hp_calls{0};

/**
 * @brief Counts HP executions of the uncached spatial parameter fixture.
 * @note The owning test resets the counter before planning and reads it only
 * after synchronous compute completion.
 */
std::atomic_int g_spatial_parameter_hp_calls{0};

/** @brief Generation emitted by the staged-input preflight source. */
std::atomic_int g_staged_source_generation{3};

/** @brief Last source generation observed by its parameter consumer. */
std::atomic_int g_derived_parameter_seen_generation{0};

/**
 * @brief Selects a malformed tagged value for the host-preparation source.
 * @note The owning test changes the flag only between synchronous requests.
 */
std::atomic_bool g_host_preparation_emit_malformed_value{false};

/**
 * @brief Counts source callbacks that stage the connected preparation value.
 * @note One failing-request call proves preflight reached source staging.
 */
std::atomic_int g_host_preparation_source_calls{0};

/**
 * @brief Counts entries into the adapted public parameter callback.
 * @note A zero count proves effective-parameter conversion failed first.
 */
std::atomic_int g_host_preparation_plugin_calls{0};

/**
 * @brief Counts HP target tiles dispatched after connected preflight.
 * @note The owning test resets the counter immediately before each request.
 */
std::atomic_int g_host_preparation_hp_target_calls{0};

/**
 * @brief Counts RT target tiles dispatched after connected preflight.
 * @note The owning test resets the counter immediately before each request.
 */
std::atomic_int g_host_preparation_rt_target_calls{0};

/**
 * @brief Optional service observed from inside a connected provider callback.
 * @note The scoped owning test installs the pointer only around one synchronous
 * request, so the service outlives every acquire-load.
 */
std::atomic<compute::ExecutionService*> g_preflight_lifecycle_service{nullptr};

/** @brief Snapshot sequence preceding the observed dirty request. */
std::atomic<std::uint64_t> g_preflight_lifecycle_min_sequence{0U};

/** @brief Number of provider-side lifecycle observations. */
std::atomic_int g_preflight_lifecycle_observations{0};

/** @brief Whether exact current-request installation preceded provider entry.
 */
std::atomic_bool g_preflight_observed_installed_bundle{false};

/** @brief Whether reserved-start physical authority preceded provider entry. */
std::atomic_bool g_preflight_observed_reserved_start{false};

/**
 * @brief Optional request source cancelled by the host-preparation provider.
 * @note Atomic shared ownership lets the execution callback safely retain the
 * source while its synchronous owning test clears the process-global hook.
 */
std::shared_ptr<compute::ComputeRequestCancellationSource>
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    g_host_preparation_cancellation_source;

/**
 * @brief Original operation failure text used by the LastError integration
 * contract.
 *
 * @note The sentinel is intentionally stable so the test can distinguish the
 * operator's message from execution and Kernel context added around it.
 */
constexpr auto kOpFailureMessage = "split runtime parallel operation failure";

/**
 * @brief Removes one test-owned runtime directory at scope exit.
 *
 * @note The guard suppresses cleanup errors because test assertions, rather
 * than temporary-directory cleanup, own the behavioral result.
 */
class ScopedTestDirectory {
 public:
  /**
   * @brief Prepares a clean temporary directory path for one runtime test.
   *
   * @param path Unique path assigned to the current GoogleTest case.
   * @throws Nothing; stale-path removal uses the error-code overload.
   * @note GraphRuntime creates the directory lazily when the test loads or
   * constructs its graph.
   */
  explicit ScopedTestDirectory(std::filesystem::path path)
      : path_(std::move(path)) {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  /**
   * @brief Removes runtime directories created during the test.
   *
   * @throws Nothing.
   * @note Cleanup is best effort and never masks an earlier test failure.
   */
  ~ScopedTestDirectory() noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  /**
   * @brief Returns the test-owned runtime root.
   *
   * @return Immutable filesystem path borrowed from this guard.
   * @throws Nothing.
   * @note The reference remains valid for the guard's lifetime.
   */
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  /** @brief Runtime root removed when the guard leaves scope. */
  std::filesystem::path path_;
};

/**
 * @brief Installs one request source for deterministic preflight cancellation.
 *
 * The host-preparation source provider loads this shared owner immediately
 * before returning its staged output. Destruction clears the hook only after
 * the synchronous ComputeService call has settled.
 *
 * @throws Nothing from construction or destruction.
 * @note Tests must not overlap installations. The fixture owns no Run and
 * exposes no cancellation authority outside this translation unit.
 */
class ScopedHostPreparationCancellationSource final {
 public:
  /**
   * @brief Publishes the request source observed by the provider callback.
   * @param source Non-null request-owned cancellation coordinator.
   * @throws Nothing.
   */
  explicit ScopedHostPreparationCancellationSource(
      std::shared_ptr<compute::ComputeRequestCancellationSource> source) {
    std::atomic_store_explicit(&g_host_preparation_cancellation_source,
                               std::move(source), std::memory_order_release);
  }

  /**
   * @brief Clears the provider-visible request source.
   * @throws Nothing.
   */
  ~ScopedHostPreparationCancellationSource() noexcept {
    std::atomic_store_explicit(
        &g_host_preparation_cancellation_source,
        std::shared_ptr<compute::ComputeRequestCancellationSource>{},
        std::memory_order_release);
  }

  /** @brief Prevents overlapping ownership of the process-global hook. */
  ScopedHostPreparationCancellationSource(
      const ScopedHostPreparationCancellationSource&) = delete;

  /** @brief Prevents reassignment of the process-global hook owner. */
  ScopedHostPreparationCancellationSource& operator=(
      const ScopedHostPreparationCancellationSource&) = delete;
};

/**
 * @brief Installs provider-side lifecycle observation for one request.
 *
 * @throws std::bad_alloc or synchronization exceptions from the baseline
 * lifecycle snapshot.
 * @note Construction captures the prior sequence before publishing the service
 * pointer. Destruction clears the pointer after synchronous settlement.
 */
class ScopedPreflightLifecycleObservation final {
 public:
  /**
   * @brief Arms one exact service for provider-side observation.
   * @param service Service owning the request lifecycle and physical root.
   * @throws std::bad_alloc or synchronization exceptions from snapshot copy.
   */
  explicit ScopedPreflightLifecycleObservation(
      compute::ExecutionService& service) {
    const compute::ExecutionLifecyclePage before =
        service.lifecycle_snapshot(0U, 1U);
    g_preflight_lifecycle_min_sequence.store(before.snapshot_cut,
                                             std::memory_order_release);
    g_preflight_lifecycle_observations.store(0, std::memory_order_relaxed);
    g_preflight_observed_installed_bundle.store(false,
                                                std::memory_order_relaxed);
    g_preflight_observed_reserved_start.store(false, std::memory_order_relaxed);
    g_preflight_lifecycle_service.store(&service, std::memory_order_release);
  }

  /** @brief Clears provider-visible service ownership. */
  ~ScopedPreflightLifecycleObservation() noexcept {
    g_preflight_lifecycle_service.store(nullptr, std::memory_order_release);
  }

  /** @brief Prevents overlapping observer installations. */
  ScopedPreflightLifecycleObservation(
      const ScopedPreflightLifecycleObservation&) = delete;
  /** @brief Prevents replacing observer-clearing ownership. */
  ScopedPreflightLifecycleObservation& operator=(
      const ScopedPreflightLifecycleObservation&) = delete;
};

Node make_node(int id, std::string type, std::string subtype) {
  Node node;
  node.id = id;
  node.name = "split_node_" + std::to_string(id);
  node.type = std::move(type);
  node.subtype = std::move(subtype);
  return node;
}

NodeOutput make_image_output(int width, int height, int channels = 1,
                             float value = 1.0f) {
  NodeOutput output;
  output.image_buffer =
      make_aligned_cpu_image_buffer(width, height, channels, DataType::FLOAT32);
  toCvMat(output.image_buffer).setTo(value);
  return output;
}

/**
 * @brief Coordinates deterministic overlap observations inside direct dirty
 * provider callbacks.
 *
 * @throws std::system_error from mutex or condition-variable operations.
 * @note Tests arm blocking only while no callback is active, release every
 * waiter before joining futures, and retain this object through callback-owned
 * shared pointers until registry cleanup.
 */
class DirectDirtyProviderProbe final {
 public:
  /**
   * @brief Resets counters and blocks subsequent provider entries.
   * @return Nothing.
   * @throws std::system_error from locking.
   * @note Callers invoke this only after all initialization computes settle.
   */
  void reset_and_block() {
    std::lock_guard<std::mutex> lock(mutex_);
    entered_ = 0;
    active_ = 0;
    maximum_active_ = 0;
    released_ = false;
  }

  /**
   * @brief Records one provider interval and waits for test release.
   * @return Nothing after decrementing the active callback count.
   * @throws std::system_error from locking or waiting.
   * @note The callback notifies waiters after incrementing both entry and
   * active counts. Test release is monotonic for the armed interval.
   */
  void enter() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++entered_;
    ++active_;
    maximum_active_ = std::max(maximum_active_, active_);
    condition_.notify_all();
    condition_.wait(lock, [this] { return released_; });
    --active_;
    condition_.notify_all();
  }

  /**
   * @brief Waits until at least the requested callbacks have entered.
   * @param expected Minimum cumulative entry count.
   * @param timeout Maximum bounded wait.
   * @return True when the count is reached before timeout.
   * @throws std::system_error from locking or waiting.
   */
  bool wait_for_entries(int expected, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(
        lock, timeout, [this, expected] { return entered_ >= expected; });
  }

  /**
   * @brief Releases every provider waiting in the current armed interval.
   * @return Nothing.
   * @throws std::system_error from locking.
   */
  void release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

  /**
   * @brief Returns the largest simultaneous provider count.
   * @return Maximum active callbacks observed since reset.
   * @throws std::system_error from locking.
   */
  int maximum_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return maximum_active_;
  }

  /**
   * @brief Returns the cumulative provider entry count.
   * @return Entries observed since reset.
   * @throws std::system_error from locking.
   */
  int entered() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entered_;
  }

 private:
  /** @brief Protects all counters and the monotonic release flag. */
  mutable std::mutex mutex_;
  /** @brief Announces provider entry and release transitions. */
  std::condition_variable condition_;
  /** @brief Cumulative callbacks entered since the last reset. */
  int entered_ = 0;
  /** @brief Callbacks currently inside the provider interval. */
  int active_ = 0;
  /** @brief High-water mark for active provider intervals. */
  int maximum_active_ = 0;
  /** @brief Whether the current blocked interval may complete. */
  bool released_ = true;
};

/**
 * @brief Coordinates the sequential-provider and post-provider lease boundary.
 *
 * One sequential callback blocks inside provider entry while a second request
 * reaches the same implementation/key admission gate. After the test releases
 * the sequential callback, the injected cache encoder blocks and ultimately
 * throws, providing a deterministic Host post-processing interval.
 *
 * @throws std::system_error from mutex and condition-variable operations.
 * @note The owning test releases both waits before joining asynchronous work
 * and unregisters both callback slots before this shared probe is destroyed.
 */
class SequentialLeaseBoundaryProbe final {
 public:
  /**
   * @brief Enters and blocks the sequential provider interval.
   * @return Nothing after the owning test releases provider execution.
   * @throws std::system_error from locking or waiting.
   * @note The active count remains nonzero for the complete provider callback
   * interval and excludes later Host normalization or cache persistence.
   */
  void enter_sequential_provider() {
    std::unique_lock<std::mutex> lock(mutex_);
    sequential_provider_entered_ = true;
    ++active_providers_;
    maximum_active_providers_ =
        std::max(maximum_active_providers_, active_providers_);
    condition_.notify_all();
    condition_.wait(lock, [this] { return sequential_provider_released_; });
    --active_providers_;
    condition_.notify_all();
  }

  /**
   * @brief Records one route-backed provider interval.
   * @return Nothing after atomically recording entry and exit.
   * @throws std::system_error from locking.
   * @note A maximum active count above one would prove the shared exclusive key
   * failed to protect provider execution.
   */
  void enter_route_provider() {
    std::lock_guard<std::mutex> lock(mutex_);
    route_provider_entered_ = true;
    ++active_providers_;
    maximum_active_providers_ =
        std::max(maximum_active_providers_, active_providers_);
    condition_.notify_all();
    --active_providers_;
    route_provider_exited_ = true;
    condition_.notify_all();
  }

  /**
   * @brief Publishes one test-product operation admission denial.
   * @param context Non-null SequentialLeaseBoundaryProbe instance.
   * @param implementation_identity Exact implementation rejected by the gate.
   * @return Nothing.
   * @throws Nothing.
   * @note The service pool mutex is held, so this callback uses atomics plus
   * condition-variable notification and never calls service code or locks the
   * fixture mutex.
   */
  static void observe_operation_admission_wait(
      void* context, std::uint64_t implementation_identity) noexcept {
    auto* probe = static_cast<SequentialLeaseBoundaryProbe*>(context);
    probe->waited_implementation_identity_.store(implementation_identity,
                                                 std::memory_order_relaxed);
    probe->operation_admission_waited_.store(true, std::memory_order_release);
    probe->condition_.notify_all();
  }

  /**
   * @brief Blocks Host cache persistence and then injects a typed failure.
   * @return No value because the configured failure is always thrown.
   * @throws GraphError with GraphErrc::Io after the test releases persistence.
   * @throws std::system_error from locking or waiting.
   * @note Entry occurs only after the sequential provider has returned and its
   * result has been normalized and published into request-local Graph state.
   */
  [[noreturn]] void block_post_provider_cache_and_fail() {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      post_provider_cache_entered_ = true;
      condition_.notify_all();
      condition_.wait(lock, [this] { return post_provider_cache_released_; });
    }
    throw GraphError(GraphErrc::Io,
                     "injected sequential post-provider cache failure");
  }

  /**
   * @brief Waits for sequential provider entry.
   * @param timeout Maximum bounded wait.
   * @return True when the sequential provider entered before the deadline.
   * @throws std::system_error from locking or waiting.
   */
  bool wait_for_sequential_provider(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout,
                               [this] { return sequential_provider_entered_; });
  }

  /**
   * @brief Waits for route-backed provider entry.
   * @param timeout Maximum bounded wait.
   * @return True when the route-backed provider entered before the deadline.
   * @throws std::system_error from locking or waiting.
   */
  bool wait_for_route_provider(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout,
                               [this] { return route_provider_entered_; });
  }

  /**
   * @brief Waits until the contender reaches gate denial or enters erroneously.
   * @param timeout Maximum bounded wait.
   * @return True when either observable event occurs before the deadline.
   * @throws std::system_error from locking or waiting.
   * @note The disjunction makes a gate-disabled mutant fail immediately on
   * provider entry instead of timing out while still proving correct execution
   * reached the blocking admission point.
   */
  bool wait_for_admission_or_route_provider(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] {
      return operation_admission_waited_.load(std::memory_order_acquire) ||
             route_provider_entered_;
    });
  }

  /**
   * @brief Waits for route-backed provider callback exit.
   * @param timeout Maximum bounded wait.
   * @return True after the provider interval ends before the deadline.
   * @throws std::system_error from locking or waiting.
   */
  bool wait_for_route_provider_exit(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout,
                               [this] { return route_provider_exited_; });
  }

  /**
   * @brief Waits for Host post-provider cache persistence.
   * @param timeout Maximum bounded wait.
   * @return True when the injected encoder entered before the deadline.
   * @throws std::system_error from locking or waiting.
   */
  bool wait_for_post_provider_cache(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout,
                               [this] { return post_provider_cache_entered_; });
  }

  /**
   * @brief Releases the blocked sequential provider.
   * @return Nothing.
   * @throws std::system_error from locking.
   */
  void release_sequential_provider() {
    std::lock_guard<std::mutex> lock(mutex_);
    sequential_provider_released_ = true;
    condition_.notify_all();
  }

  /**
   * @brief Releases blocked cache persistence so its injected failure settles.
   * @return Nothing.
   * @throws std::system_error from locking.
   */
  void release_post_provider_cache() {
    std::lock_guard<std::mutex> lock(mutex_);
    post_provider_cache_released_ = true;
    condition_.notify_all();
  }

  /**
   * @brief Returns the maximum overlapping provider intervals.
   * @return High-water mark for sequential plus route provider execution.
   * @throws std::system_error from locking.
   */
  int maximum_active_providers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return maximum_active_providers_;
  }

  /**
   * @brief Reports whether the test-product gate observer saw denial.
   * @return True after one operation admission attempt was denied.
   * @throws Nothing.
   */
  bool operation_admission_waited() const noexcept {
    return operation_admission_waited_.load(std::memory_order_acquire);
  }

  /**
   * @brief Returns the exact implementation identity denied by the gate.
   * @return Nonzero identity after operation_admission_waited() becomes true.
   * @throws Nothing.
   */
  std::uint64_t waited_implementation_identity() const noexcept {
    return waited_implementation_identity_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Reports whether the route-backed provider has entered.
   * @return True after the route callback begins.
   * @throws std::system_error from locking.
   */
  bool route_provider_entered() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return route_provider_entered_;
  }

 private:
  /** @brief Serializes every transition and provider overlap counter. */
  mutable std::mutex mutex_;
  /** @brief Announces provider, cache, and release transitions. */
  std::condition_variable condition_;
  /** @brief True after the sequential provider enters. */
  bool sequential_provider_entered_ = false;
  /** @brief True after the owning test releases the sequential provider. */
  bool sequential_provider_released_ = false;
  /** @brief True after the route-backed provider enters. */
  bool route_provider_entered_ = false;
  /** @brief True after the route-backed provider exits. */
  bool route_provider_exited_ = false;
  /** @brief True while the injected cache encoder is waiting. */
  bool post_provider_cache_entered_ = false;
  /** @brief True after the owning test releases cache persistence. */
  bool post_provider_cache_released_ = false;
  /** @brief Number of callbacks currently inside provider entry. */
  int active_providers_ = 0;
  /** @brief Largest simultaneous provider-entry count. */
  int maximum_active_providers_ = 0;
  /** @brief Whether a contender reached a denied operation admission check. */
  std::atomic_bool operation_admission_waited_{false};
  /** @brief Exact implementation identity observed at the denied gate. */
  std::atomic<std::uint64_t> waited_implementation_identity_{0U};
};

/**
 * @brief Owns one isolated operation-admission observer installation.
 *
 * @throws Nothing from destruction.
 * @note The observed asynchronous work must settle before this guard retires.
 * The underlying free-function seam exists only in the non-installed internal
 * test product and changes no production ExecutionService object layout.
 */
class ScopedOperationAdmissionWaitObservation final {
 public:
  /**
   * @brief Installs one observer for the supplied isolated service.
   * @param service Test-product service that will deny the contender.
   * @param probe Fixture receiving allocation-free notification.
   * @throws Nothing.
   */
  ScopedOperationAdmissionWaitObservation(
      compute::ExecutionService& service,
      SequentialLeaseBoundaryProbe& probe) noexcept
      : service_(service) {
    testing::ExecutionServiceTestAccess::set_operation_admission_wait_observer(
        service_,
        &SequentialLeaseBoundaryProbe::observe_operation_admission_wait,
        &probe);
  }

  /**
   * @brief Clears the process-local observer.
   * @throws Nothing.
   */
  ~ScopedOperationAdmissionWaitObservation() noexcept {
    testing::ExecutionServiceTestAccess::
        clear_operation_admission_wait_observer(service_);
  }

  /** @brief Prevents duplicate observer-clearing ownership. */
  ScopedOperationAdmissionWaitObservation(
      const ScopedOperationAdmissionWaitObservation&) = delete;

  /** @brief Prevents replacing observer-clearing ownership. */
  ScopedOperationAdmissionWaitObservation& operator=(
      const ScopedOperationAdmissionWaitObservation&) = delete;

 private:
  /** @brief Isolated service used to document observer ownership. */
  compute::ExecutionService& service_;
};

/**
 * @brief Owns one test-product dirty post-plan observer installation.
 *
 * @throws std::bad_alloc when copying the observer cannot allocate.
 * @note The hook is thread-local and active only for the lexical synchronous
 * compute call. Destruction clears the test-product seam before captured
 * registry state or counters can retire.
 */
class ScopedDirtyPostPlanObservation final {
 public:
  /**
   * @brief Installs one callback at the plan/revalidation boundary.
   * @param observer Registry mutation or observation to perform once notified.
   * @throws std::bad_alloc when callback ownership cannot allocate.
   */
  explicit ScopedDirtyPostPlanObservation(std::function<void()> observer)
      : observer_(std::move(observer)),
        hook_{this, &ScopedDirtyPostPlanObservation::notify} {
    compute::testing::set_dirty_post_plan_test_hook(&hook_);
  }

  /**
   * @brief Clears the thread-local observer.
   * @throws Nothing.
   */
  ~ScopedDirtyPostPlanObservation() noexcept {
    compute::testing::set_dirty_post_plan_test_hook(nullptr);
  }

  /** @brief Prevents duplicate ownership of the thread-local observer. */
  ScopedDirtyPostPlanObservation(const ScopedDirtyPostPlanObservation&) =
      delete;

  /** @brief Prevents replacing observer-clearing ownership. */
  ScopedDirtyPostPlanObservation& operator=(
      const ScopedDirtyPostPlanObservation&) = delete;

 private:
  /**
   * @brief Dispatches the test-product callback through the opaque context.
   * @param context Non-null ScopedDirtyPostPlanObservation instance.
   * @return Nothing.
   * @throws Any exception selected by the installed observer.
   */
  static void notify(void* context) {
    static_cast<ScopedDirtyPostPlanObservation*>(context)->observer_();
  }

  /** @brief Owned callback invoked by the borrowed test hook. */
  std::function<void()> observer_;
  /** @brief Borrowed test-product hook installed for this lexical scope. */
  compute::testing::DirtyPostPlanTestHook hook_;
};

/**
 * @brief Builds a monolithic image callback observed by one blocking probe.
 * @param probe Shared deterministic provider observer.
 * @param value Pixel value emitted after release.
 * @return Callback suitable for one HP scalar registry slot.
 * @throws std::bad_alloc when callback ownership cannot allocate.
 * @note Output extent comes from node width/height parameters and defaults to
 * eight pixels in each dimension.
 */
MonolithicOpFunc make_probed_image_operation(
    std::shared_ptr<DirectDirtyProviderProbe> probe, float value) {
  return [probe = std::move(probe), value](
             const Node& node,
             const std::vector<const NodeOutput*>&) -> NodeOutput {
    probe->enter();
    return make_image_output(as_int_flexible(node.parameters, "width", 8),
                             as_int_flexible(node.parameters, "height", 8), 1,
                             value);
  };
}

/**
 * @brief Builds a tiled image callback observed by one blocking probe.
 * @param probe Shared deterministic provider observer.
 * @param value Pixel value written after release.
 * @return Callback suitable for one RT scalar registry slot.
 * @throws std::bad_alloc when callback ownership cannot allocate.
 * @note One 8x8 dirty fixture produces one provider entry per active task.
 */
TileOpFunc make_probed_tile_operation(
    std::shared_ptr<DirectDirtyProviderProbe> probe, float value) {
  return
      [probe = std::move(probe), value](const Node&, const OutputTile& output,
                                        const std::vector<InputTile>&) {
        probe->enter();
        toCvMat(output).setTo(value);
      };
}

/**
 * @brief Builds a data-only connected-parameter callback with probe entry.
 * @param probe Shared deterministic provider observer.
 * @return Monolithic callback publishing radius seven after release.
 * @throws std::bad_alloc when output-map storage cannot allocate.
 */
MonolithicOpFunc make_probed_parameter_operation(
    std::shared_ptr<DirectDirtyProviderProbe> probe) {
  return [probe = std::move(probe)](
             const Node&, const std::vector<const NodeOutput*>&) -> NodeOutput {
    probe->enter();
    NodeOutput output;
    output.data["radius"] = 7;
    return output;
  };
}

/**
 * @brief Owns one direct ComputeService graph while sharing process authority.
 *
 * @throws Allocation, codec, lifecycle-registration, or service exceptions
 * from construction.
 * @note Each harness owns independent graph/cache/event state. Only the
 * injected ExecutionService is shared, making cross-Graph gate observations
 * attributable to process authority rather than graph-local serialization.
 */
class DirectDirtyComputeHarness final {
 public:
  /**
   * @brief Creates one registered empty graph and direct service facade.
   * @param authority Shared process execution authority.
   * @param graph_name Unique graph/cache diagnostic name.
   * @throws std::bad_alloc when Graph, cache, or lifecycle storage cannot
   * allocate.
   * @throws GraphError or codec exceptions when configured dependencies or
   * lifecycle registration cannot initialize.
   * @note Member declaration order keeps every service dependency alive until
   * after the facade and lifecycle registration retire.
   */
  DirectDirtyComputeHarness(compute::ExecutionService& authority,
                            std::string graph_name)
      : graph_("cache/" + std::move(graph_name)),
        cache_(providers::make_configured_image_artifact_codec(),
               testing::make_yaml_cache_metadata_codec()),
        service_(traversal_, cache_, events_, authority),
        lifecycle_(authority, graph_) {}

  /**
   * @brief Returns mutable graph ownership for fixture population.
   * @return Borrowed graph that remains valid for this harness lifetime.
   * @throws Nothing.
   */
  GraphModel& graph() noexcept { return graph_; }

  /**
   * @brief Returns the bound direct ComputeService facade.
   * @return Borrowed facade sharing the harness Graph and process authority.
   * @throws Nothing.
   */
  ComputeService& service() noexcept { return service_; }

 private:
  /** @brief Independent graph and visible dirty state. */
  GraphModel graph_;
  /** @brief Independent topology service. */
  GraphTraversalService traversal_;
  /** @brief Independent disk/memory cache service. */
  GraphCacheService cache_;
  /** @brief Independent compute event sink. */
  GraphEventService events_;
  /** @brief Direct facade borrowing all preceding owners. */
  ComputeService service_;
  /** @brief Explicit graph lifecycle row in shared process authority. */
  testing::ScopedExecutionGraphLifecycle lifecycle_;
};

/** @brief Shared subtype for an unprobed complete image dependency. */
constexpr const char* kDirectCachedSourceSubtype = "cached_image_source";

/**
 * @brief Registers the input-free monolithic dependency used by tiled tests.
 * @return Nothing.
 * @throws Registry allocation or callback-copy exceptions unchanged.
 * @note The owning test unregisters this key after all synchronous work
 * settles. Keeping it distinct from the target prevents a tiled RT callback
 * from being selected for the input-free source.
 */
void register_direct_cached_source_operation() {
  OpRegistry::instance().register_op_hp_monolithic(
      "issue82_direct_dirty", kDirectCachedSourceSubtype,
      MonolithicOpFunc([](const Node& node,
                          const std::vector<const NodeOutput*>&) {
        return make_image_output(as_int_flexible(node.parameters, "width", 8),
                                 as_int_flexible(node.parameters, "height", 8),
                                 1, 1.0f);
      }));
}

/**
 * @brief Populates one input-free image node for direct dirty execution.
 * @param graph Empty graph receiving the node.
 * @param subtype Registered operation subtype.
 * @param seed_cache Whether to install a complete reusable 8x8 HP output.
 * @param connect_cached_source Whether to add a complete upstream image input
 * required by tiled execution.
 * @return Nothing.
 * @throws Graph topology or image allocation exceptions unchanged.
 */
void populate_direct_dirty_graph(GraphModel& graph, const std::string& subtype,
                                 bool seed_cache = false,
                                 bool connect_cached_source = false) {
  Node node = make_node(1, "issue82_direct_dirty", subtype);
  node.parameters["width"] = 8;
  node.parameters["height"] = 8;
  if (connect_cached_source) {
    Node source =
        make_node(0, "issue82_direct_dirty", kDirectCachedSourceSubtype);
    source.parameters["width"] = 8;
    source.parameters["height"] = 8;
    source.cached_output_high_precision = make_image_output(8, 8, 1, 1.0f);
    source.hp_region =
        RegionSet::from_image_rect({image_region_domain(), 0, 8, 0, 8});
    source.hp_version = 1;
    graph.add_node(source);
    node.image_inputs.push_back({0, "image"});
  }
  if (seed_cache) {
    node.cached_output_high_precision = make_image_output(8, 8, 1, 1.0f);
    node.hp_region =
        RegionSet::from_image_rect({image_region_domain(), 0, 8, 0, 8});
    node.hp_version = 1;
  }
  graph.add_node(node);
  graph.validate_topology();
}

/**
 * @brief Populates one data producer connected to an image target.
 * @param graph Empty graph receiving parameter and target nodes.
 * @param parameter_subtype Registered probed producer subtype.
 * @param target_subtype Registered unprobed image target subtype.
 * @param seed_cache Whether to seed both nodes with complete committed output.
 * @return Nothing.
 * @throws Graph topology or image/data allocation exceptions unchanged.
 * @note Dirty target requests execute the producer during connected preflight;
 * phase-two target work remains unobserved by the producer probe.
 */
void populate_direct_preflight_graph(GraphModel& graph,
                                     const std::string& parameter_subtype,
                                     const std::string& target_subtype,
                                     bool seed_cache = false) {
  Node parameter = make_node(1, "issue82_direct_preflight", parameter_subtype);
  Node target = make_node(2, "issue82_direct_preflight", target_subtype);
  target.parameters["width"] = 8;
  target.parameters["height"] = 8;
  target.parameters["radius"] = 0;
  target.parameter_inputs.push_back({1, "radius", "radius"});
  if (seed_cache) {
    NodeOutput parameter_output;
    parameter_output.data["radius"] = 7;
    parameter.cached_output_high_precision = std::move(parameter_output);
    parameter.hp_version = 1;
    target.cached_output_high_precision = make_image_output(8, 8, 1, 1.0f);
    target.hp_region =
        RegionSet::from_image_rect({image_region_domain(), 0, 8, 0, 8});
    target.hp_version = 1;
  }
  graph.add_node(parameter);
  graph.add_node(target);
  graph.validate_topology();
}

/**
 * @brief Creates one focused nonparallel dirty request.
 * @param intent HP or RT dirty domain.
 * @param target_node_id Graph-local target node.
 * @return Request with disk cache disabled and a one-pixel dirty ROI.
 * @throws Nothing.
 */
ComputeService::Request make_direct_dirty_request(ComputeIntent intent,
                                                  int target_node_id) {
  ComputeService::Request request;
  request.node_id = target_node_id;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  request.intent = intent;
  request.dirty_roi = PixelRect{0, 0, 1, 1};
  return request;
}

/**
 * @brief Asserts that one direct process authority retained no failed Run.
 * @param authority ExecutionService whose lifecycle and ledger must be empty.
 * @return Nothing.
 * @throws std::bad_alloc or synchronization exceptions from snapshot copy.
 * @note A succeeding retry in each caller separately proves identity/key gate
 * ownership was released; this helper checks the remaining visible lifecycle
 * and physical-resource residue.
 */
void expect_direct_authority_settled(
    const compute::ExecutionService& authority) {
  const compute::ExecutionLifecyclePage lifecycle =
      authority.lifecycle_snapshot(0U, 4096U);
  EXPECT_EQ(lifecycle.counters.pending_candidate_count, 0U);
  EXPECT_EQ(lifecycle.counters.admitted_standalone_run_count, 0U);
  EXPECT_EQ(lifecycle.counters.admitted_run_group_count, 0U);
  EXPECT_EQ(lifecycle.counters.live_root_reservation_count, 0U);
  EXPECT_EQ(authority.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Recomputes one complete producer output for a Whole dependency.
 *
 * @param node Producer identity used in dependency diagnostics.
 * @param inputs Exactly one complete image dependency.
 * @return Complete same-shaped image filled with the stable value 5.
 * @throws GraphError when the required image dependency is missing.
 * @throws std::invalid_argument or std::bad_alloc when output allocation fails.
 * @throws std::runtime_error or cv::Exception when CPU image adaptation or
 * filling fails.
 * @note The callback count is the primary witness that partial persistent HP
 * state did not suppress the planned producer task.
 */
NodeOutput execute_partial_cache_producer(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  if (inputs.size() != 1U || inputs.front() == nullptr) {
    throw GraphError(
        GraphErrc::MissingDependency,
        "partial-cache producer requires one image input: " + node.name);
  }
  g_partial_cache_producer_calls.fetch_add(1, std::memory_order_relaxed);
  return make_image_output(inputs.front()->image_buffer.width,
                           inputs.front()->image_buffer.height,
                           inputs.front()->image_buffer.channels, 5.0f);
}

/**
 * @brief Captures and republishes a producer value at a whole-read boundary.
 *
 * @param node Consumer identity used in dependency diagnostics.
 * @param inputs Exactly one complete producer image.
 * @return Complete same-shaped image filled with the observed first pixel.
 * @throws GraphError when the required producer dependency is missing.
 * @throws std::invalid_argument or std::bad_alloc when input adaptation or
 * output allocation fails.
 * @throws std::runtime_error or cv::Exception when CPU pixel access or filling
 * fails.
 * @note Seeing 91 would expose the stale exact-partial producer output; seeing
 * 5 proves the producer recomputed before dependency release.
 */
NodeOutput execute_partial_cache_consumer(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  if (inputs.size() != 1U || inputs.front() == nullptr) {
    throw GraphError(
        GraphErrc::MissingDependency,
        "partial-cache consumer requires one image input: " + node.name);
  }
  const ImageBuffer& input = inputs.front()->image_buffer;
  const float observed = toCvMat(input).at<float>(0, 0);
  g_partial_cache_consumer_observed_value.store(static_cast<int>(observed),
                                                std::memory_order_release);
  g_partial_cache_consumer_calls.fetch_add(1, std::memory_order_relaxed);
  return make_image_output(input.width, input.height, input.channels, observed);
}

/**
 * @brief Emits the image and connected value used by host-preparation tests.
 *
 * @param node Source node providing the requested output extent.
 * @param inputs Image inputs, which must remain empty for this generator.
 * @return Image output plus either an Int64 or a deliberately mismatched String
 * under `injected`.
 * @throws GraphError when the input-free generator receives an image input.
 * @throws std::bad_alloc when parameter/output storage fails.
 * @throws std::invalid_argument when image-buffer allocation rejects shape.
 * @throws std::runtime_error or cv::Exception when CPU pixels cannot be filled.
 * @throws Exceptions from request cancellation cleanup callbacks unchanged.
 * @note The mismatched String remains a valid ParameterValue; the exact public
 * Int64 accessor is the intended failure. When the test-only request source is
 * installed, cancellation is requested after output production and directly
 * before provider return.
 */
NodeOutput execute_host_preparation_source(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  if (!inputs.empty()) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "host-preparation source received an image input: " + node.name);
  }
  g_host_preparation_source_calls.fetch_add(1, std::memory_order_relaxed);
  if (compute::ExecutionService* observed_service =
          g_preflight_lifecycle_service.load(std::memory_order_acquire)) {
    const compute::ExecutionLifecyclePage lifecycle =
        observed_service->lifecycle_snapshot(0U, 4096U);
    const std::uint64_t minimum_sequence =
        g_preflight_lifecycle_min_sequence.load(std::memory_order_acquire);
    const bool current_bundle_admitted = std::any_of(
        lifecycle.records.begin(), lifecycle.records.end(),
        [minimum_sequence](const compute::ExecutionLifecycleEvent& event) {
          return event.sequence > minimum_sequence &&
                 event.kind ==
                     compute::ExecutionLifecycleEventKind::BundleAdmitted;
        });
    const bool installed =
        current_bundle_admitted &&
        lifecycle.counters.pending_candidate_count == 0U &&
        (lifecycle.counters.admitted_standalone_run_count != 0U ||
         lifecycle.counters.admitted_run_group_count != 0U);
    const bool reserved_start =
        lifecycle.counters.entered_callback_count != 0U &&
        lifecycle.counters.live_root_reservation_count != 0U &&
        observed_service->resource_snapshot().reserved != ResourceVector{};
    g_preflight_observed_installed_bundle.store(installed,
                                                std::memory_order_release);
    g_preflight_observed_reserved_start.store(reserved_start,
                                              std::memory_order_release);
    g_preflight_lifecycle_observations.fetch_add(1, std::memory_order_release);
  }
  NodeOutput output = make_image_output(
      as_int_flexible(node.parameters, "width", 64),
      as_int_flexible(node.parameters, "height", 16), 1, 4.0f);
  if (g_host_preparation_emit_malformed_value.load(std::memory_order_acquire)) {
    output.data["injected"] = "not-an-integer";
  } else {
    output.data["injected"] = 1;
  }
  const std::shared_ptr<compute::ComputeRequestCancellationSource>
      cancellation_source = std::atomic_load_explicit(
          &g_host_preparation_cancellation_source, std::memory_order_acquire);
  if (cancellation_source) {
    (void)cancellation_source->request_cancellation();
  }
  return output;
}

/**
 * @brief Produces a data-only radius through the public operation contract.
 *
 * @param node Public callback identity with copied effective parameters.
 * @param inputs Source image/data views prepared by the host adapter.
 * @return Data-only output containing radius 1.
 * @throws std::bad_alloc when public output-map storage cannot grow.
 * @throws plugin::ParameterTypeError when `injected` is not exact Int64.
 * @note The callback increments its entry count first, then exercises the
 * public exact accessor before publishing its output.
 */
plugin::OperationOutput execute_host_preparation_parameter(
    const plugin::NodeView& node,
    plugin::ArrayView<plugin::OperationInputView> inputs) {
  (void)inputs;
  g_host_preparation_plugin_calls.fetch_add(1, std::memory_order_relaxed);
  const plugin::ParameterValue* injected = node.find_parameter("injected");
  if (injected == nullptr) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "host-preparation parameter is missing injected");
  }
  (void)injected->as_int64();
  plugin::OperationOutput output;
  output.data.emplace("radius", plugin::ParameterValue(std::int64_t{1}));
  return output;
}

/**
 * @brief Emits one HP target tile after connected-parameter preflight.
 * @param node Target identity retained for the private callback contract.
 * @param output Writable HP tile.
 * @param inputs Destination-indexed input tiles.
 * @return Nothing.
 * @throws std::invalid_argument or std::runtime_error from CPU adaptation.
 * @throws cv::Exception when filling the output tile fails.
 * @note The count is a direct phase-two dispatch witness; preflight never
 * executes the target node itself.
 */
void execute_host_preparation_hp_target_tile(
    const Node& node, const OutputTile& output,
    const std::vector<InputTile>& inputs) {
  (void)node;
  (void)inputs;
  g_host_preparation_hp_target_calls.fetch_add(1, std::memory_order_relaxed);
  toCvMat(output).setTo(5.0f);
}

/**
 * @brief Emits one RT target tile after connected-parameter preflight.
 * @param node Target identity retained for the private callback contract.
 * @param output Writable RT tile.
 * @param inputs Destination-indexed input tiles.
 * @return Nothing.
 * @throws std::invalid_argument or std::runtime_error from CPU adaptation.
 * @throws cv::Exception when filling the output tile fails.
 * @note The counter distinguishes RT phase-two work from its HP sibling.
 */
void execute_host_preparation_rt_target_tile(
    const Node& node, const OutputTile& output,
    const std::vector<InputTile>& inputs) {
  (void)node;
  (void)inputs;
  g_host_preparation_rt_target_calls.fetch_add(1, std::memory_order_relaxed);
  toCvMat(output).setTo(6.0f);
}

/**
 * @brief Emits one input-free RT source tile for the valid control request.
 * @param node Source identity retained for the private callback contract.
 * @param output Writable RT source tile.
 * @param inputs Image inputs, which must remain empty for this generator.
 * @return Nothing.
 * @throws GraphError when an input is supplied unexpectedly.
 * @throws std::bad_alloc when diagnostics cannot allocate.
 * @throws std::invalid_argument or std::runtime_error from CPU adaptation.
 * @throws cv::Exception when filling the output tile fails.
 * @note This callback makes the RT graph valid when preflight preparation
 * succeeds; it is not expected to run in the injected-failure request.
 */
void execute_host_preparation_rt_source_tile(
    const Node& node, const OutputTile& output,
    const std::vector<InputTile>& inputs) {
  if (!inputs.empty()) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "host-preparation RT source received an image input: " + node.name);
  }
  toCvMat(output).setTo(7.0f);
}

/**
 * @brief Emits one deterministic tile for an input-free image generator.
 *
 * @param node Generator node whose identity is owned by the test graph.
 * @param output_tile Writable HP tile selected by task-graph planning.
 * @param input_tiles Image inputs, which must remain empty for this generator.
 * @return Nothing.
 * @throws GraphError when the generator unexpectedly receives an image input.
 * @throws std::bad_alloc when diagnostic text cannot allocate.
 * @throws std::invalid_argument when the output channel description is invalid.
 * @throws std::runtime_error when the output tile has no writable CPU payload.
 * @throws cv::Exception when the CPU output tile cannot be adapted or filled.
 * @note The callback increments its counter only after validating the
 * input-free generator boundary.
 */
void execute_spatial_generator_tile(const Node& node,
                                    const OutputTile& output_tile,
                                    const std::vector<InputTile>& input_tiles) {
  if (!input_tiles.empty()) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "spatial test generator received an image input: " + node.name);
  }
  g_spatial_generator_hp_calls.fetch_add(1, std::memory_order_relaxed);
  toCvMat(output_tile).setTo(3.0f);
}

/**
 * @brief Emits the uncached radius consumed by the spatial-aligned fixture.
 *
 * @param node Parameter producer whose callback requires no image input.
 * @param inputs Image inputs, which must remain empty for this producer.
 * @return Parameter-only output containing radius 7.
 * @throws GraphError when the producer unexpectedly receives an image input.
 * @throws std::bad_alloc when output data storage cannot allocate.
 * @note The callback count proves execution was not bypassed by a cache hit.
 */
NodeOutput execute_spatial_parameter_source(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  if (!inputs.empty()) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "spatial test parameter source received an image input: " + node.name);
  }
  g_spatial_parameter_hp_calls.fetch_add(1, std::memory_order_relaxed);
  NodeOutput output;
  output.data["radius"] = 7;
  return output;
}

/**
 * @brief Writes the connected request-local radius into every output pixel.
 *
 * @param node Execution snapshot whose effective runtime parameters must
 * contain the connected radius.
 * @param output_tile Writable HP tile used as the operation-visible witness.
 * @param input_tiles Destination-indexed input tiles supplied by the executor.
 * @return Nothing.
 * @throws GraphError when the effective radius is absent.
 * @throws plugin::ParameterTypeError when radius is not an exact Int64.
 * @throws std::invalid_argument or std::runtime_error from CPU adaptation.
 * @throws cv::Exception when the output tile cannot be filled.
 * @note The owning regression keeps the graph's static radius at zero and
 * connects radius seven, so output pixels equal to seven prove that the
 * operation received the request-local parameter overlay.
 */
void execute_request_local_parameter_probe_tile(
    const Node& node, const OutputTile& output_tile,
    const std::vector<InputTile>& input_tiles) {
  (void)input_tiles;
  const auto radius = node.runtime_parameters.find("radius");
  if (radius == node.runtime_parameters.end()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "request-local parameter probe is missing radius");
  }
  toCvMat(output_tile).setTo(static_cast<double>(radius->second.as_int64()));
}

/**
 * @brief Executes the device-tiled sibling and validates exact RandomAccess
 * ROI.
 *
 * @param node Target node retained for diagnostic identity.
 * @param output_tile Writable tile selected from the exact implementation.
 * @param input_tiles Destination-indexed input views supplied by the executor.
 * @return Nothing.
 * @throws std::invalid_argument or std::runtime_error from CPU adaptation.
 * @throws cv::Exception when output filling fails.
 * @note The callback records, rather than asserts, ROI mismatch so execution
 * can settle before the owning test examines every provider invocation.
 */
void execute_exact_sibling_metadata_tile(
    const Node& node, const OutputTile& output_tile,
    const std::vector<InputTile>& input_tiles) {
  (void)node;
  g_exact_sibling_tiled_calls.fetch_add(1, std::memory_order_relaxed);
  bool full_input =
      input_tiles.size() == 1U && input_tiles.front().buffer != nullptr;
  if (full_input) {
    const ImageBuffer& input = *input_tiles.front().buffer;
    full_input =
        input_tiles.front().roi == (PixelRect{0, 0, input.width, input.height});
  }
  if (!full_input) {
    g_exact_sibling_input_roi_mismatches.fetch_add(1,
                                                   std::memory_order_relaxed);
  }
  toCvMat(output_tile).setTo(13.0f);
}

/**
 * @brief Maps every output tile to the complete first image-input extent.
 *
 * @param node Downstream node retained for the propagator contract.
 * @param roi Output ROI whose exact shape is intentionally ignored.
 * @param graph Graph retained for the propagator contract.
 * @param output_extent Complete downstream output extent.
 * @param input_extents Destination-indexed upstream image extents.
 * @param parameters Effective request parameters.
 * @param available_inputs Optional current input outputs.
 * @return Full first-input rectangle, or roi when no usable extent exists.
 * @throws Nothing.
 * @note The owning sibling implementation marks itself RandomAccess, so both
 * dependency lowering and provider tile construction must invoke this mapping.
 */
PixelRect propagate_exact_sibling_full_input(
    const Node& node, const PixelRect& roi, const GraphModel& graph,
    const PixelSize& output_extent, const std::vector<PixelSize>& input_extents,
    const plugin::ParameterMap& parameters,
    const std::vector<const NodeOutput*>* available_inputs) {
  (void)node;
  (void)graph;
  (void)output_extent;
  (void)parameters;
  (void)available_inputs;
  if (input_extents.empty() || input_extents.front().width <= 0 ||
      input_extents.front().height <= 0) {
    return roi;
  }
  return PixelRect{0, 0, input_extents.front().width,
                   input_extents.front().height};
}

/**
 * @brief Registers deterministic split-test operations once per process.
 *
 * The registration set supplies HP/RT source, tiled, random-access, cache,
 * and deliberate-failure behaviors used by planning and runtime integration
 * tests in this target.
 *
 * @return Nothing.
 * @throws std::bad_alloc when registry or callback storage cannot allocate.
 * @throws Any registry exception unchanged; std::call_once retries a later
 * invocation when registration does not complete.
 * @note OpRegistry is process-global, so callbacks and metadata remain valid
 * until process shutdown and must use stable operation keys. The dynamic blur
 * parameter source is deliberately nonreentrant so direct-preflight failure
 * tests also prove gate release and retry.
 */
void register_split_ops() {
  static std::once_flag once;
  std::call_once(once, [] {
    providers::register_configured_operation_providers();
    auto& registry = OpRegistry::instance();
    registry.register_op_hp_monolithic(
        "split_plan", "source",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>&) {
              return make_image_output(
                  as_int_flexible(node.parameters, "width", 64),
                  as_int_flexible(node.parameters, "height", 64));
            }));
    registry.register_op_hp_monolithic(
        "split_plan", "parameter_source",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              NodeOutput output;
              output.data["radius"] = 7;
              return output;
            }));
    registry.register_op_hp_monolithic(
        "split_plan", "spatial_uncached_parameter_source",
        MonolithicOpFunc(execute_spatial_parameter_source));
    OpMetadata dynamic_parameter_metadata;
    dynamic_parameter_metadata.reentrant = false;
    registry.register_op_hp_monolithic(
        "split_plan", "dynamic_blur_parameter",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              g_dynamic_parameter_calls.fetch_add(1, std::memory_order_relaxed);
              if (g_dynamic_parameter_fail.load(std::memory_order_acquire)) {
                throw GraphError(GraphErrc::ComputeError,
                                 "dynamic parameter preflight failure");
              }
              NodeOutput output;
              output.data["ksize"] =
                  g_dynamic_blur_ksize.load(std::memory_order_acquire);
              return output;
            }),
        dynamic_parameter_metadata);
    registry.register_op_hp_monolithic(
        "split_plan", "dynamic_extent_parameter",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              NodeOutput output;
              output.data["width"] =
                  g_dynamic_extent_width.load(std::memory_order_acquire);
              return output;
            }));
    registry.register_op_hp_monolithic(
        "split_plan", "dynamic_extent_target",
        MonolithicOpFunc([](const Node& node,
                            const std::vector<const NodeOutput*>&) {
          const plugin::ParameterMap& parameters =
              node.runtime_parameters.empty() ? node.parameters
                                              : node.runtime_parameters;
          return make_image_output(as_int_flexible(parameters, "width", 0),
                                   as_int_flexible(parameters, "height", 0));
        }));
    registry.register_op_hp_monolithic(
        "image_generator", "split_image_parameter_source",
        MonolithicOpFunc([](const Node&,
                            const std::vector<const NodeOutput*>&) {
          g_image_parameter_hp_calls.fetch_add(1, std::memory_order_relaxed);
          NodeOutput output = make_image_output(64, 16, 1, 3.0f);
          output.data["radius"] = 1;
          return output;
        }));
    registry.register_op_hp_monolithic(
        "split_plan", "gradient_source",
        MonolithicOpFunc([](const Node& node,
                            const std::vector<const NodeOutput*>&) {
          NodeOutput output = make_image_output(
              as_int_flexible(node.parameters, "width", 320),
              as_int_flexible(node.parameters, "height", 64), 1, 0.0f);
          cv::Mat pixels = toCvMat(output.image_buffer);
          for (int y = 0; y < pixels.rows; ++y) {
            for (int x = 0; x < pixels.cols; ++x) {
              pixels.at<float>(y, x) = ((x / 5 + y / 3) % 2 == 0) ? 0.0f : 1.0f;
            }
          }
          return output;
        }));
    registry.register_op_hp_monolithic(
        "split_plan", "staged_generation_source",
        MonolithicOpFunc([](const Node&,
                            const std::vector<const NodeOutput*>&) {
          const int generation =
              g_staged_source_generation.load(std::memory_order_acquire);
          NodeOutput output = make_image_output(320, 64, 1, 0.0f);
          cv::Mat pixels = toCvMat(output.image_buffer);
          for (int y = 0; y < pixels.rows; ++y) {
            for (int x = 0; x < pixels.cols; ++x) {
              pixels.at<float>(y, x) =
                  static_cast<float>(((x / 5 + y / 3 + generation) % 7) / 6.0);
            }
          }
          output.data["generation"] = generation;
          return output;
        }));
    registry.register_op_hp_monolithic(
        "split_plan", "derived_blur_parameter",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>& inputs) {
              if (inputs.empty() || !inputs.front()) {
                throw GraphError(GraphErrc::MissingDependency,
                                 "derived parameter requires source input");
              }
              const int generation = static_cast<int>(
                  inputs.front()->data.at("generation").as_int64());
              g_derived_parameter_seen_generation.store(
                  generation, std::memory_order_release);
              NodeOutput output;
              output.data["ksize"] = generation;
              return output;
            }));
    registry.register_op_hp_monolithic(
        "split_plan", "partial_cache_producer",
        MonolithicOpFunc(execute_partial_cache_producer));
    registry.register_op_hp_monolithic(
        "split_plan", "partial_cache_consumer",
        MonolithicOpFunc(execute_partial_cache_consumer));
    registry.register_op_hp_monolithic(
        "image_generator", "host_preparation_source",
        MonolithicOpFunc(execute_host_preparation_source));
    registry.register_op_hp_monolithic("split_plan",
                                       "host_preparation_parameter",
                                       plugin_host::adapt_monolithic_operation(
                                           execute_host_preparation_parameter));
    registry.register_op_hp_monolithic(
        "split_plan", "monolithic",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>& inputs) {
              if (inputs.empty())
                throw GraphError(GraphErrc::MissingDependency, "missing input");
              return make_image_output(inputs.front()->image_buffer.width,
                                       inputs.front()->image_buffer.height);
            }));
    ps::OpMetadata micro_meta;
    micro_meta.tile_preference = ps::TileSizePreference::MICRO;
    ps::OpMetadata macro_meta;
    macro_meta.tile_preference = ps::TileSizePreference::MACRO;
    registry.register_op_hp_tiled(
        "split_plan", "tile",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>&) {
          toCvMat(output_tile).setTo(2.0f);
        }),
        micro_meta);
    registry.register_op_hp_tiled(
        "split_plan", "request_local_parameter_probe",
        TileOpFunc(execute_request_local_parameter_probe_tile), micro_meta);
    registry.register_op_hp_tiled(
        "image_generator", "spatial_uncached_tiled_source",
        TileOpFunc(execute_spatial_generator_tile), micro_meta);
    registry.register_op_hp_tiled(
        "split_plan", "host_preparation_target",
        TileOpFunc(execute_host_preparation_hp_target_tile), micro_meta);
    registry.register_op_rt_tiled(
        "split_plan", "tile",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>&) {
          toCvMat(output_tile).setTo(2.0f);
        }),
        micro_meta);
    registry.register_op_rt_tiled(
        "image_generator", "split_image_parameter_source",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>& input_tiles) {
          EXPECT_TRUE(input_tiles.empty())
              << "the RT generator must remain an input-free source boundary";
          g_image_parameter_rt_calls.fetch_add(1, std::memory_order_relaxed);
          toCvMat(output_tile).setTo(8.0f);
        }),
        micro_meta);
    registry.register_op_rt_tiled(
        "image_generator", "host_preparation_source",
        TileOpFunc(execute_host_preparation_rt_source_tile), micro_meta);
    registry.register_op_rt_tiled(
        "split_plan", "host_preparation_target",
        TileOpFunc(execute_host_preparation_rt_target_tile), micro_meta);
    registry.register_op_hp_tiled(
        "split_plan", "domain_tile",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>&) {
          toCvMat(output_tile).setTo(6.0f);
        }),
        macro_meta);
    registry.register_op_rt_tiled(
        "split_plan", "domain_tile",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>&) {
          toCvMat(output_tile).setTo(6.0f);
        }),
        micro_meta);
    registry.register_op_hp_tiled(
        "image_process", "gaussian_blur_dependency_test",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>&) {
          toCvMat(output_tile).setTo(4.0f);
        }),
        micro_meta);
    ps::OpMetadata random_meta = micro_meta;
    random_meta.access_pattern =
        ps::OpMetadata::InputAccessPattern::RandomAccess;
    ps::OpMetadata exact_sibling_monolithic_meta;
    exact_sibling_monolithic_meta.tile_preference =
        ps::TileSizePreference::UNDEFINED;
    exact_sibling_monolithic_meta.access_pattern =
        ps::OpMetadata::InputAccessPattern::SpatialAligned;
    registry.register_op_hp_monolithic(
        "split_plan", "exact_sibling_metadata",
        MonolithicOpFunc([](const Node& node,
                            const std::vector<const NodeOutput*>& inputs) {
          g_exact_sibling_monolithic_calls.fetch_add(1,
                                                     std::memory_order_relaxed);
          const int width = inputs.empty() || !inputs.front()
                                ? as_int_flexible(node.parameters, "width", 1)
                                : inputs.front()->image_buffer.width;
          const int height = inputs.empty() || !inputs.front()
                                 ? as_int_flexible(node.parameters, "height", 1)
                                 : inputs.front()->image_buffer.height;
          return make_image_output(width, height, 1, 17.0f);
        }),
        exact_sibling_monolithic_meta);
    ps::OpMetadata exact_sibling_tiled_meta = micro_meta;
    exact_sibling_tiled_meta.access_pattern =
        ps::OpMetadata::InputAccessPattern::RandomAccess;
    registry.register_impl("split_plan", "exact_sibling_metadata", Device::CPU,
                           TileOpFunc(execute_exact_sibling_metadata_tile),
                           exact_sibling_tiled_meta);
    registry.register_op_hp_tiled(
        "split_plan", "random_tile",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>&) {
          toCvMat(output_tile).setTo(5.0f);
        }),
        random_meta);
    ps::OpMetadata rt_random_meta = micro_meta;
    rt_random_meta.access_pattern =
        ps::OpMetadata::InputAccessPattern::RandomAccess;
    registry.register_op_hp_tiled(
        "split_plan", "domain_random_tile",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>&) {
          toCvMat(output_tile).setTo(7.0f);
        }),
        macro_meta);
    registry.register_op_rt_tiled(
        "split_plan", "domain_random_tile",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>&) {
          toCvMat(output_tile).setTo(7.0f);
        }),
        rt_random_meta);
    registry.register_op_hp_tiled(
        "split_plan", "disk_cache_guard_tile",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>&) {
          g_disk_cache_guard_tile_calls.fetch_add(1, std::memory_order_relaxed);
          toCvMat(output_tile).setTo(11.0f);
        }),
        micro_meta);
    registry.register_op_hp_tiled("split_plan", "parallel_failure",
                                  TileOpFunc([](const Node&, const OutputTile&,
                                                const std::vector<InputTile>&) {
                                    throw std::runtime_error(kOpFailureMessage);
                                  }),
                                  micro_meta);
    registry.register_dirty_propagator(
        "split_plan", "random_tile",
        DirtyRoiPropFunc([](const Node&, const PixelRect& roi,
                            const GraphModel&, const PixelSize&,
                            const std::vector<PixelSize>&,
                            const plugin::ParameterMap& parameters,
                            const std::vector<const NodeOutput*>*) {
          const auto found = parameters.find("radius");
          const int radius = found == parameters.end()
                                 ? 16
                                 : static_cast<int>(found->second.as_int64());
          return compute::expand_rect(roi, radius);
        }));
    registry.register_dirty_propagator(
        "split_plan", "exact_sibling_metadata",
        DirtyRoiPropFunc(propagate_exact_sibling_full_input));
    registry.register_dirty_propagator(
        "split_plan", "domain_random_tile",
        DirtyRoiPropFunc([](const Node&, const PixelRect& roi,
                            const GraphModel&, const PixelSize&,
                            const std::vector<PixelSize>&,
                            const plugin::ParameterMap& parameters,
                            const std::vector<const NodeOutput*>*) {
          const auto found = parameters.find("radius");
          const int radius = found == parameters.end()
                                 ? 16
                                 : static_cast<int>(found->second.as_int64());
          return compute::expand_rect(roi, radius);
        }));
  });
}

/**
 * @brief Verifies that a compute event stream contains every required source.
 *
 * @param events Runtime event snapshot drained through InteractionService.
 * @param required_sources Stable source labels expected from the production
 * coordinator and node executors.
 * @return True when each required source occurs at least once.
 * @throws Nothing directly; string comparison does not allocate.
 * @note Event ordering is asserted separately by tests whose contract depends
 * on RT-before-HP inline coordination.
 */
bool contains_event_sources(
    const std::vector<ComputeEventSnapshot>& events,
    std::initializer_list<const char*> required_sources) {
  return std::all_of(
      required_sources.begin(), required_sources.end(),
      [&](const char* required_source) {
        return std::any_of(
            events.begin(), events.end(),
            [&](const auto& event) { return event.source == required_source; });
      });
}

compute::FullTaskGraph expand_full_task_graph(GraphModel& graph,
                                              ComputeIntent intent) {
  compute::FullTaskGraphExpander expander;
  return expander.expand(graph, intent);
}

compute::ComputePlan node_cache_pruned_plan(
    GraphModel& graph, const compute::ComputeRequest& request,
    const std::vector<int>& execution_order) {
  compute::NodeCacheTaskGraphPruner pruner;
  return pruner.prune(expand_full_task_graph(graph, request.intent), request,
                      execution_order, graph);
}

compute::ComputePlan dirty_snapshot_pruned_plan(
    const compute::ComputePlan& node_cache_plan,
    const compute::DirtyRegionSnapshot& snapshot, GraphModel& graph) {
  compute::DirtySnapshotTaskGraphPruner pruner;
  return pruner.prune(node_cache_plan, snapshot, graph);
}

/**
 * @brief Populates a deterministic gradient→Gaussian graph with a connected
 * kernel-size producer.
 *
 * @param graph Empty graph receiving source, parameter, and blur nodes.
 * @return Nothing.
 * @throws GraphError or allocation/YAML exceptions from graph construction.
 * @note The 320-pixel width crosses both 16-pixel dirty tiles and the
 * built-in Gaussian HP macro-task boundary at x=256.
 */
void populate_dynamic_blur_graph(GraphModel& graph) {
  Node source = make_node(1, "split_plan", "gradient_source");
  source.parameters["width"] = 320;
  source.parameters["height"] = 64;
  Node parameter = make_node(3, "split_plan", "dynamic_blur_parameter");
  parameter.parameters["width"] = 1;
  parameter.parameters["height"] = 1;
  Node blur = make_node(2, "image_process", "gaussian_blur");
  blur.parameters["width"] = 320;
  blur.parameters["height"] = 64;
  blur.parameters["ksize"] = 3;
  blur.image_inputs.push_back({1, "image"});
  blur.parameter_inputs.push_back({3, "ksize", "ksize"});
  graph.add_node(source);
  graph.add_node(parameter);
  graph.add_node(blur);
  graph.validate_topology();
}

/**
 * @brief Populates a valid source→parameter→target preparation chain.
 *
 * @param graph Empty graph receiving the image/data source, adapted public
 * parameter callback, and HP/RT tiled target.
 * @return Nothing.
 * @throws GraphError or allocation exceptions from graph construction.
 * @note The parameter node consumes the source through both image and named
 * parameter edges. Preflight must therefore stage the source and copy its
 * `injected` ParameterValue into the effective map before callback entry.
 */
void populate_host_preparation_failure_graph(GraphModel& graph) {
  Node source = make_node(1, "image_generator", "host_preparation_source");
  source.parameters["width"] = 64;
  source.parameters["height"] = 16;
  Node parameter = make_node(3, "split_plan", "host_preparation_parameter");
  parameter.parameters["width"] = 1;
  parameter.parameters["height"] = 1;
  parameter.parameters["injected"] = 1;
  parameter.image_inputs.push_back({1, "image"});
  parameter.parameter_inputs.push_back({1, "injected", "injected"});
  Node target = make_node(2, "split_plan", "host_preparation_target");
  target.parameters["width"] = 64;
  target.parameters["height"] = 16;
  target.parameters["radius"] = 1;
  target.image_inputs.push_back({1, "image"});
  target.parameter_inputs.push_back({3, "radius", "radius"});
  graph.add_node(source);
  graph.add_node(parameter);
  graph.add_node(target);
  graph.validate_topology();
}

/**
 * @brief Populates an image target whose width comes from a connected value.
 * @param graph Empty graph receiving source, parameter, and target nodes.
 * @throws GraphError or allocation/YAML exceptions from graph construction.
 * @note The target starts at width 64 so tests can shrink it below a dirty ROI
 * that was valid only for the previous generation.
 */
void populate_dynamic_extent_graph(GraphModel& graph) {
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 64;
  source.parameters["height"] = 8;
  Node parameter = make_node(3, "split_plan", "dynamic_extent_parameter");
  parameter.parameters["width"] = 1;
  parameter.parameters["height"] = 1;
  Node target = make_node(2, "split_plan", "dynamic_extent_target");
  target.parameters["width"] = 64;
  target.parameters["height"] = 8;
  target.image_inputs.push_back({1, "image"});
  target.parameter_inputs.push_back({3, "width", "width"});
  graph.add_node(source);
  graph.add_node(parameter);
  graph.add_node(target);
  graph.validate_topology();
}

/**
 * @brief Populates A(image+data) to B(parameter) to C(random-access blur).
 * @param graph Empty graph receiving the staged preflight chain.
 * @throws GraphError or allocation/YAML exceptions from graph construction.
 * @note B consumes A as an image edge while C consumes the same A image and
 * B's named value, forcing preflight and phase two to share one A snapshot.
 */
void populate_staged_parameter_chain(GraphModel& graph) {
  Node source = make_node(1, "split_plan", "staged_generation_source");
  source.parameters["width"] = 320;
  source.parameters["height"] = 64;
  Node parameter = make_node(3, "split_plan", "derived_blur_parameter");
  parameter.parameters["width"] = 1;
  parameter.parameters["height"] = 1;
  parameter.image_inputs.push_back({1, "image"});
  Node blur = make_node(2, "image_process", "gaussian_blur");
  blur.parameters["width"] = 320;
  blur.parameters["height"] = 64;
  blur.parameters["ksize"] = 3;
  blur.image_inputs.push_back({1, "image"});
  blur.parameter_inputs.push_back({3, "ksize", "ksize"});
  graph.add_node(source);
  graph.add_node(parameter);
  graph.add_node(blur);
  graph.validate_topology();
}

}  // namespace

TEST(ComputeGeometrySplit, CoversClippingAlignmentScalingMergingAndHalo) {
  using compute::align_rect;
  using compute::calculate_halo;
  using compute::clip_rect;
  using compute::is_rect_empty;
  using compute::merge_rect;
  using compute::scale_down_rect;
  using compute::scale_down_size;
  using compute::scale_up_rect;
  using compute::translate_rect;

  EXPECT_TRUE(is_rect_empty((PixelRect{0, 0, 0, 5})));
  EXPECT_EQ(clip_rect((PixelRect{-5, 2, 12, 10}), (PixelSize{10, 8})),
            (PixelRect{0, 2, 7, 6}));
  EXPECT_EQ(align_rect((PixelRect{5, 6, 10, 11}), 8),
            (PixelRect{0, 0, 16, 24}));
  EXPECT_EQ(merge_rect((PixelRect{2, 3, 4, 5}), (PixelRect{10, 1, 2, 4})),
            (PixelRect{2, 1, 10, 7}));
  EXPECT_EQ(scale_down_size((PixelSize{65, 33}), 4), (PixelSize{17, 9}));
  EXPECT_EQ(scale_down_rect((PixelRect{3, 5, 10, 11}), 4),
            (PixelRect{0, 1, 4, 3}));
  EXPECT_EQ(scale_up_rect((PixelRect{2, 3, 4, 5}), 4),
            (PixelRect{8, 12, 16, 20}));
  EXPECT_EQ(calculate_halo((PixelRect{4, 4, 8, 8}), 3, (PixelSize{14, 20})),
            (PixelRect{1, 1, 13, 14}));
  EXPECT_TRUE(is_rect_empty(translate_rect(
      (PixelRect{1, 1, 2, 2}), std::numeric_limits<std::int64_t>::max(), 0)));
  EXPECT_TRUE(is_rect_empty(translate_rect(
      (PixelRect{1, 1, 2, 2}), 0, std::numeric_limits<std::int64_t>::min())));
}

TEST(ComputeCachePolicySplit, PreservesHpAuthorityAndRtNonAuthority) {
  Node node = make_node(1, "split", "cache");
  EXPECT_FALSE(compute::ComputeCachePolicy::has_reusable_output(node));
  EXPECT_EQ(compute::ComputeCachePolicy::reusable_output(node), nullptr);
  EXPECT_FALSE(compute::ComputeCachePolicy::select_output(
      node, compute::CacheReadMode::InteractivePreferred));

  node.cached_output_high_precision = make_image_output(8, 8);
  node.hp_region = value_image_adapter::full_node_output_region(
      *node.cached_output_high_precision);
  EXPECT_EQ(
      compute::ComputeCachePolicy::reusable_output(node)->image_buffer.width,
      8);
  EXPECT_TRUE(compute::ComputeCachePolicy::can_read_disk_cache(false, false));
  EXPECT_FALSE(compute::ComputeCachePolicy::can_read_disk_cache(true, false));
  EXPECT_FALSE(compute::ComputeCachePolicy::can_read_disk_cache(false, true));

  auto selected = compute::ComputeCachePolicy::select_output(
      node, compute::CacheReadMode::InteractivePreferred);
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(*selected, &*node.cached_output_high_precision)
      << "node-level interactive mode now degrades to HP; RT lives in proxy";
}

TEST(NodeInputResolverSplit,
     ClonesParametersTransfersInputsAndReportsMissingData) {
  GraphModel graph("cache/split-input-resolver");
  Node parent = make_node(10, "split", "parent");
  parent.cached_output_high_precision = make_image_output(12, 7);
  parent.hp_region = value_image_adapter::full_node_output_region(
      *parent.cached_output_high_precision);
  parent.cached_output_high_precision->data["threshold"] = 42;
  graph.add_node(parent);

  Node child = make_node(20, "split", "child");
  child.parameters["threshold"] = 1;
  child.parameter_inputs.push_back({10, "threshold", "threshold"});
  child.image_inputs.push_back({10, "image"});

  auto resolved = compute::NodeInputResolver::resolve(
      child,
      [&](int upstream_id) -> const NodeOutput* {
        return compute::ComputeCachePolicy::reusable_output(
            graph.node(upstream_id));
      },
      "resolver test");

  ASSERT_EQ(resolved.image_inputs.size(), 1u);
  EXPECT_EQ(child.runtime_parameters.at("threshold").as_int64(), 42);
  EXPECT_EQ(child.parameters.at("threshold").as_int64(), 1)
      << "runtime parameter cloning must not mutate static parameters";
  ASSERT_TRUE(child.last_input_size_hp.has_value());
  EXPECT_EQ(*child.last_input_size_hp, (PixelSize{12, 7}));

  Node missing_named_output = child;
  missing_named_output.parameter_inputs[0].from_output_name = "missing";
  EXPECT_THROW(compute::NodeInputResolver::resolve(
                   missing_named_output,
                   [&](int) -> const NodeOutput* {
                     return &*graph.node(10).cached_output_high_precision;
                   },
                   "resolver test"),
               GraphError);

  Node missing_image = child;
  EXPECT_THROW(
      compute::NodeInputResolver::resolve(
          missing_image, [&](int) -> const NodeOutput* { return nullptr; },
          "resolver test"),
      GraphError);
}

TEST(NodeExecutorSplit,
     SharesMonolithicTiledMixingRandomAccessAndExceptionWrapping) {
  GraphModel graph("cache/split-node-executor");

  Node mono = make_node(1, "split_exec", "mono");
  OpRegistry::OpVariant mono_op = MonolithicOpFunc(
      [](const Node&, const std::vector<const NodeOutput*>& inputs) {
        return make_image_output(inputs.front()->image_buffer.width,
                                 inputs.front()->image_buffer.height);
      });
  std::vector<const NodeOutput*> mono_inputs;
  NodeOutput mono_input = make_image_output(5, 3);
  mono_inputs.push_back(&mono_input);
  NodeOutput mono_output =
      compute::NodeExecutor::execute(graph, mono, mono_op, mono_inputs);
  EXPECT_EQ(mono_output.image_buffer.width, 5);
  EXPECT_EQ(mono_output.image_buffer.height, 3);

  Node tiled = make_node(2, "image_mixing", "tile");
  bool saw_normalized_second_input = false;
  bool saw_normalized_second_spatial = false;
  int tiled_calls = 0;
  std::set<const ImageBuffer*> normalized_second_buffers;
  OpRegistry::OpVariant tile_op =
      TileOpFunc([&](const Node&, const OutputTile& output_tile,
                     const std::vector<InputTile>& input_tiles) {
        ASSERT_EQ(input_tiles.size(), 2u);
        ASSERT_NE(input_tiles[1].buffer, nullptr);
        ++tiled_calls;
        normalized_second_buffers.insert(input_tiles[1].buffer);
        saw_normalized_second_input = input_tiles[1].buffer->width == 8 &&
                                      input_tiles[1].buffer->height == 8 &&
                                      input_tiles[1].buffer->channels == 3;
        saw_normalized_second_spatial =
            input_tiles[1].spatial != nullptr &&
            input_tiles[1].spatial->absolute_roi == (PixelRect{3, 4, 4, 4});
        toCvMat(output_tile).setTo(3.0f);
      });
  NodeOutput base = make_image_output(8, 8, 3);
  NodeOutput secondary = make_image_output(4, 4, 1);
  secondary.data["normalization_marker"] = 17;
  secondary.space.absolute_roi = (PixelRect{3, 4, 4, 4});
  secondary.plugin_library_lifetime = std::make_shared<int>(42);
  std::vector<const NodeOutput*> tiled_inputs{&base, &secondary};
  const compute::TiledInputContext normalized_context =
      compute::TiledInputNormalizer::normalize(tiled, tiled_inputs);
  ASSERT_EQ(normalized_context.normalized_storage.size(), 1u);
  EXPECT_EQ(normalized_context.normalized_storage.front()
                .data.at("normalization_marker")
                .as_int64(),
            17);
  EXPECT_EQ(normalized_context.normalized_storage.front().space.absolute_roi,
            secondary.space.absolute_roi);
  EXPECT_EQ(
      normalized_context.normalized_storage.front().plugin_library_lifetime,
      secondary.plugin_library_lifetime);
  compute::TiledExecutionConfig tiled_config;
  tiled_config.tile_size = 4;
  NodeOutput tiled_output = compute::NodeExecutor::execute(
      graph, tiled, tile_op, tiled_inputs, tiled_config);
  EXPECT_TRUE(saw_normalized_second_input);
  EXPECT_TRUE(saw_normalized_second_spatial);
  EXPECT_EQ(tiled_calls, 4);
  ASSERT_EQ(normalized_second_buffers.size(), 1u);
  EXPECT_NE(*normalized_second_buffers.begin(), &secondary.image_buffer);
  EXPECT_EQ(tiled_output.image_buffer.width, 8);
  EXPECT_EQ(tiled_output.image_buffer.channels, 3);

  auto& registry = OpRegistry::instance();
  registry.register_dirty_propagator(
      "split_exec", "random_tile",
      DirtyRoiPropFunc([](const Node&, const PixelRect& roi, const GraphModel&,
                          const PixelSize&, const std::vector<PixelSize>&,
                          const plugin::ParameterMap&,
                          const std::vector<const NodeOutput*>*) {
        return compute::expand_rect(roi, 2);
      }));
  Node random_node = make_node(3, "split_exec", "random_tile");
  compute::TiledExecutionConfig random_config;
  random_config.metadata = OpMetadata{};
  random_config.metadata->access_pattern =
      OpMetadata::InputAccessPattern::RandomAccess;
  EXPECT_EQ(compute::NodeExecutor::input_roi_for_tile(
                graph, random_node, (PixelRect{1, 1, 4, 4}), base.image_buffer,
                random_config),
            (PixelRect{0, 0, 7, 7}));

  OpRegistry::OpVariant failing_op =
      MonolithicOpFunc([](const Node&, const std::vector<const NodeOutput*>&) {
        throw std::runtime_error("boom");
        return NodeOutput{};
      });
  EXPECT_THROW(
      compute::NodeExecutor::execute(graph, mono, failing_op, mono_inputs),
      GraphError);
}

TEST(NodeExecutorSplit,
     RandomAccessUsesExecutionLocalParametersAndAllActualInputExtents) {
  GraphModel graph("cache/split-node-executor-same-batch");
  graph.add_node(make_node(10, "split_exec", "source"));
  graph.add_node(make_node(11, "split_exec", "source"));
  graph.add_node(make_node(20, "split_exec", "parameter_source"));
  Node graph_child = make_node(12, "split_exec", "same_batch_random");
  graph_child.parameters["radius"] = 1;
  graph_child.image_inputs = {ImageInput{10, "image"}, ImageInput{11, "image"}};
  graph_child.parameter_inputs = {ParameterInput{20, "value", "radius"}};
  graph.add_node(graph_child);
  graph.validate_topology();
  graph.mutate_node_runtime_state(10, [](auto& state) {
    state.cached_output_high_precision = make_image_output(40, 20);
    state.cached_output_high_precision->data["generation"] = 1;
    state.cached_output_high_precision->space.absolute_roi =
        (PixelRect{1, 1, 40, 20});
  });

  auto exact_context_count = std::make_shared<int>(0);
  OpRegistry::instance().register_dirty_propagator(
      "split_exec", "same_batch_random",
      plugin_host::adapt_dirty_propagator(
          [exact_context_count](const plugin::RoiContext& context) {
            const plugin::ParameterValue* radius =
                context.node->find_parameter("radius");
            if (radius && radius->as_int64() == 3 &&
                context.output_extent.width == 40 &&
                context.output_extent.height == 20 &&
                context.input_edges.size() == 2 &&
                context.input_edges[0].extent.width == 40 &&
                context.input_edges[0].extent.height == 20 &&
                context.input_edges[0].has_available_input &&
                context.input_edges[0].available_input.data != nullptr &&
                context.input_edges[0]
                        .available_input.data->at("generation")
                        .as_int64() == 99 &&
                context.input_edges[0].available_input.spatial != nullptr &&
                context.input_edges[0]
                        .available_input.spatial->absolute_roi.x == 7 &&
                context.input_edges[1].extent.width == 3 &&
                context.input_edges[1].extent.height == 4) {
              ++*exact_context_count;
            }
            return context.requested_roi;
          }));

  Node execution_node = graph.node(12);
  execution_node.runtime_parameters = execution_node.parameters;
  execution_node.runtime_parameters["radius"] = 3;
  NodeOutput left = make_image_output(40, 20);
  left.data["generation"] = 99;
  left.space.absolute_roi = (PixelRect{7, 8, 40, 20});
  NodeOutput right = make_image_output(3, 4);
  const std::vector<const NodeOutput*> inputs{&left, &right};
  OpRegistry::OpVariant operation = TileOpFunc(
      [](const Node&, const OutputTile& output, const std::vector<InputTile>&) {
        toCvMat(output).setTo(1.0f);
      });
  compute::TiledExecutionConfig config;
  config.tile_size = 16;
  config.metadata = OpMetadata{};
  config.metadata->access_pattern =
      OpMetadata::InputAccessPattern::RandomAccess;

  const NodeOutput result = compute::NodeExecutor::execute(
      graph, execution_node, operation, inputs, config);

  EXPECT_EQ(result.image_buffer.width, 40);
  EXPECT_EQ(result.image_buffer.height, 20);
  EXPECT_EQ(*exact_context_count, 12)
      << "one random-access mapping per input must see the same complete "
         "same-batch snapshot";
}

TEST(NodeExecutorSplit,
     PreservesDisconnectedSlotIdentityThroughPublicTiledCallback) {
  GraphModel graph("cache/split-disconnected-public-input-slot");
  graph.add_node(make_node(11, "split_exec", "source"));
  Node graph_child = make_node(12, "split_exec", "disconnected_slot");
  graph_child.image_inputs = {ImageInput{-1, "image"}, ImageInput{11, "image"}};
  graph.add_node(graph_child);
  graph.validate_topology();

  NodeOutput connected = make_image_output(7, 5);
  Node execution_node = graph.node(12);
  const compute::ResolvedNodeInputs resolved =
      compute::NodeInputResolver::resolve(
          execution_node,
          [&](int upstream_id) -> const NodeOutput* {
            return upstream_id == 11 ? &connected : nullptr;
          },
          "disconnected slot regression");
  ASSERT_EQ(resolved.image_inputs.size(), 2u);
  EXPECT_EQ(resolved.image_inputs[0], nullptr);
  EXPECT_EQ(resolved.image_inputs[1], &connected);
  ASSERT_TRUE(execution_node.last_input_size_hp.has_value());
  EXPECT_EQ(*execution_node.last_input_size_hp, (PixelSize{7, 5}));

  bool roi_context_preserved_index = false;
  OpRegistry::instance().register_dirty_propagator(
      execution_node.type, execution_node.subtype,
      plugin_host::adapt_dirty_propagator(
          [&](const plugin::RoiContext& context) {
            roi_context_preserved_index =
                context.input_edges.size() == 1 &&
                context.input_edges[0].input_index == 1 &&
                context.input_edges[0].extent.width == 7 &&
                context.input_edges[0].extent.height == 5;
            return context.requested_roi;
          }));

  bool tiled_callback_preserved_slots = false;
  OpRegistry::OpVariant operation = plugin_host::adapt_tiled_operation(
      [&](const plugin::NodeView&, const OutputTileView& output,
          plugin::ArrayView<plugin::OperationTileInputView> inputs) {
        tiled_callback_preserved_slots =
            inputs.size() == 2 && inputs[0].tile.buffer == nullptr &&
            inputs[0].spatial == nullptr &&
            inputs[1].tile.buffer == &connected.image_buffer &&
            inputs[1].spatial != nullptr && output.buffer != nullptr;
      });
  compute::TiledExecutionConfig config;
  config.tile_size = 16;
  config.metadata = OpMetadata{};
  config.metadata->access_pattern =
      OpMetadata::InputAccessPattern::RandomAccess;
  const NodeOutput output = compute::NodeExecutor::execute(
      graph, execution_node, operation, resolved.image_inputs, config);

  EXPECT_EQ(output.image_buffer.width, 7);
  EXPECT_EQ(output.image_buffer.height, 5);
  EXPECT_TRUE(roi_context_preserved_index);
  EXPECT_TRUE(tiled_callback_preserved_slots);
}

TEST(ComputeMetricsRecorderSplit, FinalizesMetadataAndDebugStatistics) {
  NodeOutput input = make_image_output(3, 3, 1, 2.0f);
  input.space.absolute_roi = (PixelRect{5, 6, 3, 3});
  NodeOutput output = make_image_output(3, 3, 1, 4.0f);

  compute::ComputeMetricsRecorder::finalize_output_metadata(output, {&input},
                                                            true, 12.7);
  EXPECT_EQ(output.space.absolute_roi, input.space.absolute_roi);
  EXPECT_GT(output.debug.timestamp_us, 0u);
  EXPECT_EQ(output.debug.execution_time_ms, 13u);
  EXPECT_EQ(output.debug.compute_device, "CPU");
  EXPECT_FLOAT_EQ(output.debug.min_val, 4.0f);
  EXPECT_FLOAT_EQ(output.debug.max_val, 4.0f);
  EXPECT_FALSE(output.debug.has_nan);

  const std::pair<Device, const char*> backend_devices[] = {
      {Device::GPU_METAL, "GPU_METAL"},
      {Device::GPU_CUDA, "GPU_CUDA"},
      {Device::ASIC_NPU, "ASIC_NPU"},
  };
  for (const auto& [device, expected_label] : backend_devices) {
    NodeOutput backend;
    backend.image_buffer.width = 3;
    backend.image_buffer.height = 3;
    backend.image_buffer.channels = 1;
    backend.image_buffer.device = device;
    backend.image_buffer.context = std::make_shared<int>(7);
    backend.debug.min_val = 12.0;
    backend.debug.max_val = 34.0;
    compute::ComputeMetricsRecorder::finalize_output_metadata(backend, {}, true,
                                                              1.0);
    EXPECT_EQ(backend.debug.compute_device, expected_label);
    EXPECT_DOUBLE_EQ(backend.debug.min_val, 12.0);
    EXPECT_DOUBLE_EQ(backend.debug.max_val, 34.0);
  }
}

TEST(DirtyRegionPlannerSplit,
     ProducesGraphScopedSnapshotAndMonolithicEscalation) {
  register_split_ops();
  GraphModel graph("cache/split-dirty-planner");
  Node source = make_node(10, "split_plan", "tile");
  source.parameters["width"] = 128;
  source.parameters["height"] = 128;
  Node mono = make_node(42, "split_plan", "monolithic");
  mono.image_inputs.push_back({10, "image"});
  graph.add_node(source);
  graph.add_node(mono);
  graph.validate_topology();

  GraphTraversalService traversal;
  RoiPropagationService propagation;
  compute::DirtyRegionPlanner planner(traversal, propagation);
  auto plan = planner.plan_high_precision(graph, 42, (PixelRect{5, 5, 10, 10}));

  ASSERT_TRUE(plan.entries.count(42));
  EXPECT_EQ(plan.entries.at(42).roi_hp, (PixelRect{0, 0, 128, 128}))
      << "monolithic nodes must escalate local dirty work to the full output";
  EXPECT_FALSE(plan.snapshot.empty());
  EXPECT_FALSE(plan.snapshot.dirty_monolithic_nodes.empty());
  EXPECT_FALSE(plan.snapshot.dirty_source_nodes.empty());
  EXPECT_FALSE(plan.snapshot.actual_dirty_rois.empty());
  EXPECT_TRUE(plan.snapshot.per_node_dirty_rois.count(42));
  EXPECT_FALSE(plan.snapshot.edge_mappings.empty());
  EXPECT_NE(compute::DirtyRegionPlanner::describe_snapshot(plan.snapshot)
                .find("edges="),
            std::string::npos);

  EXPECT_THROW(planner.plan_real_time(graph, 42, (PixelRect{})), GraphError);
}

TEST(DirtyRegionPlannerSplit, PreservesDomainSpecificHpAndRtProjection) {
  register_split_ops();
  GraphModel graph("cache/split-dirty-domain-policy");
  Node source = make_node(10, "split_plan", "tile");
  source.parameters["width"] = 128;
  source.parameters["height"] = 128;
  Node target = make_node(20, "split_plan", "tile");
  target.image_inputs.push_back({10, "image"});
  graph.add_node(source);
  graph.add_node(target);
  graph.validate_topology();

  GraphTraversalService traversal;
  RoiPropagationService propagation;
  compute::DirtyRegionPlanner planner(traversal, propagation);

  auto hp_plan =
      planner.plan_high_precision(graph, 20, (PixelRect{5, 5, 10, 10}));
  ASSERT_EQ(hp_plan.entries.size(), 2u);
  ASSERT_TRUE(hp_plan.entries.count(10));
  ASSERT_TRUE(hp_plan.entries.count(20));
  EXPECT_EQ(hp_plan.entries.at(20).roi_hp, (PixelRect{0, 0, 64, 64}));
  EXPECT_EQ(hp_plan.entries.at(10).roi_hp, (PixelRect{0, 0, 64, 64}));
  ASSERT_EQ(hp_plan.snapshot.edge_mappings.size(), 1u);
  EXPECT_EQ(hp_plan.snapshot.edge_mappings.front().domain,
            compute::DirtyDomain::HighPrecision);
  EXPECT_EQ(hp_plan.snapshot.edge_mappings.front().from_roi,
            (PixelRect{0, 0, 64, 64}));
  EXPECT_EQ(hp_plan.snapshot.edge_mappings.front().to_roi,
            (PixelRect{0, 0, 64, 64}));
  ASSERT_EQ(hp_plan.snapshot.dirty_tiles.size(), 2u);
  for (const auto& tile : hp_plan.snapshot.dirty_tiles) {
    EXPECT_EQ(tile.domain, compute::DirtyDomain::HighPrecision);
    EXPECT_EQ(tile.tile_size, compute::kHpMicroTileSize);
    EXPECT_EQ(tile.pixel_roi, (PixelRect{0, 0, 64, 64}));
  }

  auto rt_plan = planner.plan_real_time(graph, 20, (PixelRect{5, 5, 10, 10}));
  ASSERT_EQ(rt_plan.entries.size(), 2u);
  ASSERT_TRUE(rt_plan.entries.count(10));
  ASSERT_TRUE(rt_plan.entries.count(20));
  EXPECT_EQ(rt_plan.entries.at(20).hp_size, (PixelSize{128, 128}));
  EXPECT_EQ(rt_plan.entries.at(20).rt_size, (PixelSize{32, 32}));
  EXPECT_EQ(rt_plan.entries.at(20).roi_hp, (PixelRect{0, 0, 64, 64}));
  EXPECT_EQ(rt_plan.entries.at(20).roi_rt, (PixelRect{0, 0, 16, 16}));
  EXPECT_EQ(rt_plan.entries.at(10).roi_hp, (PixelRect{0, 0, 64, 64}));
  EXPECT_EQ(rt_plan.entries.at(10).roi_rt, (PixelRect{0, 0, 16, 16}));
  ASSERT_EQ(rt_plan.snapshot.edge_mappings.size(), 1u);
  EXPECT_EQ(rt_plan.snapshot.edge_mappings.front().domain,
            compute::DirtyDomain::RealTime);
  EXPECT_EQ(rt_plan.snapshot.edge_mappings.front().from_roi,
            (PixelRect{0, 0, 64, 64}));
  EXPECT_EQ(rt_plan.snapshot.edge_mappings.front().to_roi,
            (PixelRect{0, 0, 64, 64}));
  ASSERT_EQ(rt_plan.snapshot.dirty_tiles.size(), 2u);
  for (const auto& tile : rt_plan.snapshot.dirty_tiles) {
    EXPECT_EQ(tile.domain, compute::DirtyDomain::RealTime);
    EXPECT_EQ(tile.tile_size, compute::kRtTileSize);
    EXPECT_EQ(tile.pixel_roi, (PixelRect{0, 0, 16, 16}));
  }
  ASSERT_TRUE(rt_plan.snapshot.per_node_dirty_rois.count(20));
  EXPECT_EQ(rt_plan.snapshot.per_node_dirty_rois.at(20).front(),
            (PixelRect{0, 0, 64, 64}));
}

TEST(DirtyRegionPlannerSplit,
     SourceLifecycleKeepsMembershipAndDerivesActualRegions) {
  register_split_ops();
  GraphModel graph("cache/split-dirty-source-lifecycle");
  Node source = make_node(10, "split_plan", "tile");
  source.parameters["width"] = 64;
  source.parameters["height"] = 64;
  graph.add_node(source);
  graph.validate_topology();

  GraphTraversalService traversal;
  RoiPropagationService propagation;
  compute::DirtyRegionPlanner planner(traversal, propagation);

  EXPECT_THROW(
      planner.begin_dirty_source(graph, 99, compute::DirtyDomain::HighPrecision,
                                 (PixelRect{0, 0, 8, 8})),
      GraphError);
  EXPECT_THROW(
      planner.begin_dirty_source(graph, 10, compute::DirtyDomain::HighPrecision,
                                 (PixelRect{})),
      GraphError);

  auto begin = planner.begin_dirty_source(
      graph, 10, compute::DirtyDomain::HighPrecision, (PixelRect{1, 2, 8, 8}));
  EXPECT_EQ(begin.dirty_source_nodes, (std::vector<int>{10}));
  ASSERT_TRUE(begin.dirty_source_state.count(10));
  EXPECT_EQ(begin.dirty_source_state.at(10).lifecycle,
            compute::DirtySourceLifecycleState::Updating);
  EXPECT_EQ(begin.dirty_updating_count, 1u);
  EXPECT_TRUE(begin.actual_dirty_rois.count(10));
  EXPECT_FALSE(begin.dirty_tiles.empty());

  auto end =
      planner.end_dirty_source(graph, 10, compute::DirtyDomain::HighPrecision);
  EXPECT_EQ(end.dirty_source_nodes, (std::vector<int>{10}))
      << "source membership remains until the dirty generation settles";
  ASSERT_TRUE(end.dirty_source_state.count(10));
  EXPECT_EQ(end.dirty_source_state.at(10).lifecycle,
            compute::DirtySourceLifecycleState::Settled);
  EXPECT_EQ(end.dirty_updating_count, 0u);
  ASSERT_TRUE(graph.last_dirty_region_snapshot_debug.has_value());
  EXPECT_NE(graph.last_dirty_region_snapshot_debug->find("sources=1"),
            std::string::npos);
  EXPECT_NE(graph.last_dirty_region_snapshot_debug->find("actual=1"),
            std::string::npos);
}

TEST(TaskGraphPlanningSplit, PreservesSequentialParallelPlanParity) {
  register_split_ops();
  GraphModel graph("cache/split-plan-parity");
  Node independent = make_node(10, "split_plan", "source");
  independent.parameters["width"] = 16;
  independent.parameters["height"] = 16;
  Node dirty_source = make_node(42, "split_plan", "tile");
  dirty_source.parameters["width"] = 16;
  dirty_source.parameters["height"] = 16;
  Node monolithic = make_node(100, "split_plan", "monolithic");
  monolithic.image_inputs.push_back({42, "image"});
  graph.add_node(independent);
  graph.add_node(dirty_source);
  graph.add_node(monolithic);
  graph.validate_topology();

  compute::DirtyRegionSnapshot snapshot;
  snapshot.graph_generation = 7;
  snapshot.dirty_source_nodes.push_back(42);
  snapshot.per_node_dirty_rois[42].push_back((PixelRect{0, 0, 16, 16}));
  snapshot.per_node_dirty_rois[100].push_back((PixelRect{0, 0, 8, 8}));
  snapshot.actual_dirty_rois = snapshot.per_node_dirty_rois;
  snapshot.dirty_tiles.push_back({42, compute::DirtyDomain::HighPrecision,
                                  compute::DirtyTileLevel::Micro, 0, 0, 16,
                                  (PixelRect{0, 0, 16, 16})});
  snapshot.dirty_monolithic_nodes.push_back(
      {100, compute::DirtyDomain::HighPrecision, (PixelRect{0, 0, 8, 8}),
       true});
  snapshot.edge_mappings.push_back(
      {42, 100, compute::DirtyDomain::HighPrecision, (PixelRect{0, 0, 16, 16}),
       (PixelRect{0, 0, 8, 8}), compute::DirtyEdgeDirection::BackwardDemand});
  std::vector<int> execution_order{10, 42, 100};

  compute::ComputeRequest sequential;
  sequential.intent = ComputeIntent::GlobalHighPrecision;
  sequential.target_node_id = 100;
  sequential.parallel = false;
  compute::ComputeRequest parallel = sequential;
  parallel.parallel = true;

  const auto sequential_base =
      node_cache_pruned_plan(graph, sequential, execution_order);
  const auto parallel_base =
      node_cache_pruned_plan(graph, parallel, execution_order);
  const auto sequential_plan =
      dirty_snapshot_pruned_plan(sequential_base, snapshot, graph);
  const auto parallel_plan =
      dirty_snapshot_pruned_plan(parallel_base, snapshot, graph);
  EXPECT_EQ(sequential_plan.planned_nodes, parallel_plan.planned_nodes);
  EXPECT_EQ(sequential_plan.planned_nodes, (std::vector<int>{10, 42, 100}));
  ASSERT_EQ(sequential_plan.planned_work.size(), 3u);
  EXPECT_EQ(sequential_plan.planned_work[1].node_id, 42);
  EXPECT_EQ(sequential_plan.planned_work[1].represented_hp_roi,
            (PixelRect{0, 0, 16, 16}));
  EXPECT_EQ(sequential_plan.planned_work[1].execution_roi,
            (PixelRect{0, 0, 16, 16}));
  EXPECT_EQ(sequential_plan.planned_work[2].node_id, 100);
  EXPECT_TRUE(sequential_plan.planned_work[2].whole_output);
  ASSERT_EQ(sequential_plan.task_graph.dependencies.size(), 1u);
  EXPECT_EQ(sequential_plan.task_graph.dependencies[0].from_node_id, 42);
  EXPECT_EQ(sequential_plan.task_graph.dependencies[0].to_node_id, 100);
  EXPECT_EQ(sequential_plan.task_graph.dependencies[0].domain,
            compute::DirtyDomain::HighPrecision);
  ASSERT_EQ(sequential_plan.task_graph.tasks.size(), 3u);
  auto task_for_node = [&](int node_id) -> const compute::PlannedTask& {
    auto it = std::find_if(sequential_plan.task_graph.tasks.begin(),
                           sequential_plan.task_graph.tasks.end(),
                           [&](const compute::PlannedTask& task) {
                             return task.node_id == node_id;
                           });
    EXPECT_NE(it, sequential_plan.task_graph.tasks.end());
    return *it;
  };
  const auto& tile_task = task_for_node(42);
  const auto& mono_task = task_for_node(100);
  EXPECT_EQ(tile_task.kind, compute::PlannedTaskKind::Tile);
  EXPECT_TRUE(tile_task.source_boundary_eligible);
  EXPECT_TRUE(tile_task.dirty_selected);
  EXPECT_EQ(tile_task.dirty_generation, 7u);
  EXPECT_EQ(mono_task.kind, compute::PlannedTaskKind::Monolithic);
  EXPECT_TRUE(mono_task.whole_output);
  EXPECT_NE(std::find(sequential_plan.task_graph.initial_task_ids.begin(),
                      sequential_plan.task_graph.initial_task_ids.end(),
                      tile_task.task_id),
            sequential_plan.task_graph.initial_task_ids.end());
  EXPECT_NE(std::find(mono_task.dependency_task_ids.begin(),
                      mono_task.dependency_task_ids.end(), tile_task.task_id),
            mono_task.dependency_task_ids.end());
  EXPECT_EQ(sequential_plan.task_graph.dependencies.size(),
            parallel_plan.task_graph.dependencies.size());
  EXPECT_EQ(sequential_plan.task_graph.tasks.size(),
            parallel_plan.task_graph.tasks.size());
}

TEST(TaskGraphPlanningSplit, ExpandsFullGraphBeforeNodeCachePruning) {
  register_split_ops();
  GraphModel graph("cache/split-full-tile-plan");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  source.cached_output_high_precision = make_image_output(32, 16);
  source.hp_region = value_image_adapter::full_node_output_region(
      *source.cached_output_high_precision);
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.parameters["width"] = 32;
  downstream.parameters["height"] = 16;
  downstream.image_inputs.push_back({1, "image"});
  Node unrelated = make_node(99, "split_plan", "tile");
  unrelated.parameters["width"] = 32;
  unrelated.parameters["height"] = 16;
  graph.add_node(source);
  graph.add_node(downstream);
  graph.add_node(unrelated);
  graph.validate_topology();

  const auto full_graph =
      expand_full_task_graph(graph, ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(full_graph.expanded_node_ids, (std::vector<int>{1, 2, 99}));
  ASSERT_EQ(full_graph.task_graph.tasks.size(), 6u);
  EXPECT_NE(std::find_if(full_graph.task_graph.tasks.begin(),
                         full_graph.task_graph.tasks.end(),
                         [](const compute::PlannedTask& task) {
                           return task.node_id == 99;
                         }),
            full_graph.task_graph.tasks.end())
      << "full expansion must include unrelated nodes before request pruning";

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 2;
  const auto plan = node_cache_pruned_plan(graph, request, {1, 2});

  EXPECT_EQ(plan.planned_nodes, (std::vector<int>{1, 2}));
  ASSERT_EQ(plan.planned_work.size(), 2u);
  EXPECT_TRUE(plan.planned_work.front().reusable_cache_available)
      << "node/cache pruning records cache state without changing full graph";
  ASSERT_EQ(plan.task_graph.tasks.size(), 4u);
  for (const auto& task : plan.task_graph.tasks) {
    EXPECT_NE(task.node_id, 99);
    EXPECT_EQ(task.kind, compute::PlannedTaskKind::Tile);
    EXPECT_EQ(task.domain, compute::DirtyDomain::HighPrecision);
    EXPECT_EQ(task.tile_size, 16);
    EXPECT_TRUE(task.dirty_selected)
        << "without a dirty snapshot, all full-frame tasks are active";
  }
}

TEST(TaskGraphPlanningSplit,
     GraphlessSnapshotPopulationDoesNotCreateDirtyTaskShapes) {
  compute::ComputePlan plan;
  plan.intent = ComputeIntent::GlobalHighPrecision;
  plan.target_node_id = 1;
  plan.planned_nodes = {1};
  compute::PlannedNodeWork work;
  work.node_id = 1;
  work.domain = compute::DirtyDomain::HighPrecision;
  work.execution_roi = (PixelRect{0, 0, 64, 64});
  plan.planned_work.push_back(work);

  compute::DirtyRegionSnapshot snapshot;
  snapshot.graph_generation = 9;
  snapshot.dirty_source_nodes.push_back(1);
  snapshot.per_node_dirty_rois[1].push_back((PixelRect{0, 0, 16, 16}));
  snapshot.dirty_tiles.push_back({1, compute::DirtyDomain::HighPrecision,
                                  compute::DirtyTileLevel::Micro, 0, 0, 16,
                                  (PixelRect{0, 0, 16, 16})});
  snapshot.dirty_monolithic_nodes.push_back(
      {1, compute::DirtyDomain::HighPrecision, (PixelRect{0, 0, 64, 64}),
       true});

  compute::TaskPopulationStrategy strategy;
  strategy.populate(plan, &snapshot, compute::DirtyDomain::HighPrecision,
                    nullptr);

  ASSERT_EQ(plan.task_graph.tasks.size(), 1u);
  const auto& task = plan.task_graph.tasks.front();
  EXPECT_EQ(task.kind, compute::PlannedTaskKind::Node);
  EXPECT_EQ(task.node_id, 1);
  EXPECT_EQ(task.output_roi, (PixelRect{0, 0, 64, 64}));
  EXPECT_EQ(task.tile_size, 0);
  EXPECT_EQ(task.tile_x, -1);
  EXPECT_EQ(task.tile_y, -1);
  EXPECT_TRUE(task.source_boundary_eligible);
  EXPECT_TRUE(task.dirty_selected);
  EXPECT_EQ(task.dirty_generation, 9u);
}

TEST(TaskGraphPlanningSplit, TileDependenciesFollowRoiOverlap) {
  register_split_ops();
  GraphModel graph("cache/split-tile-overlap-dependencies");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.parameters["width"] = 32;
  downstream.parameters["height"] = 16;
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(downstream);
  graph.validate_topology();

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 2;
  const auto plan = node_cache_pruned_plan(graph, request, {1, 2});

  std::vector<const compute::PlannedTask*> downstream_tasks;
  for (const auto& task : plan.task_graph.tasks) {
    if (task.node_id == 2) {
      downstream_tasks.push_back(&task);
    }
  }
  ASSERT_EQ(downstream_tasks.size(), 2u);
  size_t dependency_edges = 0;
  for (const auto* task : downstream_tasks) {
    ASSERT_EQ(task->dependency_task_ids.size(), 1u);
    const auto& upstream_task =
        plan.task_graph.tasks.at(task->dependency_task_ids.front());
    EXPECT_EQ(upstream_task.node_id, 1);
    EXPECT_FALSE(compute::is_rect_empty(
        compute::intersect_rect(upstream_task.output_roi, task->output_roi)));
    dependency_edges += task->dependency_task_ids.size();
  }
  EXPECT_EQ(dependency_edges, 2u)
      << "two upstream tiles feeding two downstream tiles should not form the "
         "four-edge Cartesian product";
}

TEST(TaskGraphPlanningSplit, TileDependenciesUseGaussianHaloInputRoi) {
  register_split_ops();
  GraphModel graph("cache/split-tile-halo-dependencies");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  Node downstream =
      make_node(2, "image_process", "gaussian_blur_dependency_test");
  downstream.parameters["width"] = 32;
  downstream.parameters["height"] = 16;
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(downstream);
  graph.validate_topology();

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 2;
  const auto plan = node_cache_pruned_plan(graph, request, {1, 2});

  std::vector<const compute::PlannedTask*> downstream_tasks;
  for (const auto& task : plan.task_graph.tasks) {
    if (task.node_id == 2) {
      downstream_tasks.push_back(&task);
    }
  }
  ASSERT_EQ(downstream_tasks.size(), 2u);
  for (const auto* task : downstream_tasks) {
    EXPECT_EQ(task->dependency_task_ids.size(), 2u)
        << "gaussian halo expands each downstream tile input ROI across both "
           "upstream tiles";
    for (int dependency_task_id : task->dependency_task_ids) {
      EXPECT_EQ(plan.task_graph.tasks.at(dependency_task_id).node_id, 1);
    }
  }
}

TEST(TaskGraphPlanningSplit, TileDependenciesUseRandomAccessInputRoi) {
  register_split_ops();
  GraphModel graph("cache/split-tile-random-access-dependencies");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 48;
  source.parameters["height"] = 16;
  Node parameter_source = make_node(3, "split_plan", "source");
  parameter_source.cached_output_high_precision = NodeOutput{};
  parameter_source.cached_output_high_precision->data["radius"] = 16;
  parameter_source.hp_version = 1;
  Node downstream = make_node(2, "split_plan", "random_tile");
  downstream.parameters["width"] = 48;
  downstream.parameters["height"] = 16;
  downstream.parameters["radius"] = 0;
  downstream.image_inputs.push_back({1, "image"});
  downstream.parameter_inputs.push_back({3, "radius", "radius"});
  graph.add_node(source);
  graph.add_node(parameter_source);
  graph.add_node(downstream);
  graph.validate_topology();

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 2;
  const auto plan = node_cache_pruned_plan(graph, request, {1, 2});

  const compute::PlannedTask* middle_downstream_task = nullptr;
  for (const auto& task : plan.task_graph.tasks) {
    if (task.node_id == 2 && task.output_roi == (PixelRect{16, 0, 16, 16})) {
      middle_downstream_task = &task;
      break;
    }
  }
  ASSERT_NE(middle_downstream_task, nullptr);
  ASSERT_EQ(middle_downstream_task->dependency_task_ids.size(), 3u)
      << "random-access input ROI expands the middle tile across three "
         "upstream tiles";
  for (int dependency_task_id : middle_downstream_task->dependency_task_ids) {
    EXPECT_EQ(plan.task_graph.tasks.at(dependency_task_id).node_id, 1);
  }
}

/**
 * @brief Proves dependency lowering uses the exact selected sibling metadata.
 *
 * @return Nothing; GoogleTest reports route or dependency mismatches.
 * @throws Graph, registry, extent-resolution, or allocation exceptions when
 * the production planner cannot build the focused graph.
 * @note A monolithic SpatialAligned sibling is registered before the selected
 * device-tiled RandomAccess implementation. Generic key metadata would give
 * the middle downstream tile only one producer dependency; the exact route
 * requires all three.
 */
TEST(TaskGraphPlanningSplit,
     ExactSelectedSiblingMetadataShapesTileDependencies) {
  register_split_ops();
  GraphModel graph("cache/split-exact-sibling-metadata-dependencies");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 48;
  source.parameters["height"] = 16;
  Node downstream = make_node(2, "split_plan", "exact_sibling_metadata");
  downstream.parameters["width"] = 48;
  downstream.parameters["height"] = 16;
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(downstream);
  graph.validate_topology();

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 2;
  const auto plan = node_cache_pruned_plan(graph, request, {1, 2});

  const auto work =
      std::find_if(plan.planned_work.begin(), plan.planned_work.end(),
                   [](const compute::PlannedNodeWork& candidate) {
                     return candidate.node_id == 2;
                   });
  ASSERT_NE(work, plan.planned_work.end());
  ASSERT_TRUE(work->operation_route.has_value());
  EXPECT_TRUE(work->operation_route->tiled);
  EXPECT_EQ(work->operation_route->metadata.tile_preference,
            TileSizePreference::MICRO);
  EXPECT_EQ(work->operation_route->metadata.access_pattern,
            OpMetadata::InputAccessPattern::RandomAccess);

  const compute::PlannedTask* middle_downstream_task = nullptr;
  for (const compute::PlannedTask& task : plan.task_graph.tasks) {
    if (task.node_id == 2 && task.output_roi == (PixelRect{16, 0, 16, 16})) {
      middle_downstream_task = &task;
      break;
    }
  }
  ASSERT_NE(middle_downstream_task, nullptr);
  ASSERT_EQ(middle_downstream_task->dependency_task_ids.size(), 3U);
  for (int dependency_task_id : middle_downstream_task->dependency_task_ids) {
    EXPECT_EQ(plan.task_graph.tasks.at(dependency_task_id).node_id, 1);
  }
}

TEST(TaskGraphPlanningSplit,
     ParameterizedRandomAccessAlwaysWaitsForEveryImageTile) {
  register_split_ops();
  GraphModel graph("cache/split-parameterized-random-access-dependencies");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 64;
  source.parameters["height"] = 16;
  Node parameter_source = make_node(3, "split_plan", "source");
  parameter_source.parameters["width"] = 1;
  parameter_source.parameters["height"] = 1;
  parameter_source.cached_output_high_precision = NodeOutput{};
  parameter_source.cached_output_high_precision->data["radius"] = 0;
  parameter_source.hp_version = 1;
  Node downstream = make_node(2, "split_plan", "random_tile");
  downstream.parameters["width"] = 64;
  downstream.parameters["height"] = 16;
  downstream.parameters["radius"] = 0;
  downstream.image_inputs.push_back({1, "image"});
  downstream.parameter_inputs.push_back({3, "radius", "radius"});
  graph.add_node(source);
  graph.add_node(parameter_source);
  graph.add_node(downstream);
  graph.validate_topology();

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 2;
  const auto plan = node_cache_pruned_plan(graph, request, {1, 3, 2});

  std::size_t checked_downstream_tiles = 0;
  for (const compute::PlannedTask& task : plan.task_graph.tasks) {
    if (task.node_id != 2 || task.kind != compute::PlannedTaskKind::Tile) {
      continue;
    }
    ++checked_downstream_tiles;
    std::size_t image_dependency_count = 0;
    std::size_t parameter_dependency_count = 0;
    for (int dependency_task_id : task.dependency_task_ids) {
      const int dependency_node_id =
          plan.task_graph.tasks.at(dependency_task_id).node_id;
      image_dependency_count += dependency_node_id == 1 ? 1u : 0u;
      parameter_dependency_count += dependency_node_id == 3 ? 1u : 0u;
    }
    EXPECT_EQ(image_dependency_count, 4u)
        << "a cached FullTaskGraph and the old radius=0 snapshot must not "
           "release a random-access consumer before any image tile that a "
           "same-request parameter result could newly require";
    EXPECT_EQ(parameter_dependency_count, 1u);
  }
  EXPECT_EQ(checked_downstream_tiles, 4u);
}

TEST(TaskGraphPlanningSplit,
     SpatialAlignedConsumerUsesUncachedRequestLocalParameterOverlay) {
  register_split_ops();
  g_spatial_generator_hp_calls.store(0, std::memory_order_relaxed);
  g_spatial_parameter_hp_calls.store(0, std::memory_order_relaxed);
  GraphModel graph("cache/split-spatial-uncached-parameter-producer");
  Node source =
      make_node(1, "image_generator", "spatial_uncached_tiled_source");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  Node parameter_source =
      make_node(3, "split_plan", "spatial_uncached_parameter_source");
  parameter_source.parameters["width"] = 1;
  parameter_source.parameters["height"] = 1;
  Node downstream = make_node(2, "split_plan", "request_local_parameter_probe");
  downstream.parameters["width"] = 32;
  downstream.parameters["height"] = 16;
  downstream.parameters["radius"] = 0;
  downstream.image_inputs.push_back({1, "image"});
  downstream.parameter_inputs.push_back({3, "radius", "radius"});
  graph.add_node(source);
  graph.add_node(parameter_source);
  graph.add_node(downstream);
  graph.validate_topology();
  EXPECT_FALSE(graph.node(1).cached_output_high_precision.has_value());
  EXPECT_FALSE(graph.node(3).cached_output_high_precision.has_value());

  compute::ComputeRequest planning_request;
  planning_request.intent = ComputeIntent::GlobalHighPrecision;
  planning_request.target_node_id = 2;
  const auto plan = node_cache_pruned_plan(graph, planning_request, {1, 3, 2});
  std::size_t source_tile_count = 0;
  std::size_t parameter_task_count = 0;
  std::size_t downstream_tile_count = 0;
  for (const compute::PlannedTask& task : plan.task_graph.tasks) {
    if (task.node_id == 1) {
      EXPECT_EQ(task.kind, compute::PlannedTaskKind::Tile);
      ++source_tile_count;
      continue;
    }
    if (task.node_id == 3) {
      EXPECT_EQ(task.kind, compute::PlannedTaskKind::Monolithic);
      EXPECT_TRUE(task.dependency_task_ids.empty());
      ++parameter_task_count;
      continue;
    }
    if (task.node_id != 2 || task.kind != compute::PlannedTaskKind::Tile) {
      continue;
    }
    ++downstream_tile_count;
    std::size_t image_dependency_count = 0;
    std::size_t parameter_dependency_count = 0;
    for (int dependency_task_id : task.dependency_task_ids) {
      const int dependency_node_id =
          plan.task_graph.tasks.at(dependency_task_id).node_id;
      image_dependency_count += dependency_node_id == 1 ? 1u : 0u;
      parameter_dependency_count += dependency_node_id == 3 ? 1u : 0u;
    }
    EXPECT_EQ(image_dependency_count, 1u);
    EXPECT_EQ(parameter_dependency_count, 1u)
        << "the uncached parameter producer remains a scheduling dependency "
           "even though SpatialAligned ROI geometry does not read its value";
  }
  EXPECT_EQ(source_tile_count, 2u);
  EXPECT_EQ(parameter_task_count, 1u);
  EXPECT_EQ(downstream_tile_count, 2u);
  EXPECT_EQ(g_spatial_generator_hp_calls.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(g_spatial_parameter_hp_calls.load(std::memory_order_relaxed), 0);

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  ComputeService::Request execution_request;
  execution_request.node_id = 2;
  execution_request.intent = ComputeIntent::GlobalHighPrecision;
  execution_request.cache.precision = "float32";
  execution_request.cache.disable_disk_cache = true;
  NodeOutput& output = service.compute(graph, execution_request);

  EXPECT_EQ(output.image_buffer.width, 32);
  EXPECT_EQ(output.image_buffer.height, 16);
  double output_min = 0.0;
  double output_max = 0.0;
  cv::minMaxLoc(toCvMat(output.image_buffer), &output_min, &output_max);
  EXPECT_DOUBLE_EQ(output_min, 7.0);
  EXPECT_DOUBLE_EQ(output_max, 7.0);
  EXPECT_EQ(g_spatial_generator_hp_calls.load(std::memory_order_relaxed), 1)
      << "inline execution computes the generator node once, while the "
         "separately inspected task graph retains two source tiles";
  EXPECT_EQ(g_spatial_parameter_hp_calls.load(std::memory_order_relaxed), 1);
  ASSERT_TRUE(graph.node(1).cached_output_high_precision.has_value());
  const NodeOutput& source_output = *graph.node(1).cached_output_high_precision;
  EXPECT_EQ(source_output.image_buffer.width, 32);
  EXPECT_EQ(source_output.image_buffer.height, 16);
  double source_min = 0.0;
  double source_max = 0.0;
  cv::minMaxLoc(toCvMat(source_output.image_buffer), &source_min, &source_max);
  EXPECT_DOUBLE_EQ(source_min, 3.0);
  EXPECT_DOUBLE_EQ(source_max, 3.0);
  ASSERT_TRUE(graph.node(3).cached_output_high_precision.has_value());
  ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
  EXPECT_EQ(graph.node(2).parameters.at("radius").as_int64(), 0);
  EXPECT_TRUE(graph.node(1).runtime_parameters.empty());
  EXPECT_TRUE(graph.node(2).runtime_parameters.empty());
  EXPECT_TRUE(graph.node(3).runtime_parameters.empty());
}

/**
 * @brief Proves sequential direct authority ends at provider callback return.
 *
 * @return Nothing; GoogleTest reports gate ordering, failure typing, or
 * authority residue.
 * @throws Setup, graph, registry, service, future, synchronization, cache, or
 * provider exceptions unchanged when fixture construction itself fails.
 * @note Both Graphs use the same revisioned HP implementation, cap one, shared
 * exclusive key, and declared retained/scratch demand. A test-product observer
 * first proves the route-backed contender reached a denied operation-admission
 * check. It must then enter and exit while the injected cache encoder remains
 * blocked. The encoder subsequently throws, proving post-provider failure
 * settlement leaves no operation gate, resource ledger, or Run residue.
 */
TEST(ComputeServiceSequentialAdmission,
     ProviderLeaseEndsBeforeBlockingPostProviderCacheFailure) {
  constexpr const char* kType = "issue82_sequential_lease";
  constexpr const char* kSubtype = "shared_provider";
  constexpr const char* kExclusiveKey =
      "issue82-sequential-post-provider-boundary";
  constexpr int kSequentialRole = 1;
  constexpr int kRouteRole = 2;
  auto probe = std::make_shared<SequentialLeaseBoundaryProbe>();

  OpMetadata metadata;
  metadata.maximum_parallelism = 1U;
  metadata.exclusive_key = kExclusiveKey;
  metadata.retained_memory_bytes = 64U;
  metadata.scratch_bytes = 32U;
  OpRegistry::instance().register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc(
          [probe](const Node& node,
                  const std::vector<const NodeOutput*>&) -> NodeOutput {
            const int role = as_int_flexible(node.parameters, "role", 0);
            if (role == kSequentialRole) {
              probe->enter_sequential_provider();
            } else if (role == kRouteRole) {
              probe->enter_route_provider();
            } else {
              throw GraphError(GraphErrc::InvalidParameter,
                               "unknown sequential lease provider role");
            }
            return make_image_output(
                as_int_flexible(node.parameters, "width", 8),
                as_int_flexible(node.parameters, "height", 8), 1,
                role == kSequentialRole ? 2.0f : 3.0f);
          }),
      metadata);
  const auto selected = OpRegistry::instance().select_implementation(
      kType, kSubtype, {Device::CPU}, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(selected.has_value());
  ASSERT_NE(selected->implementation_identity, 0U);

  const ScopedTestDirectory root(
      std::filesystem::temp_directory_path() /
      "photospider-sequential-provider-lease-boundary");
  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>(
      testing::FakeImageArtifactCodec::DecodeCallback{},
      [probe](const std::filesystem::path&, const ImageBuffer&,
              ImageArtifactPrecision) {
        probe->block_post_provider_cache_and_fail();
      });
  compute::ExecutionService authority(1U);
  ScopedOperationAdmissionWaitObservation admission_observation(authority,
                                                                *probe);

  GraphModel sequential_graph((root.path() / "sequential-cache").string());
  Node sequential_node = make_node(1, kType, std::string(kSubtype));
  sequential_node.parameters["width"] = 8;
  sequential_node.parameters["height"] = 8;
  sequential_node.parameters["role"] = kSequentialRole;
  sequential_node.caches.push_back({"image", "output.png"});
  sequential_graph.add_node(std::move(sequential_node));
  sequential_graph.validate_topology();
  GraphTraversalService sequential_traversal;
  GraphCacheService sequential_cache(image_codec,
                                     testing::make_yaml_cache_metadata_codec());
  GraphEventService sequential_events;
  ComputeService sequential_service(sequential_traversal, sequential_cache,
                                    sequential_events, authority);
  testing::ScopedExecutionGraphLifecycle sequential_lifecycle(authority,
                                                              sequential_graph);

  GraphRuntime::Info route_info;
  route_info.name = "issue82-sequential-route-peer";
  route_info.root = root.path() / "route";
  route_info.cache_root = root.path() / "route-cache";
  GraphRuntime route_runtime(route_info);
  route_runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision,
                                        "cpu");
  route_runtime.start();
  GraphModel& route_graph = route_runtime.model();
  Node route_node = make_node(1, kType, std::string(kSubtype));
  route_node.parameters["width"] = 8;
  route_node.parameters["height"] = 8;
  route_node.parameters["role"] = kRouteRole;
  route_graph.add_node(std::move(route_node));
  route_graph.validate_topology();
  GraphTraversalService route_traversal;
  GraphCacheService route_cache(
      providers::make_configured_image_artifact_codec(),
      testing::make_yaml_cache_metadata_codec());
  GraphEventService route_events;
  ComputeService route_service(route_traversal, route_cache, route_events,
                               authority);
  testing::ScopedExecutionGraphLifecycle route_lifecycle(authority,
                                                         route_graph);

  ComputeService::Request sequential_request;
  sequential_request.node_id = 1;
  sequential_request.cache.precision = "int8";
  sequential_request.cache.force_recache = true;
  sequential_request.graph_identity = "issue82-sequential-provider";
  auto route_cancellation =
      std::make_shared<compute::ComputeRequestCancellationSource>();
  ComputeService::Request route_request;
  route_request.node_id = 1;
  route_request.cache.precision = "int8";
  route_request.cache.disable_disk_cache = true;
  route_request.graph_identity = "issue82-route-provider";
  route_request.cancellation_source = route_cancellation;

  auto sequential_future = std::async(std::launch::async, [&] {
    return &sequential_service.compute(sequential_graph, sequential_request);
  });
  const bool sequential_provider_entered =
      probe->wait_for_sequential_provider(std::chrono::seconds(2));
  auto route_future = std::async(std::launch::async, [&] {
    return &route_service.compute_parallel(route_graph, route_runtime,
                                           route_request);
  });
  const bool contender_became_observable =
      probe->wait_for_admission_or_route_provider(std::chrono::seconds(2));
  const bool admission_waited_during_provider =
      probe->operation_admission_waited();
  const bool route_entered_during_provider = probe->route_provider_entered();
  const std::uint64_t waited_implementation_identity =
      probe->waited_implementation_identity();

  probe->release_sequential_provider();
  const bool post_provider_cache_entered =
      probe->wait_for_post_provider_cache(std::chrono::seconds(2));
  const bool route_entered_during_post_provider_cache =
      probe->wait_for_route_provider(std::chrono::seconds(2));
  const bool route_exited_during_post_provider_cache =
      probe->wait_for_route_provider_exit(std::chrono::seconds(2));
  const bool route_settled_during_post_provider_cache =
      route_future.wait_for(std::chrono::seconds(2)) ==
      std::future_status::ready;
  NodeOutput* route_output = nullptr;
  if (route_settled_during_post_provider_cache) {
    EXPECT_NO_THROW(route_output = route_future.get());
  } else {
    (void)route_cancellation->request_cancellation();
  }
  const ResourceVector resources_during_post_provider_cache =
      authority.resource_snapshot().reserved;
  const bool sequential_waited_for_cache =
      sequential_future.wait_for(std::chrono::milliseconds(0)) ==
      std::future_status::timeout;
  probe->release_post_provider_cache();

  bool sequential_saw_cache_failure = false;
  try {
    (void)sequential_future.get();
  } catch (const GraphError& error) {
    sequential_saw_cache_failure = true;
    EXPECT_EQ(error.code(), GraphErrc::Io);
    EXPECT_NE(std::string(error.what())
                  .find("injected sequential post-provider cache failure"),
              std::string::npos);
  }
  if (!route_settled_during_post_provider_cache) {
    EXPECT_THROW((void)route_future.get(), GraphError);
  }

  EXPECT_TRUE(sequential_provider_entered);
  EXPECT_TRUE(contender_became_observable);
  EXPECT_TRUE(admission_waited_during_provider);
  EXPECT_FALSE(route_entered_during_provider);
  EXPECT_EQ(waited_implementation_identity, selected->implementation_identity);
  EXPECT_TRUE(post_provider_cache_entered);
  EXPECT_TRUE(route_entered_during_post_provider_cache);
  EXPECT_TRUE(route_exited_during_post_provider_cache);
  EXPECT_TRUE(route_settled_during_post_provider_cache);
  EXPECT_TRUE(sequential_waited_for_cache);
  EXPECT_TRUE(sequential_saw_cache_failure);
  EXPECT_NE(route_output, nullptr);
  EXPECT_EQ(probe->maximum_active_providers(), 1);
  EXPECT_EQ(resources_during_post_provider_cache, ResourceVector{});
  expect_direct_authority_settled(authority);

  route_runtime.stop();
  OpRegistry::instance().unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Proves every direct resource component releases before Host cache I/O.
 *
 * @return Nothing; GoogleTest reports admission, capacity, failure typing, or
 * authority residue.
 * @throws Setup, graph, registry, service, future, synchronization, cache, or
 * provider exceptions unchanged when fixture construction itself fails.
 * @note Both requests use one callback identity, `maximum_parallelism=1`, and
 * one heap-backed exclusive key. The service CPU, retained-memory, and scratch
 * ceilings equal exactly one production direct-lease vector after an
 * independent capacity-plus-terminator check; a one-byte-short limit and a
 * terminator-only overflow declaration reject without residue. The peer first
 * reaches a denied gate while the sequential provider is active, then must
 * enter and exit while the sequential codec remains blocked.
 */
TEST(ComputeServiceSequentialAdmission,
     ExactDirectCapacityReleasesBeforeBlockingPostProviderCacheFailure) {
  constexpr const char* kType = "issue82_sequential_exact_capacity";
  constexpr const char* kSubtype = "shared_provider";
  constexpr const char* kExclusiveKey =
      "resource-accounting-sequential-direct-exclusive-key";
  constexpr int kSequentialRole = 1;
  constexpr int kPeerRole = 2;
  auto probe = std::make_shared<SequentialLeaseBoundaryProbe>();

  OpMetadata metadata;
  metadata.maximum_parallelism = 1U;
  metadata.exclusive_key = kExclusiveKey;
  metadata.retained_memory_bytes = 64U;
  metadata.scratch_bytes = 32U;
  OpRegistry::instance().register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc(
          [probe](const Node& node,
                  const std::vector<const NodeOutput*>&) -> NodeOutput {
            const int role = as_int_flexible(node.parameters, "role", 0);
            if (role == kSequentialRole) {
              probe->enter_sequential_provider();
            } else if (role == kPeerRole) {
              probe->enter_route_provider();
            } else {
              throw GraphError(GraphErrc::InvalidParameter,
                               "unknown exact-capacity provider role");
            }
            return make_image_output(
                as_int_flexible(node.parameters, "width", 8),
                as_int_flexible(node.parameters, "height", 8), 1,
                role == kSequentialRole ? 4.0f : 5.0f);
          }),
      metadata);
  const auto selected = OpRegistry::instance().select_implementation(
      kType, kSubtype, {Device::CPU}, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(selected.has_value());
  ASSERT_NE(selected->implementation_identity, 0U);
  const compute::OperationExecutionConstraints constraints{
      selected->implementation_identity,
      selected->metadata.reentrant,
      selected->metadata.maximum_parallelism,
      selected->metadata.exclusive_key,
  };
  ASSERT_GT(constraints.exclusive_key.capacity(), 15U);
  const compute::ReadyTaskResourceDemand demand{
      selected->metadata.retained_memory_bytes,
      selected->metadata.scratch_bytes,
      0U,
      1U,
  };
  const ResourceVector exact_resources =
      testing::ExecutionServiceTestAccess::estimate_direct_operation_resources(
          constraints, demand);
  ASSERT_EQ(exact_resources.cpu_slots, 1U);
  const compute::OperationExecutionConstraints retained_constraints(
      constraints);
  const std::uint64_t fixed_retained_bytes =
      testing::ExecutionServiceTestAccess::
          direct_operation_fixed_retained_memory_bytes();
  ASSERT_EQ(exact_resources.retained_memory_bytes,
            metadata.retained_memory_bytes + fixed_retained_bytes +
                static_cast<std::uint64_t>(
                    retained_constraints.exclusive_key.capacity()) +
                1U);
  ASSERT_EQ(exact_resources.scratch_bytes, metadata.scratch_bytes);
  ASSERT_EQ(exact_resources.ready_entries, 0U);
  ASSERT_EQ(exact_resources.ready_bytes, 0U);

  ResourceVector one_byte_short = exact_resources;
  ASSERT_GT(one_byte_short.retained_memory_bytes, 0U);
  --one_byte_short.retained_memory_bytes;
  compute::ExecutionResourceLimits short_limits =
      compute::ExecutionService::default_resource_limits();
  short_limits.cpu_slots = one_byte_short.cpu_slots;
  short_limits.retained_memory_bytes = one_byte_short.retained_memory_bytes;
  short_limits.scratch_bytes = one_byte_short.scratch_bytes;
  short_limits.interactive_headroom = ResourceVector{};
  compute::ExecutionService short_authority(short_limits);
  compute::ComputeRun short_run(compute::ComputeRunSubmission{
      "direct-key-one-byte-short",
      GraphInstanceId{8201U},
      GraphRevision{8201U},
      1,
      ComputeIntent::GlobalHighPrecision,
      compute::ComputeRunQuality::Full,
      compute::ComputeRunQos{compute::ComputeRunQosClass::Throughput,
                             std::nullopt, 1U, std::nullopt},
      compute::SupersessionIdentity{
          compute::SupersessionKey{1, ComputeIntent::GlobalHighPrecision},
          compute::SupersessionGeneration{1U}},
  });
  EXPECT_THROW((void)short_authority.acquire_operation_execution(
                   short_run.acquire_lease(), constraints, demand),
               GraphError);
  EXPECT_EQ(short_authority.resource_snapshot().reserved, ResourceVector{});

  const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
  const std::uint64_t copied_capacity =
      static_cast<std::uint64_t>(retained_constraints.exclusive_key.capacity());
  ASSERT_LT(fixed_retained_bytes + copied_capacity, maximum);
  const compute::ReadyTaskResourceDemand overflow_demand{
      maximum - fixed_retained_bytes - copied_capacity, 0U, 0U, 1U};
  compute::ExecutionResourceLimits overflow_limits =
      compute::ExecutionService::default_resource_limits();
  overflow_limits.cpu_slots = 1U;
  overflow_limits.retained_memory_bytes = maximum;
  overflow_limits.scratch_bytes = 0U;
  overflow_limits.interactive_headroom = ResourceVector{};
  compute::ExecutionService overflow_authority(overflow_limits);
  compute::ComputeRun overflow_run(compute::ComputeRunSubmission{
      "direct-key-terminator-overflow",
      GraphInstanceId{8202U},
      GraphRevision{8202U},
      1,
      ComputeIntent::GlobalHighPrecision,
      compute::ComputeRunQuality::Full,
      compute::ComputeRunQos{compute::ComputeRunQosClass::Throughput,
                             std::nullopt, 1U, std::nullopt},
      compute::SupersessionIdentity{
          compute::SupersessionKey{1, ComputeIntent::GlobalHighPrecision},
          compute::SupersessionGeneration{1U}},
  });
  EXPECT_THROW((void)overflow_authority.acquire_operation_execution(
                   overflow_run.acquire_lease(), constraints, overflow_demand),
               GraphError);
  EXPECT_EQ(overflow_authority.resource_snapshot().reserved, ResourceVector{});

  compute::ExecutionResourceLimits limits =
      compute::ExecutionService::default_resource_limits();
  limits.cpu_slots = exact_resources.cpu_slots;
  limits.retained_memory_bytes = exact_resources.retained_memory_bytes;
  limits.scratch_bytes = exact_resources.scratch_bytes;
  limits.interactive_headroom = ResourceVector{};
  compute::ExecutionService authority(limits);
  ScopedOperationAdmissionWaitObservation admission_observation(authority,
                                                                *probe);

  const ScopedTestDirectory root(
      std::filesystem::temp_directory_path() /
      "photospider-sequential-exact-direct-capacity");
  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>(
      testing::FakeImageArtifactCodec::DecodeCallback{},
      [probe](const std::filesystem::path&, const ImageBuffer&,
              ImageArtifactPrecision) {
        probe->block_post_provider_cache_and_fail();
      });

  GraphModel sequential_graph((root.path() / "sequential-cache").string());
  Node sequential_node = make_node(1, kType, std::string(kSubtype));
  sequential_node.parameters["width"] = 8;
  sequential_node.parameters["height"] = 8;
  sequential_node.parameters["role"] = kSequentialRole;
  sequential_node.caches.push_back({"image", "output.png"});
  sequential_graph.add_node(std::move(sequential_node));
  sequential_graph.validate_topology();
  GraphTraversalService sequential_traversal;
  GraphCacheService sequential_cache(image_codec,
                                     testing::make_yaml_cache_metadata_codec());
  GraphEventService sequential_events;
  ComputeService sequential_service(sequential_traversal, sequential_cache,
                                    sequential_events, authority);
  testing::ScopedExecutionGraphLifecycle sequential_lifecycle(authority,
                                                              sequential_graph);

  GraphModel peer_graph((root.path() / "peer-cache").string());
  Node peer_node = make_node(1, kType, std::string(kSubtype));
  peer_node.parameters["width"] = 8;
  peer_node.parameters["height"] = 8;
  peer_node.parameters["role"] = kPeerRole;
  peer_graph.add_node(std::move(peer_node));
  peer_graph.validate_topology();
  GraphTraversalService peer_traversal;
  GraphCacheService peer_cache(
      providers::make_configured_image_artifact_codec(),
      testing::make_yaml_cache_metadata_codec());
  GraphEventService peer_events;
  ComputeService peer_service(peer_traversal, peer_cache, peer_events,
                              authority);
  testing::ScopedExecutionGraphLifecycle peer_lifecycle(authority, peer_graph);

  ComputeService::Request sequential_request;
  sequential_request.node_id = 1;
  sequential_request.cache.precision = "int8";
  sequential_request.cache.force_recache = true;
  sequential_request.graph_identity = "issue82-exact-capacity-sequential";
  auto peer_cancellation =
      std::make_shared<compute::ComputeRequestCancellationSource>();
  ComputeService::Request peer_request;
  peer_request.node_id = 1;
  peer_request.cache.precision = "int8";
  peer_request.cache.disable_disk_cache = true;
  peer_request.graph_identity = "issue82-exact-capacity-peer";
  peer_request.cancellation_source = peer_cancellation;

  auto sequential_future = std::async(std::launch::async, [&] {
    return &sequential_service.compute(sequential_graph, sequential_request);
  });
  const bool sequential_provider_entered =
      probe->wait_for_sequential_provider(std::chrono::seconds(2));
  auto peer_future = std::async(std::launch::async, [&] {
    return &peer_service.compute(peer_graph, peer_request);
  });
  const bool contender_became_observable =
      probe->wait_for_admission_or_route_provider(std::chrono::seconds(2));
  const bool admission_waited_during_provider =
      probe->operation_admission_waited();
  const bool peer_entered_during_provider = probe->route_provider_entered();
  const std::uint64_t waited_implementation_identity =
      probe->waited_implementation_identity();

  probe->release_sequential_provider();
  const bool post_provider_cache_entered =
      probe->wait_for_post_provider_cache(std::chrono::seconds(2));
  const bool peer_entered_during_post_provider_cache =
      probe->wait_for_route_provider(std::chrono::seconds(2));
  const bool peer_exited_during_post_provider_cache =
      probe->wait_for_route_provider_exit(std::chrono::seconds(2));
  const bool peer_settled_during_post_provider_cache =
      peer_future.wait_for(std::chrono::seconds(2)) ==
      std::future_status::ready;
  NodeOutput* peer_output = nullptr;
  if (peer_settled_during_post_provider_cache) {
    EXPECT_NO_THROW(peer_output = peer_future.get());
  } else {
    (void)peer_cancellation->request_cancellation();
  }
  const ResourceVector resources_during_post_provider_cache =
      authority.resource_snapshot().reserved;
  const bool sequential_waited_for_cache =
      sequential_future.wait_for(std::chrono::milliseconds(0)) ==
      std::future_status::timeout;
  probe->release_post_provider_cache();

  bool sequential_saw_cache_failure = false;
  try {
    (void)sequential_future.get();
  } catch (const GraphError& error) {
    sequential_saw_cache_failure = true;
    EXPECT_EQ(error.code(), GraphErrc::Io);
  }
  if (!peer_settled_during_post_provider_cache) {
    EXPECT_THROW((void)peer_future.get(), GraphError);
  }

  EXPECT_TRUE(sequential_provider_entered);
  EXPECT_TRUE(contender_became_observable);
  EXPECT_TRUE(admission_waited_during_provider);
  EXPECT_FALSE(peer_entered_during_provider);
  EXPECT_EQ(waited_implementation_identity, selected->implementation_identity);
  EXPECT_TRUE(post_provider_cache_entered);
  EXPECT_TRUE(peer_entered_during_post_provider_cache);
  EXPECT_TRUE(peer_exited_during_post_provider_cache);
  EXPECT_TRUE(peer_settled_during_post_provider_cache);
  EXPECT_TRUE(sequential_waited_for_cache);
  EXPECT_TRUE(sequential_saw_cache_failure);
  EXPECT_NE(peer_output, nullptr);
  EXPECT_EQ(probe->maximum_active_providers(), 1);
  EXPECT_EQ(resources_during_post_provider_cache, ResourceVector{});
  expect_direct_authority_settled(authority);

  OpRegistry::instance().unregister_key(make_key(kType, kSubtype));
}

TEST(TaskGraphPlanningSplit,
     DirtyConnectedParameterPromotesGeometryAndTaskWaitsToFullExtent) {
  register_split_ops();
  GraphModel graph("cache/split-dirty-connected-parameter-full-dependency");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 64;
  source.parameters["height"] = 16;
  Node parameter = make_node(3, "split_plan", "parameter_source");
  parameter.parameters["width"] = 1;
  parameter.parameters["height"] = 1;
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.parameters["width"] = 64;
  downstream.parameters["height"] = 16;
  downstream.parameter_inputs.push_back({3, "radius", "radius"});
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(parameter);
  graph.add_node(downstream);
  graph.validate_topology();

  GraphTraversalService traversal;
  RoiPropagationService propagation;
  compute::DirtyRegionPlanner planner(traversal, propagation);
  auto verify_domain = [&](ComputeIntent intent) {
    const bool realtime = intent == ComputeIntent::RealTimeUpdate;
    const auto snapshot =
        realtime
            ? planner.plan_real_time(graph, 2, (PixelRect{31, 0, 2, 2}))
                  .snapshot
            : planner.plan_high_precision(graph, 2, (PixelRect{31, 0, 2, 2}))
                  .snapshot;
    ASSERT_TRUE(snapshot.per_node_dirty_rois.count(1));
    ASSERT_TRUE(snapshot.per_node_dirty_rois.count(2));
    ASSERT_TRUE(snapshot.per_node_dirty_rois.count(3));
    EXPECT_EQ(snapshot.per_node_dirty_rois.at(1).front(),
              (PixelRect{0, 0, 64, 16}));
    EXPECT_EQ(snapshot.per_node_dirty_rois.at(2).front(),
              (PixelRect{0, 0, 64, 16}));

    compute::ComputeRequest request;
    request.intent = intent;
    request.target_node_id = 2;
    const auto base = node_cache_pruned_plan(graph, request, {1, 3, 2});
    compute::DirtySnapshotTaskGraphPruner pruner;
    const auto selection = pruner.select(base, snapshot, graph);
    const std::size_t upstream_image_task_count =
        static_cast<std::size_t>(std::count_if(
            base.task_graph.tasks.begin(), base.task_graph.tasks.end(),
            [](const auto& task) { return task.node_id == 1; }));
    ASSERT_GT(upstream_image_task_count, 0u);
    std::size_t checked_consumers = 0;
    for (int task_id : selection.active_task_ids) {
      const auto& task = base.task_graph.tasks.at(task_id);
      if (task.node_id != 2) {
        continue;
      }
      ++checked_consumers;
      std::size_t image_dependencies = 0;
      std::size_t parameter_dependencies = 0;
      for (int dependency_id : selection.dependency_task_ids.at(task_id)) {
        const int dependency_node =
            base.task_graph.tasks.at(dependency_id).node_id;
        image_dependencies += dependency_node == 1 ? 1u : 0u;
        parameter_dependencies += dependency_node == 3 ? 1u : 0u;
      }
      EXPECT_EQ(image_dependencies, upstream_image_task_count)
          << "snapshot from_roi is a dependency lower bound for every active "
             "consumer task";
      EXPECT_EQ(parameter_dependencies, 1u);
    }
    EXPECT_GT(checked_consumers, 0u);
  };

  verify_domain(ComputeIntent::GlobalHighPrecision);
  verify_domain(ComputeIntent::RealTimeUpdate);
}

/**
 * @brief Proves HP and realtime child Runs validate before target planning.
 *
 * @note A zero QoS weight is a Run descriptor error. The missing HP target
 * would instead produce NotFound if planning ran first. RealTimeUpdate creates
 * its separate HP/RT children before intent planning, so the same invalid QoS
 * is rejected without manufacturing a mixed-domain Run.
 */
TEST(ComputeRunProductPath, HpAndRealtimeChildrenValidateBeforePlanning) {
  GraphModel graph("cache/compute-run-product-path");
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  ComputeService::Request hp_request;
  hp_request.node_id = 404;
  hp_request.graph_identity = "product-path-hp";
  hp_request.qos.weight = 0;
  EXPECT_THROW((void)service.compute(graph, hp_request), std::invalid_argument);

  ComputeService::Request realtime_request = hp_request;
  realtime_request.intent = ComputeIntent::RealTimeUpdate;
  EXPECT_THROW((void)service.compute(graph, realtime_request),
               std::invalid_argument);

  const compute::ExecutionLifecyclePage lifecycle =
      execution_service.lifecycle_snapshot(0U, 64U);
  EXPECT_EQ(lifecycle.counters.pending_candidate_count, 0U);
  EXPECT_EQ(lifecycle.counters.admitted_standalone_run_count, 0U);
  EXPECT_EQ(lifecycle.counters.admitted_run_group_count, 0U);
  EXPECT_EQ(std::count_if(
                lifecycle.records.begin(), lifecycle.records.end(),
                [](const compute::ExecutionLifecycleEvent& event) {
                  return event.kind ==
                         compute::ExecutionLifecycleEventKind::CandidateBegan;
                }),
            2);
  EXPECT_EQ(
      std::count_if(
          lifecycle.records.begin(), lifecycle.records.end(),
          [](const compute::ExecutionLifecycleEvent& event) {
            return event.kind ==
                   compute::ExecutionLifecycleEventKind::CandidateRolledBack;
          }),
      2);
  EXPECT_EQ(std::count_if(
                lifecycle.records.begin(), lifecycle.records.end(),
                [](const compute::ExecutionLifecycleEvent& event) {
                  return event.kind ==
                         compute::ExecutionLifecycleEventKind::BundleAdmitted;
                }),
            0);
}

/**
 * @brief Proves failed full-HP preparation publishes no diagnostics or sink
 * reset.
 *
 * @return Nothing; GoogleTest assertions report candidate, inspection, or
 * caller-owned benchmark mutations.
 * @throws Runtime, graph, planning, or test setup failures unchanged.
 * @note Both inline and route-backed paths build a valid task graph for an
 * operation key that has no executable callback. Resolution therefore fails
 * after planning but before lifecycle installation. The existing benchmark
 * record and all plan histories must remain unchanged.
 */
TEST(ComputeRunProductPath,
     FullHpPreparationFailureLeavesInspectionAndBenchmarkUnpublished) {
  const ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                                 "photospider-full-hp-preparation-rollback");
  GraphRuntime::Info info;
  info.name = "full-hp-preparation-rollback";
  info.root = root.path();
  info.cache_root = root.path() / "cache";
  GraphRuntime runtime(info);
  runtime.start();
  GraphModel& graph = runtime.model();
  Node unresolved = make_node(1, "unregistered", "candidate_preparation");
  unresolved.parameters["width"] = 8;
  unresolved.parameters["height"] = 8;
  graph.add_node(unresolved);
  graph.validate_topology();

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(1U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  std::vector<BenchmarkEvent> benchmark_events(1U);
  ComputeService::Request request;
  request.node_id = 1;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  request.telemetry.benchmark_events = &benchmark_events;

  const auto expect_unpublished_failure = [&](bool parallel) {
    SCOPED_TRACE(parallel ? "parallel" : "inline");
    EXPECT_THROW(parallel ? static_cast<void>(service.compute_parallel(
                                graph, runtime, request))
                          : static_cast<void>(service.compute(graph, request)),
                 GraphError);
    EXPECT_EQ(benchmark_events.size(), 1U);
    EXPECT_FALSE(graph.last_compute_plan.has_value());
    EXPECT_FALSE(graph.last_compute_plan_summary.has_value());
    EXPECT_TRUE(graph.recent_compute_plan_summaries.empty());
  };
  expect_unpublished_failure(false);
  expect_unpublished_failure(true);

  const compute::ExecutionLifecyclePage lifecycle =
      execution_service.lifecycle_snapshot(0U, 64U);
  EXPECT_EQ(lifecycle.counters.pending_candidate_count, 0U);
  EXPECT_EQ(lifecycle.counters.admitted_standalone_run_count, 0U);
  EXPECT_EQ(std::count_if(
                lifecycle.records.begin(), lifecycle.records.end(),
                [](const compute::ExecutionLifecycleEvent& event) {
                  return event.kind ==
                         compute::ExecutionLifecycleEventKind::CandidateBegan;
                }),
            2);
  EXPECT_EQ(
      std::count_if(
          lifecycle.records.begin(), lifecycle.records.end(),
          [](const compute::ExecutionLifecycleEvent& event) {
            return event.kind ==
                   compute::ExecutionLifecycleEventKind::CandidateRolledBack;
          }),
      2);
  EXPECT_EQ(std::count_if(
                lifecycle.records.begin(), lifecycle.records.end(),
                [](const compute::ExecutionLifecycleEvent& event) {
                  return event.kind ==
                         compute::ExecutionLifecycleEventKind::BundleAdmitted;
                }),
            0);
  runtime.stop();
}

TEST(ComputeServiceSplit,
     DirtyConnectedKernelStabilizesThreeToTwentyOneToThree) {
  register_split_ops();
  g_dynamic_parameter_fail.store(false, std::memory_order_release);
  g_dynamic_parameter_calls.store(0, std::memory_order_relaxed);
  g_dynamic_blur_ksize.store(3, std::memory_order_release);
  GraphModel graph("cache/split-dynamic-connected-kernel");
  populate_dynamic_blur_graph(graph);
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  ComputeService::Request full;
  full.node_id = 2;
  full.cache.precision = "float32";
  full.cache.disable_disk_cache = true;
  (void)service.compute(graph, full);
  const int calls_after_full =
      g_dynamic_parameter_calls.load(std::memory_order_relaxed);

  const auto verify_kernel = [&](int kernel_size) {
    g_dynamic_blur_ksize.store(kernel_size, std::memory_order_release);
    ComputeService::Request dirty = full;
    dirty.intent = ComputeIntent::GlobalHighPrecision;
    dirty.dirty_roi = (PixelRect{270, 16, 3, 3});
    NodeOutput& output = service.compute(graph, dirty);
    const cv::Mat source =
        toCvMat(graph.node(1).cached_output_high_precision->image_buffer);
    cv::Mat expected;
    cv::GaussianBlur(source, expected, cv::Size{kernel_size, kernel_size}, 0, 0,
                     cv::BORDER_REPLICATE);
    EXPECT_LE(cv::norm(toCvMat(output.image_buffer), expected, cv::NORM_INF),
              1e-6);
    ASSERT_TRUE(graph.last_compute_plan_summary.has_value());
    EXPECT_EQ(graph.last_compute_plan_summary->topology_generation,
              graph.topology_generation());
    EXPECT_EQ(graph.last_compute_plan_summary->full_graph_cache_key,
              compute::full_task_graph_cache_key(
                  graph, ComputeIntent::GlobalHighPrecision));
  };

  verify_kernel(21);
  verify_kernel(3);
  EXPECT_EQ(g_dynamic_parameter_calls.load(std::memory_order_relaxed),
            calls_after_full + 2)
      << "each dirty request stabilizes its data-only producer exactly once";
}

/**
 * @brief Proves nonparallel HP dirty callbacks share nonreentrant authority
 * across independent Graphs.
 *
 * @return Nothing; GoogleTest reports overlap, settlement, or output failures.
 * @throws Graph, service, registry, future, or provider exceptions unchanged.
 * @note The first provider blocks after entry. The second Graph reaches the
 * same identity but cannot enter until release, proving dependency-free inline
 * topology still uses the process OperationStartGate.
 */
TEST(ComputeServiceDirectDirtyAdmission,
     NonparallelHpSerializesNonreentrantIdentityAcrossGraphs) {
  constexpr const char* kType = "issue82_direct_dirty";
  constexpr const char* kSubtype = "hp_nonreentrant";
  auto probe = std::make_shared<DirectDirtyProviderProbe>();
  OpMetadata metadata;
  metadata.reentrant = false;
  OpRegistry::instance().register_op_hp_monolithic(
      kType, kSubtype, make_probed_image_operation(probe, 2.0f), metadata);

  compute::ExecutionService authority;
  DirectDirtyComputeHarness first(authority, "issue82-hp-nonreentrant-first");
  DirectDirtyComputeHarness second(authority, "issue82-hp-nonreentrant-second");
  populate_direct_dirty_graph(first.graph(), kSubtype);
  populate_direct_dirty_graph(second.graph(), kSubtype);
  ComputeService::Request full =
      make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 1);
  full.intent.reset();
  full.dirty_roi.reset();
  (void)first.service().compute(first.graph(), full);
  (void)second.service().compute(second.graph(), full);

  probe->reset_and_block();
  const ComputeService::Request dirty =
      make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 1);
  auto first_future = std::async(std::launch::async, [&] {
    (void)first.service().compute(first.graph(), dirty);
  });
  EXPECT_TRUE(probe->wait_for_entries(1, std::chrono::seconds(2)));
  auto second_future = std::async(std::launch::async, [&] {
    (void)second.service().compute(second.graph(), dirty);
  });
  const bool second_entered_while_blocked =
      probe->wait_for_entries(2, std::chrono::milliseconds(200));
  probe->release();

  EXPECT_FALSE(second_entered_while_blocked);
  EXPECT_NO_THROW(first_future.get());
  EXPECT_NO_THROW(second_future.get());
  EXPECT_EQ(probe->entered(), 2);
  EXPECT_EQ(probe->maximum_active(), 1);
  EXPECT_EQ(authority.resource_snapshot().reserved, ResourceVector{});
  OpRegistry::instance().unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Proves nonparallel RT dirty callbacks honor the exact implementation
 * parallelism cap across Graphs.
 *
 * @return Nothing; GoogleTest reports overlap or settlement failures.
 * @throws Graph, service, registry, future, or provider exceptions unchanged.
 * @note HP sibling work uses an unobserved monolithic slot. Only the RT tiled
 * slot carries `maximum_parallelism=1`, so the observation also proves domain
 * metadata remains paired with the selected RT callback.
 */
TEST(ComputeServiceDirectDirtyAdmission,
     NonparallelRtHonorsImplementationCapAcrossGraphs) {
  constexpr const char* kType = "issue82_direct_dirty";
  constexpr const char* kSubtype = "rt_identity_cap";
  auto probe = std::make_shared<DirectDirtyProviderProbe>();
  register_direct_cached_source_operation();
  OpRegistry::instance().register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc([](const Node& node,
                          const std::vector<const NodeOutput*>&) {
        return make_image_output(as_int_flexible(node.parameters, "width", 8),
                                 as_int_flexible(node.parameters, "height", 8),
                                 1, 3.0f);
      }));
  OpMetadata rt_metadata;
  rt_metadata.reentrant = true;
  rt_metadata.maximum_parallelism = 1U;
  rt_metadata.tile_preference = TileSizePreference::MICRO;
  OpRegistry::instance().register_op_rt_tiled(
      kType, kSubtype, make_probed_tile_operation(probe, 4.0f), rt_metadata);

  compute::ExecutionService authority;
  DirectDirtyComputeHarness first(authority, "issue82-rt-cap-first");
  DirectDirtyComputeHarness second(authority, "issue82-rt-cap-second");
  populate_direct_dirty_graph(first.graph(), kSubtype, false, true);
  populate_direct_dirty_graph(second.graph(), kSubtype, false, true);
  ComputeService::Request full =
      make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 1);
  full.intent.reset();
  full.dirty_roi.reset();
  (void)first.service().compute(first.graph(), full);
  (void)second.service().compute(second.graph(), full);

  probe->reset_and_block();
  const ComputeService::Request dirty =
      make_direct_dirty_request(ComputeIntent::RealTimeUpdate, 1);
  auto first_future = std::async(std::launch::async, [&] {
    (void)first.service().compute(first.graph(), dirty);
  });
  EXPECT_TRUE(probe->wait_for_entries(1, std::chrono::seconds(2)));
  auto second_future = std::async(std::launch::async, [&] {
    (void)second.service().compute(second.graph(), dirty);
  });
  const bool second_entered_while_blocked =
      probe->wait_for_entries(2, std::chrono::milliseconds(200));
  probe->release();

  EXPECT_FALSE(second_entered_while_blocked);
  EXPECT_NO_THROW(first_future.get());
  EXPECT_NO_THROW(second_future.get());
  EXPECT_EQ(probe->entered(), 2);
  EXPECT_EQ(probe->maximum_active(), 1);
  EXPECT_EQ(authority.resource_snapshot().reserved, ResourceVector{});
  OpRegistry::instance().unregister_key(make_key(kType, kSubtype));
  OpRegistry::instance().unregister_key(
      make_key(kType, kDirectCachedSourceSubtype));
}

/**
 * @brief Proves connected-preflight direct callbacks serialize equal keys and
 * overlap different keys across independent identities.
 *
 * @return Nothing; GoogleTest reports incorrect key gating or settlement.
 * @throws Graph, service, registry, future, or provider exceptions unchanged.
 * @note Each round uses two different parameter operation identities. Equal
 * keys must serialize; different keys must both enter while blocked. The
 * target callback is unobserved and runs only after connected stabilization.
 */
TEST(ComputeServiceDirectDirtyAdmission,
     NonparallelPreflightHonorsSameAndDifferentExclusiveKeys) {
  constexpr const char* kType = "issue82_direct_preflight";
  constexpr const char* kTargetSubtype = "exclusive_key_target";
  OpRegistry::instance().register_op_hp_monolithic(
      kType, kTargetSubtype,
      MonolithicOpFunc([](const Node& node,
                          const std::vector<const NodeOutput*>&) {
        return make_image_output(as_int_flexible(node.parameters, "width", 8),
                                 as_int_flexible(node.parameters, "height", 8),
                                 1, 5.0f);
      }));

  const auto run_round =
      [&](const std::string& label, const std::string& first_key,
          const std::string& second_key, bool expect_overlap) {
        const std::string first_subtype = label + "_first";
        const std::string second_subtype = label + "_second";
        auto probe = std::make_shared<DirectDirtyProviderProbe>();
        OpMetadata first_metadata;
        first_metadata.exclusive_key = first_key;
        OpMetadata second_metadata;
        second_metadata.exclusive_key = second_key;
        OpRegistry::instance().register_op_hp_monolithic(
            kType, first_subtype, make_probed_parameter_operation(probe),
            first_metadata);
        OpRegistry::instance().register_op_hp_monolithic(
            kType, second_subtype, make_probed_parameter_operation(probe),
            second_metadata);

        compute::ExecutionService authority;
        DirectDirtyComputeHarness first(authority, label + "-first");
        DirectDirtyComputeHarness second(authority, label + "-second");
        populate_direct_preflight_graph(first.graph(), first_subtype,
                                        kTargetSubtype);
        populate_direct_preflight_graph(second.graph(), second_subtype,
                                        kTargetSubtype);
        ComputeService::Request full =
            make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 2);
        full.intent.reset();
        full.dirty_roi.reset();
        (void)first.service().compute(first.graph(), full);
        (void)second.service().compute(second.graph(), full);

        probe->reset_and_block();
        const ComputeService::Request dirty =
            make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 2);
        auto first_future = std::async(std::launch::async, [&] {
          (void)first.service().compute(first.graph(), dirty);
        });
        EXPECT_TRUE(probe->wait_for_entries(1, std::chrono::seconds(2)));
        auto second_future = std::async(std::launch::async, [&] {
          (void)second.service().compute(second.graph(), dirty);
        });
        const bool overlapped = probe->wait_for_entries(
            2, expect_overlap ? std::chrono::seconds(2)
                              : std::chrono::milliseconds(200));
        probe->release();

        EXPECT_EQ(overlapped, expect_overlap);
        EXPECT_NO_THROW(first_future.get());
        EXPECT_NO_THROW(second_future.get());
        EXPECT_EQ(probe->entered(), 2);
        EXPECT_EQ(probe->maximum_active(), expect_overlap ? 2 : 1);
        EXPECT_EQ(authority.resource_snapshot().reserved, ResourceVector{});
        OpRegistry::instance().unregister_key(make_key(kType, first_subtype));
        OpRegistry::instance().unregister_key(make_key(kType, second_subtype));
      };

  run_round("issue82-preflight-same-key", "shared-context", "shared-context",
            false);
  run_round("issue82-preflight-different-key", "context-a", "context-b", true);
  OpRegistry::instance().unregister_key(make_key(kType, kTargetSubtype));
}

/**
 * @brief Proves RT scratch rejection happens before provider entry and leaves
 * direct authority reusable.
 *
 * @return Nothing; GoogleTest reports error typing, callback entry, or residue.
 * @throws Graph, service, registry, or provider exceptions unchanged.
 * @note Re-registering the exact RT slot with an admissible demand mints a new
 * identity; a succeeding retry proves both the failed resource attempt and its
 * gate ownership were released.
 */
TEST(ComputeServiceDirectDirtyAdmission,
     NonparallelRtRejectsScratchBeforeProviderAndRecovers) {
  constexpr const char* kType = "issue82_direct_dirty";
  constexpr const char* kSubtype = "rt_scratch_rejection";
  auto probe = std::make_shared<DirectDirtyProviderProbe>();
  register_direct_cached_source_operation();
  OpRegistry::instance().register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc([](const Node& node,
                          const std::vector<const NodeOutput*>&) {
        return make_image_output(as_int_flexible(node.parameters, "width", 8),
                                 as_int_flexible(node.parameters, "height", 8),
                                 1, 6.0f);
      }));
  OpMetadata rejected_metadata;
  rejected_metadata.tile_preference = TileSizePreference::MICRO;
  rejected_metadata.scratch_bytes = 2U;
  OpRegistry::instance().register_op_rt_tiled(
      kType, kSubtype, make_probed_tile_operation(probe, 7.0f),
      rejected_metadata);

  compute::ExecutionResourceLimits limits =
      compute::ExecutionService::default_resource_limits();
  limits.scratch_bytes = 1U;
  limits.interactive_headroom = ResourceVector{};
  compute::ExecutionService authority(limits);
  DirectDirtyComputeHarness harness(authority, "issue82-rt-scratch-rejection");
  populate_direct_dirty_graph(harness.graph(), kSubtype, true, true);
  const ComputeService::Request dirty =
      make_direct_dirty_request(ComputeIntent::RealTimeUpdate, 1);

  bool saw_typed_rejection = false;
  try {
    (void)harness.service().compute(harness.graph(), dirty);
  } catch (const GraphError& error) {
    saw_typed_rejection = true;
    EXPECT_EQ(error.code(), GraphErrc::ComputeError);
  }
  EXPECT_TRUE(saw_typed_rejection);
  EXPECT_EQ(probe->entered(), 0);
  EXPECT_EQ(authority.resource_snapshot().reserved, ResourceVector{});

  OpMetadata recovered_metadata;
  recovered_metadata.tile_preference = TileSizePreference::MICRO;
  recovered_metadata.scratch_bytes = 0U;
  OpRegistry::instance().register_op_rt_tiled(
      kType, kSubtype, make_probed_tile_operation(probe, 8.0f),
      recovered_metadata);
  EXPECT_NO_THROW((void)harness.service().compute(harness.graph(), dirty));
  EXPECT_GT(probe->entered(), 0);
  EXPECT_EQ(authority.resource_snapshot().reserved, ResourceVector{});
  OpRegistry::instance().unregister_key(make_key(kType, kSubtype));
  OpRegistry::instance().unregister_key(
      make_key(kType, kDirectCachedSourceSubtype));
}

/**
 * @brief Proves connected-preflight retained-memory rejection happens before
 * provider entry and leaves no gate or ledger residue.
 *
 * @return Nothing; GoogleTest reports error typing, entry, or recovery failure.
 * @throws Graph, service, registry, or provider exceptions unchanged.
 * @note The declared retained bytes equal the complete service limit, so the
 * mandatory direct-lease envelope makes admission impossible. A zero-demand
 * replacement then executes successfully through the same process authority.
 */
TEST(ComputeServiceDirectDirtyAdmission,
     NonparallelPreflightRejectsRetainedMemoryBeforeProviderAndRecovers) {
  constexpr const char* kType = "issue82_direct_preflight";
  constexpr const char* kParameterSubtype = "retained_rejection_parameter";
  constexpr const char* kTargetSubtype = "retained_rejection_target";
  constexpr std::uint64_t kRetainedLimit = 4096U;
  auto probe = std::make_shared<DirectDirtyProviderProbe>();
  OpMetadata rejected_metadata;
  rejected_metadata.retained_memory_bytes = kRetainedLimit;
  OpRegistry::instance().register_op_hp_monolithic(
      kType, kParameterSubtype, make_probed_parameter_operation(probe),
      rejected_metadata);
  OpRegistry::instance().register_op_hp_monolithic(
      kType, kTargetSubtype,
      MonolithicOpFunc([](const Node& node,
                          const std::vector<const NodeOutput*>&) {
        return make_image_output(as_int_flexible(node.parameters, "width", 8),
                                 as_int_flexible(node.parameters, "height", 8),
                                 1, 9.0f);
      }));

  compute::ExecutionResourceLimits limits =
      compute::ExecutionService::default_resource_limits();
  limits.retained_memory_bytes = kRetainedLimit;
  limits.interactive_headroom = ResourceVector{};
  compute::ExecutionService authority(limits);
  DirectDirtyComputeHarness harness(authority,
                                    "issue82-preflight-retained-rejection");
  populate_direct_preflight_graph(harness.graph(), kParameterSubtype,
                                  kTargetSubtype, true);
  const ComputeService::Request dirty =
      make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 2);

  bool saw_typed_rejection = false;
  try {
    (void)harness.service().compute(harness.graph(), dirty);
  } catch (const GraphError& error) {
    saw_typed_rejection = true;
    EXPECT_EQ(error.code(), GraphErrc::ComputeError);
  }
  EXPECT_TRUE(saw_typed_rejection);
  EXPECT_EQ(probe->entered(), 0);
  EXPECT_EQ(authority.resource_snapshot().reserved, ResourceVector{});

  OpRegistry::instance().register_op_hp_monolithic(
      kType, kParameterSubtype, make_probed_parameter_operation(probe),
      OpMetadata{});
  EXPECT_NO_THROW((void)harness.service().compute(harness.graph(), dirty));
  EXPECT_EQ(probe->entered(), 1);
  EXPECT_EQ(authority.resource_snapshot().reserved, ResourceVector{});
  OpRegistry::instance().unregister_key(make_key(kType, kParameterSubtype));
  OpRegistry::instance().unregister_key(make_key(kType, kTargetSubtype));
}

/**
 * @brief Proves HP stale identity settles an installed logical lifecycle.
 *
 * @return Nothing; GoogleTest reports error typing, entry, residue, or retry
 * failures.
 * @throws Graph, registry, service, allocation, or provider exceptions
 * unchanged.
 * @note The observer first proves the standalone Run is logically admitted
 * with no callback, grant, root reservation, or resource ledger ownership,
 * then replaces the active scalar slot before exact revalidation. Rejection
 * occurs before provider and operation/resource/physical admission, settles
 * the logical lifecycle, and a clean retry executes only the replacement.
 */
TEST(ComputeServiceDirtyIdentity,
     HpReplacementAfterPlanFailsBeforeProviderAndRecovers) {
  constexpr const char* kType = "issue82_direct_dirty";
  constexpr const char* kSubtype = "hp_replace";
  auto old_entries = std::make_shared<std::atomic_int>(0);
  auto new_entries = std::make_shared<std::atomic_int>(0);
  auto& registry = OpRegistry::instance();
  registry.register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc([old_entries](const Node& node,
                                     const std::vector<const NodeOutput*>&) {
        old_entries->fetch_add(1, std::memory_order_relaxed);
        return make_image_output(as_int_flexible(node.parameters, "width", 8),
                                 as_int_flexible(node.parameters, "height", 8),
                                 1, 10.0f);
      }));

  compute::ExecutionService authority;
  DirectDirtyComputeHarness harness(authority, "issue82-hp-stale-identity");
  populate_direct_dirty_graph(harness.graph(), kSubtype, true);
  const ComputeService::Request dirty =
      make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 1);
  int observer_calls = 0;
  bool saw_logical_standalone_admission = false;
  bool saw_no_physical_admission = false;
  bool saw_no_operation = false;
  {
    ScopedDirtyPostPlanObservation observation([&] {
      ++observer_calls;
      const compute::ExecutionLifecyclePage lifecycle =
          authority.lifecycle_snapshot(0U, 4096U);
      saw_logical_standalone_admission =
          lifecycle.counters.pending_candidate_count == 0U &&
          lifecycle.counters.admitted_standalone_run_count == 1U &&
          lifecycle.counters.admitted_run_group_count == 0U;
      saw_no_physical_admission =
          lifecycle.counters.ready_entry_count == 0U &&
          lifecycle.counters.entered_callback_count == 0U &&
          lifecycle.counters.live_root_reservation_count == 0U &&
          lifecycle.counters.live_child_grant_count == 0U &&
          authority.resource_snapshot().reserved == ResourceVector{};
      registry.register_op_hp_monolithic(
          kType, kSubtype,
          MonolithicOpFunc(
              [new_entries](const Node& node,
                            const std::vector<const NodeOutput*>&) {
                new_entries->fetch_add(1, std::memory_order_relaxed);
                return make_image_output(
                    as_int_flexible(node.parameters, "width", 8),
                    as_int_flexible(node.parameters, "height", 8), 1, 11.0f);
              }));
    });
    try {
      (void)harness.service().compute(harness.graph(), dirty);
    } catch (const GraphError& error) {
      saw_no_operation = true;
      EXPECT_EQ(error.code(), GraphErrc::NoOperation);
    }
  }

  EXPECT_TRUE(saw_no_operation);
  EXPECT_EQ(observer_calls, 1);
  EXPECT_TRUE(saw_logical_standalone_admission);
  EXPECT_TRUE(saw_no_physical_admission);
  EXPECT_EQ(old_entries->load(std::memory_order_relaxed), 0);
  EXPECT_EQ(new_entries->load(std::memory_order_relaxed), 0);
  expect_direct_authority_settled(authority);

  EXPECT_NO_THROW((void)harness.service().compute(harness.graph(), dirty));
  EXPECT_EQ(old_entries->load(std::memory_order_relaxed), 0);
  EXPECT_EQ(new_entries->load(std::memory_order_relaxed), 1);
  expect_direct_authority_settled(authority);
  registry.unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Proves RT stale identity settles an installed logical RunGroup.
 *
 * @return Nothing; GoogleTest reports restoration, error typing, provider,
 * residue, or retry failures.
 * @throws Graph, registry, service, allocation, or provider exceptions
 * unchanged.
 * @note HP preparation notifies first without mutating. At the second
 * notification the test proves the two child Runs are logically admitted as
 * one group with no callback, grant, root reservation, or resource ledger
 * ownership, then restores the plugin capture. Exact revalidation rejects
 * before either provider or operation/resource/physical admission and the
 * group lifecycle settles before retry.
 */
TEST(ComputeServiceDirtyIdentity,
     RtPluginUnloadAfterPlanFailsBeforeProvidersAndRecovers) {
  constexpr const char* kType = "issue82_direct_dirty";
  constexpr const char* kSubtype = "rt_plugin_unload";
  const std::string operation_key = make_key(kType, kSubtype);
  auto hp_entries = std::make_shared<std::atomic_int>(0);
  auto core_rt_entries = std::make_shared<std::atomic_int>(0);
  auto plugin_rt_entries = std::make_shared<std::atomic_int>(0);
  auto& registry = OpRegistry::instance();
  register_direct_cached_source_operation();
  registry.register_op_hp_monolithic(
      kType, kSubtype,
      MonolithicOpFunc([hp_entries](const Node& node,
                                    const std::vector<const NodeOutput*>&) {
        hp_entries->fetch_add(1, std::memory_order_relaxed);
        return make_image_output(as_int_flexible(node.parameters, "width", 8),
                                 as_int_flexible(node.parameters, "height", 8),
                                 1, 12.0f);
      }));
  OpMetadata tiled_metadata;
  tiled_metadata.tile_preference = TileSizePreference::MICRO;
  registry.register_op_rt_tiled(
      kType, kSubtype,
      TileOpFunc([core_rt_entries](const Node&, const OutputTile& output,
                                   const std::vector<InputTile>&) {
        core_rt_entries->fetch_add(1, std::memory_order_relaxed);
        toCvMat(output).setTo(13.0f);
      }),
      tiled_metadata);
  OpRegistry::RegistrationCapture plugin_capture;
  registry.capture_registration(
      [&] {
        registry.register_op_rt_tiled(
            kType, kSubtype,
            TileOpFunc([plugin_rt_entries](const Node&,
                                           const OutputTile& output,
                                           const std::vector<InputTile>&) {
              plugin_rt_entries->fetch_add(1, std::memory_order_relaxed);
              toCvMat(output).setTo(14.0f);
            }),
            tiled_metadata);
      },
      plugin_capture);

  compute::ExecutionService authority;
  DirectDirtyComputeHarness harness(authority, "issue82-rt-stale-identity");
  populate_direct_dirty_graph(harness.graph(), kSubtype, true, true);
  const ComputeService::Request dirty =
      make_direct_dirty_request(ComputeIntent::RealTimeUpdate, 1);
  OpRegistry::RegistryEntrySnapshot retirement;
  retirement.implementations.emplace();
  int observer_calls = 0;
  bool unload_changed_registry = false;
  bool saw_logical_group_admission = false;
  bool saw_no_physical_admission = false;
  bool saw_no_operation = false;
  {
    ScopedDirtyPostPlanObservation observation([&] {
      ++observer_calls;
      if (observer_calls == 2) {
        const compute::ExecutionLifecyclePage lifecycle =
            authority.lifecycle_snapshot(0U, 4096U);
        saw_logical_group_admission =
            lifecycle.counters.pending_candidate_count == 0U &&
            lifecycle.counters.admitted_standalone_run_count == 0U &&
            lifecycle.counters.admitted_run_group_count == 1U &&
            lifecycle.counters.admitted_child_run_count == 2U;
        saw_no_physical_admission =
            lifecycle.counters.ready_entry_count == 0U &&
            lifecycle.counters.entered_callback_count == 0U &&
            lifecycle.counters.live_root_reservation_count == 0U &&
            lifecycle.counters.live_child_grant_count == 0U &&
            authority.resource_snapshot().reserved == ResourceVector{};
        unload_changed_registry = registry.retire_owned_entry_noexcept(
            operation_key, plugin_capture.owned_entries.at(operation_key),
            plugin_capture.previous_entries.at(operation_key), retirement);
      }
    });
    try {
      (void)harness.service().compute(harness.graph(), dirty);
    } catch (const GraphError& error) {
      saw_no_operation = true;
      EXPECT_EQ(error.code(), GraphErrc::NoOperation);
    }
  }

  EXPECT_TRUE(saw_no_operation);
  EXPECT_EQ(observer_calls, 2);
  EXPECT_TRUE(unload_changed_registry);
  EXPECT_TRUE(saw_logical_group_admission);
  EXPECT_TRUE(saw_no_physical_admission);
  EXPECT_EQ(hp_entries->load(std::memory_order_relaxed), 0);
  EXPECT_EQ(core_rt_entries->load(std::memory_order_relaxed), 0);
  EXPECT_EQ(plugin_rt_entries->load(std::memory_order_relaxed), 0);
  expect_direct_authority_settled(authority);

  EXPECT_NO_THROW((void)harness.service().compute(harness.graph(), dirty));
  EXPECT_GT(hp_entries->load(std::memory_order_relaxed), 0);
  EXPECT_GT(core_rt_entries->load(std::memory_order_relaxed), 0);
  EXPECT_EQ(plugin_rt_entries->load(std::memory_order_relaxed), 0);
  expect_direct_authority_settled(authority);
  registry.unregister_key(operation_key);
  registry.unregister_key(make_key(kType, kDirectCachedSourceSubtype));
}

/**
 * @brief Proves an externally satisfied preflight producer is excluded from
 * phase-two exact-identity revalidation.
 *
 * @return Nothing; GoogleTest reports incorrect preflight, replacement, target
 * execution, or authority residue.
 * @throws Graph, registry, service, allocation, or provider exceptions
 * unchanged.
 * @note Connected preflight executes the old producer first. The post-plan
 * observer then replaces that producer while the target remains active; phase
 * two must ignore the externally staged node and execute the unchanged target.
 */
TEST(ComputeServiceDirtyIdentity,
     ExternallySatisfiedReplacementDoesNotInvalidateActiveDirtyTarget) {
  constexpr const char* kType = "issue82_direct_preflight";
  constexpr const char* kParameterSubtype = "external_identity_parameter";
  constexpr const char* kTargetSubtype = "external_identity_target";
  auto old_parameter_entries = std::make_shared<std::atomic_int>(0);
  auto new_parameter_entries = std::make_shared<std::atomic_int>(0);
  auto target_entries = std::make_shared<std::atomic_int>(0);
  auto& registry = OpRegistry::instance();
  registry.register_op_hp_monolithic(
      kType, kParameterSubtype,
      MonolithicOpFunc([old_parameter_entries](
                           const Node&, const std::vector<const NodeOutput*>&) {
        old_parameter_entries->fetch_add(1, std::memory_order_relaxed);
        NodeOutput output;
        output.data["radius"] = 7;
        return output;
      }));
  registry.register_op_hp_monolithic(
      kType, kTargetSubtype,
      MonolithicOpFunc([target_entries](const Node& node,
                                        const std::vector<const NodeOutput*>&) {
        target_entries->fetch_add(1, std::memory_order_relaxed);
        return make_image_output(as_int_flexible(node.parameters, "width", 8),
                                 as_int_flexible(node.parameters, "height", 8),
                                 1, 15.0f);
      }));

  compute::ExecutionService authority;
  DirectDirtyComputeHarness harness(authority, "issue82-external-identity");
  populate_direct_preflight_graph(harness.graph(), kParameterSubtype,
                                  kTargetSubtype, true);
  const ComputeService::Request dirty =
      make_direct_dirty_request(ComputeIntent::GlobalHighPrecision, 2);
  int observer_calls = 0;
  {
    ScopedDirtyPostPlanObservation observation([&] {
      ++observer_calls;
      registry.register_op_hp_monolithic(
          kType, kParameterSubtype,
          MonolithicOpFunc(
              [new_parameter_entries](const Node&,
                                      const std::vector<const NodeOutput*>&) {
                new_parameter_entries->fetch_add(1, std::memory_order_relaxed);
                NodeOutput output;
                output.data["radius"] = 9;
                return output;
              }));
    });
    EXPECT_NO_THROW((void)harness.service().compute(harness.graph(), dirty));
  }

  EXPECT_EQ(observer_calls, 1);
  EXPECT_EQ(old_parameter_entries->load(std::memory_order_relaxed), 1);
  EXPECT_EQ(new_parameter_entries->load(std::memory_order_relaxed), 0);
  EXPECT_EQ(target_entries->load(std::memory_order_relaxed), 1);
  expect_direct_authority_settled(authority);
  registry.unregister_key(make_key(kType, kParameterSubtype));
  registry.unregister_key(make_key(kType, kTargetSubtype));
}

TEST(ComputeServiceSplit,
     PreflightAndPhaseTwoShareStagedImageDataAndDerivedParameter) {
  register_split_ops();
  g_staged_source_generation.store(3, std::memory_order_release);
  g_derived_parameter_seen_generation.store(0, std::memory_order_relaxed);
  GraphModel graph("cache/split-staged-parameter-chain");
  populate_staged_parameter_chain(graph);
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  (void)service.compute(graph, request);

  const auto verify_generation = [&](int generation) {
    g_staged_source_generation.store(generation, std::memory_order_release);
    request.intent = ComputeIntent::GlobalHighPrecision;
    request.dirty_roi = (PixelRect{270, 16, 3, 3});
    NodeOutput& output = service.compute(graph, request);
    EXPECT_EQ(
        g_derived_parameter_seen_generation.load(std::memory_order_acquire),
        generation)
        << "B must consume the request-local staged A output";
    ASSERT_TRUE(graph.last_dirty_region_snapshot.has_value());
    const auto& snapshot = *graph.last_dirty_region_snapshot;
    ASSERT_TRUE(snapshot.dirty_source_state.count(1));
    const auto& source_rois = snapshot.dirty_source_state.at(1).source_rois;
    EXPECT_NE(std::find(source_rois.begin(), source_rois.end(),
                        (PixelRect{0, 0, 320, 64})),
              source_rois.end())
        << "staged A is represented as a complete HP source boundary";
    ASSERT_TRUE(snapshot.actual_dirty_rois.count(2));
    const auto& target_rois = snapshot.actual_dirty_rois.at(2);
    EXPECT_NE(std::find(target_rois.begin(), target_rois.end(),
                        (PixelRect{0, 0, 320, 64})),
              target_rois.end())
        << "phase two conservatively selects the complete dependent output";
    ASSERT_TRUE(graph.last_compute_plan_summary.has_value());
    EXPECT_EQ(graph.last_compute_plan_summary->dirty_source_task_count, 0u)
        << "preflight-staged source tasks must not execute again";
    EXPECT_EQ(graph.last_compute_plan_summary->active_task_count,
              graph.last_compute_plan_summary->downstream_task_count);
    const cv::Mat staged_source =
        toCvMat(graph.node(1).cached_output_high_precision->image_buffer);
    cv::Mat expected;
    cv::GaussianBlur(staged_source, expected, cv::Size{generation, generation},
                     0, 0, cv::BORDER_REPLICATE);
    EXPECT_LE(cv::norm(toCvMat(output.image_buffer), expected, cv::NORM_INF),
              1e-6)
        << "C image and parameter inputs must describe the same staged A";
  };
  verify_generation(21);
  verify_generation(3);
}

TEST(ComputeServiceSplit,
     ConnectedExtentShrinkAcceptsDirtyRoiOutsideNewOutputInBothDomains) {
  register_split_ops();
  for (ComputeIntent intent :
       {ComputeIntent::GlobalHighPrecision, ComputeIntent::RealTimeUpdate}) {
    SCOPED_TRACE(intent == ComputeIntent::RealTimeUpdate ? "RT" : "HP");
    g_dynamic_extent_width.store(64, std::memory_order_release);
    GraphModel graph("cache/split-dynamic-extent-shrink");
    populate_dynamic_extent_graph(graph);
    GraphTraversalService traversal;
    GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                            testing::make_yaml_cache_metadata_codec()};
    GraphEventService events;
    compute::ExecutionService execution_service;
    ComputeService service(traversal, cache, events, execution_service);
    testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                           graph);
    ComputeService::Request request;
    request.node_id = 2;
    request.cache.precision = "float32";
    request.cache.disable_disk_cache = true;
    (void)service.compute(graph, request);
    ASSERT_EQ(graph.node(2).cached_output_high_precision->image_buffer.width,
              64);

    g_dynamic_extent_width.store(16, std::memory_order_release);
    request.intent = intent;
    request.dirty_roi = (PixelRect{48, 0, 8, 8});
    NodeOutput& result = service.compute(graph, request);
    ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
    EXPECT_EQ(graph.node(2).cached_output_high_precision->image_buffer.width,
              16);
    EXPECT_EQ(graph.node(2).cached_output_high_precision->image_buffer.height,
              8);
    EXPECT_GT(result.image_buffer.width, 0);
    EXPECT_GT(result.image_buffer.height, 0);
    if (intent == ComputeIntent::RealTimeUpdate) {
      ASSERT_GE(graph.recent_dirty_region_snapshots.size(), 2u);
      const auto end = graph.recent_dirty_region_snapshots.end();
      EXPECT_EQ((end - 1)->graph_generation, (end - 2)->graph_generation)
          << "HP and RT sibling plans share one request generation";
    }
  }
}

TEST(ComputeServiceSplit,
     ImageCarryingParameterProducerKeepsRtImageWorkDomainLocal) {
  register_split_ops();
  g_image_parameter_hp_calls.store(0, std::memory_order_relaxed);
  g_image_parameter_rt_calls.store(0, std::memory_order_relaxed);
  GraphModel graph("cache/split-image-parameter-rt-domain");
  Node producer =
      make_node(1, "image_generator", "split_image_parameter_source");
  producer.parameters["width"] = 64;
  producer.parameters["height"] = 16;
  Node target = make_node(2, "split_plan", "tile");
  target.parameters["width"] = 64;
  target.parameters["height"] = 16;
  target.parameters["radius"] = 0;
  target.image_inputs.push_back({1, "image"});
  target.parameter_inputs.push_back({1, "radius", "radius"});
  graph.add_node(producer);
  graph.add_node(target);
  graph.validate_topology();
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  (void)service.compute(graph, request);

  g_image_parameter_hp_calls.store(0, std::memory_order_relaxed);
  g_image_parameter_rt_calls.store(0, std::memory_order_relaxed);
  request.intent = ComputeIntent::RealTimeUpdate;
  request.dirty_roi = (PixelRect{8, 0, 4, 4});
  (void)service.compute(graph, request);
  EXPECT_EQ(g_image_parameter_hp_calls.load(std::memory_order_relaxed), 1)
      << "HP preflight result is imported rather than recomputed by HP phase";
  EXPECT_GT(g_image_parameter_rt_calls.load(std::memory_order_relaxed), 0)
      << "an image-carrying parameter producer remains executable in RT";
}

/**
 * @brief Proves a throwing nonreentrant connected preflight publishes no HP
 * state and releases direct operation authority for retry.
 *
 * @return Nothing; GoogleTest reports cache, error, resource, or retry
 * failures.
 * @throws Graph, service, registry, allocation, or provider exceptions
 * unchanged outside the expected failure assertion.
 * @note The initial full compute establishes committed versions. The failing
 * dirty request must preserve both parameter and target state, while a later
 * request must reenter the same identity exactly once.
 */
TEST(ComputeServiceSplit, PreflightFailurePublishesNoHpCacheState) {
  register_split_ops();
  g_dynamic_parameter_fail.store(false, std::memory_order_release);
  g_dynamic_blur_ksize.store(3, std::memory_order_release);
  GraphModel graph("cache/split-preflight-failure-atomicity");
  populate_dynamic_blur_graph(graph);
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  (void)service.compute(graph, request);
  const int target_version = graph.node(2).hp_version;
  const int parameter_version = graph.node(3).hp_version;
  const cv::Mat before =
      toCvMat(graph.node(2).cached_output_high_precision->image_buffer).clone();

  g_dynamic_parameter_fail.store(true, std::memory_order_release);
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.dirty_roi = (PixelRect{10, 10, 2, 2});
  EXPECT_THROW(service.compute(graph, request), GraphError);
  g_dynamic_parameter_fail.store(false, std::memory_order_release);
  EXPECT_EQ(graph.node(2).hp_version, target_version);
  EXPECT_EQ(graph.node(3).hp_version, parameter_version);
  EXPECT_DOUBLE_EQ(
      cv::norm(
          before,
          toCvMat(graph.node(2).cached_output_high_precision->image_buffer),
          cv::NORM_INF),
      0.0);
  EXPECT_EQ(execution_service.resource_snapshot().reserved, ResourceVector{});

  const int calls_before_recovery =
      g_dynamic_parameter_calls.load(std::memory_order_relaxed);
  EXPECT_NO_THROW((void)service.compute(graph, request));
  EXPECT_EQ(g_dynamic_parameter_calls.load(std::memory_order_relaxed),
            calls_before_recovery + 1)
      << "the nonreentrant preflight identity must be reusable after throw";
  EXPECT_EQ(execution_service.resource_snapshot().reserved, ResourceVector{});
}

TEST(ComputeServiceSplit,
     ExactParameterTypeFailureInsidePluginPublishesNoDirtyState) {
  register_split_ops();
  for (const ComputeIntent intent :
       {ComputeIntent::GlobalHighPrecision, ComputeIntent::RealTimeUpdate}) {
    const bool is_rt = intent == ComputeIntent::RealTimeUpdate;
    SCOPED_TRACE(is_rt ? "RT" : "HP");
    g_host_preparation_emit_malformed_value.store(false,
                                                  std::memory_order_release);
    GraphRuntime::Info info;
    info.name = is_rt ? "split-rt-host-preparation-failure"
                      : "split-hp-host-preparation-failure";
    info.root = "cache/" + info.name;
    info.cache_root = info.root / "cache";
    GraphRuntime runtime(info);
    runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision,
                                    "serial_debug");
    runtime.replace_execution_route(ComputeIntent::RealTimeUpdate,
                                    "serial_debug");
    runtime.start();
    GraphModel& graph = runtime.model();
    populate_host_preparation_failure_graph(graph);
    GraphTraversalService traversal;
    GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                            testing::make_yaml_cache_metadata_codec()};
    GraphEventService events;
    compute::ExecutionService execution_service(1U);
    ComputeService service(traversal, cache, events, execution_service);
    testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                           graph);
    ComputeService::Request request;
    request.node_id = 2;
    request.cache.precision = "float32";
    request.cache.disable_disk_cache = true;
    (void)service.compute_parallel(graph, runtime, request);

    g_host_preparation_source_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_plugin_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_hp_target_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_rt_target_calls.store(0, std::memory_order_relaxed);
    request.intent = intent;
    request.dirty_roi = (PixelRect{8, 0, 4, 4});
    (void)service.compute_parallel(graph, runtime, request);
    ASSERT_GT(g_host_preparation_source_calls.load(std::memory_order_relaxed),
              0);
    ASSERT_GT(g_host_preparation_plugin_calls.load(std::memory_order_relaxed),
              0);
    ASSERT_GT((is_rt ? g_host_preparation_rt_target_calls
                     : g_host_preparation_hp_target_calls)
                  .load(std::memory_order_relaxed),
              0)
        << "the control request must prove the graph reaches phase two";

    ASSERT_TRUE(graph.node(1).cached_output_high_precision.has_value());
    ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
    ASSERT_TRUE(graph.node(3).cached_output_high_precision.has_value());
    const cv::Mat source_pixels_before =
        toCvMat(graph.node(1).cached_output_high_precision->image_buffer)
            .clone();
    const cv::Mat target_pixels_before =
        toCvMat(graph.node(2).cached_output_high_precision->image_buffer)
            .clone();
    const int source_value_before =
        static_cast<int>(graph.node(1)
                             .cached_output_high_precision->data.at("injected")
                             .as_int64());
    const int parameter_value_before =
        static_cast<int>(graph.node(3)
                             .cached_output_high_precision->data.at("radius")
                             .as_int64());
    const int source_version_before = graph.node(1).hp_version;
    const int target_version_before = graph.node(2).hp_version;
    const int parameter_version_before = graph.node(3).hp_version;
    const std::optional<RegionSet> source_region_before =
        graph.node(1).hp_region;
    const std::optional<RegionSet> target_region_before =
        graph.node(2).hp_region;
    const std::optional<RegionSet> parameter_region_before =
        graph.node(3).hp_region;

    const compute::RealtimeProxyGraph::NodeState* proxy_before_ptr =
        runtime.realtime_proxy_graph().find_state(2);
    ASSERT_NE(proxy_before_ptr, nullptr);
    const int proxy_version_before = proxy_before_ptr->version;
    const std::optional<RegionSet> proxy_region_before =
        proxy_before_ptr->region_hp;
    const std::optional<std::uint64_t> proxy_generation_before =
        proxy_before_ptr->dirty_source_generation;
    const bool proxy_had_output_before = proxy_before_ptr->output.has_value();
    cv::Mat proxy_pixels_before;
    if (proxy_before_ptr->output) {
      proxy_pixels_before =
          toCvMat(proxy_before_ptr->output->image_buffer).clone();
    }

    const std::size_t dirty_snapshots_before =
        graph.recent_dirty_region_snapshots.size();
    const std::size_t compute_plans_before = graph.recent_compute_plans.size();
    const std::size_t plan_summaries_before =
        graph.recent_compute_plan_summaries.size();
    const std::uint64_t request_generation_before =
        graph.dirty_generation_counter;
    g_host_preparation_source_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_plugin_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_hp_target_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_rt_target_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_emit_malformed_value.store(true,
                                                  std::memory_order_release);
    bool saw_type_failure = false;
    try {
      (void)service.compute_parallel(graph, runtime, request);
    } catch (const GraphError& error) {
      saw_type_failure = true;
      EXPECT_EQ(error.code(), GraphErrc::ComputeError);
      EXPECT_NE(std::string(error.what()).find("split_node_3"),
                std::string::npos)
          << "failure must belong to the adapted parameter node";
    } catch (...) {
      g_host_preparation_emit_malformed_value.store(false,
                                                    std::memory_order_release);
      throw;
    }
    g_host_preparation_emit_malformed_value.store(false,
                                                  std::memory_order_release);

    EXPECT_TRUE(saw_type_failure);
    EXPECT_EQ(g_host_preparation_source_calls.load(std::memory_order_relaxed),
              1)
        << "preflight must stage the malformed connected source exactly once";
    EXPECT_EQ(g_host_preparation_plugin_calls.load(std::memory_order_relaxed),
              1)
        << "the exact Int64 accessor must fail after callback entry";
    EXPECT_EQ(
        g_host_preparation_hp_target_calls.load(std::memory_order_relaxed), 0)
        << "HP phase two must not dispatch after preparation failure";
    EXPECT_EQ(
        g_host_preparation_rt_target_calls.load(std::memory_order_relaxed), 0)
        << "RT phase two must not dispatch after preparation failure";
    EXPECT_EQ(graph.dirty_generation_counter, request_generation_before + 1)
        << "the failure occurs after request reservation inside preflight";
    EXPECT_EQ(graph.recent_dirty_region_snapshots.size(),
              dirty_snapshots_before);
    EXPECT_EQ(graph.recent_compute_plans.size(), compute_plans_before);
    EXPECT_EQ(graph.recent_compute_plan_summaries.size(),
              plan_summaries_before);

    ASSERT_TRUE(graph.node(1).cached_output_high_precision.has_value());
    ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
    ASSERT_TRUE(graph.node(3).cached_output_high_precision.has_value());
    EXPECT_EQ(graph.node(1).hp_version, source_version_before);
    EXPECT_EQ(graph.node(2).hp_version, target_version_before);
    EXPECT_EQ(graph.node(3).hp_version, parameter_version_before);
    EXPECT_EQ(graph.node(1).hp_region, source_region_before);
    EXPECT_EQ(graph.node(2).hp_region, target_region_before);
    EXPECT_EQ(graph.node(3).hp_region, parameter_region_before);
    EXPECT_DOUBLE_EQ(
        cv::norm(
            source_pixels_before,
            toCvMat(graph.node(1).cached_output_high_precision->image_buffer),
            cv::NORM_INF),
        0.0);
    EXPECT_DOUBLE_EQ(
        cv::norm(
            target_pixels_before,
            toCvMat(graph.node(2).cached_output_high_precision->image_buffer),
            cv::NORM_INF),
        0.0);
    EXPECT_EQ(graph.node(1)
                  .cached_output_high_precision->data.at("injected")
                  .as_int64(),
              source_value_before)
        << "the malformed staged source value must not replace HP cache";
    EXPECT_EQ(graph.node(3)
                  .cached_output_high_precision->data.at("radius")
                  .as_int64(),
              parameter_value_before);

    const compute::RealtimeProxyGraph::NodeState* proxy_after =
        runtime.realtime_proxy_graph().find_state(2);
    ASSERT_NE(proxy_after, nullptr);
    EXPECT_EQ(proxy_after->version, proxy_version_before);
    EXPECT_EQ(proxy_after->region_hp, proxy_region_before);
    EXPECT_EQ(proxy_after->dirty_source_generation, proxy_generation_before);
    EXPECT_EQ(proxy_after->output.has_value(), proxy_had_output_before);
    if (proxy_after->output && proxy_had_output_before) {
      EXPECT_DOUBLE_EQ(
          cv::norm(proxy_pixels_before,
                   toCvMat(proxy_after->output->image_buffer), cv::NORM_INF),
          0.0);
    }
    runtime.stop();
  }
}

/**
 * @brief Proves connected provider entry follows install and reserved start.
 *
 * @return Nothing; GoogleTest reports lifecycle, ledger, or provider-order
 * failures.
 * @throws Setup, graph, service, allocation, or callback exceptions unchanged.
 * @note The provider copies the real lifecycle/ledger snapshots from inside
 * its callback. The observed BundleAdmitted sequence must be newer than the
 * baseline request, the candidate must already be consumed, and both entered
 * callback plus live root counters must be nonzero.
 */
TEST(ComputeServiceLifecycle,
     ConnectedPreflightProviderEntersAfterInstallAndReservedStart) {
  register_split_ops();
  const ScopedTestDirectory root(
      std::filesystem::temp_directory_path() /
      "photospider-connected-preflight-installed-start");
  GraphRuntime::Info info;
  info.name = "connected-preflight-installed-start";
  info.root = root.path();
  info.cache_root = root.path() / "cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision,
                                  "serial_debug");
  runtime.start();
  GraphModel& graph = runtime.model();
  populate_host_preparation_failure_graph(graph);
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(1U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  (void)service.compute_parallel(graph, runtime, request);

  g_host_preparation_emit_malformed_value.store(false,
                                                std::memory_order_release);
  g_host_preparation_source_calls.store(0, std::memory_order_relaxed);
  g_host_preparation_plugin_calls.store(0, std::memory_order_relaxed);
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.dirty_roi = PixelRect{8, 0, 4, 4};
  {
    ScopedPreflightLifecycleObservation observation(execution_service);
    EXPECT_NO_THROW((void)service.compute_parallel(graph, runtime, request));
  }

  EXPECT_EQ(g_host_preparation_source_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_host_preparation_plugin_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_preflight_lifecycle_observations.load(std::memory_order_acquire),
            1);
  EXPECT_TRUE(
      g_preflight_observed_installed_bundle.load(std::memory_order_acquire));
  EXPECT_TRUE(
      g_preflight_observed_reserved_start.load(std::memory_order_acquire));
  const compute::ExecutionLifecyclePage settled =
      execution_service.lifecycle_snapshot(0U, 4096U);
  EXPECT_EQ(settled.counters.pending_candidate_count, 0U);
  EXPECT_EQ(settled.counters.admitted_standalone_run_count, 0U);
  EXPECT_EQ(settled.counters.live_root_reservation_count, 0U);
  EXPECT_EQ(execution_service.resource_snapshot().reserved, ResourceVector{});
  runtime.stop();
}

/**
 * @brief Proves connected-preflight cancellation suppresses dirty phase two.
 *
 * @return Nothing; GoogleTest assertions report dependent entry, publication,
 * or terminal-translation failures for HP and paired HP/RT requests.
 * @throws Setup, execution, graph, allocation, or image-adaptation exceptions
 * when the fixture itself cannot execute.
 * @note The serial-debug execution route runs source preflight. Its provider
 * requests cancellation immediately before return, so the parameter callback,
 * HP/RT target tiles, inspection publication, Graph caches, and RT proxy must
 * remain unchanged after bounded synchronous settlement.
 */
TEST(ComputeServiceCancellation,
     ConnectedPreflightCancellationSuppressesDirtyAndSiblingPublication) {
  register_split_ops();
  for (const ComputeIntent intent :
       {ComputeIntent::GlobalHighPrecision, ComputeIntent::RealTimeUpdate}) {
    const bool is_rt = intent == ComputeIntent::RealTimeUpdate;
    SCOPED_TRACE(is_rt ? "RT" : "HP");
    const std::string suffix = is_rt ? "rt" : "hp";
    const ScopedTestDirectory root(
        std::filesystem::temp_directory_path() /
        ("photospider-connected-preflight-cancellation-" + suffix));
    GraphRuntime::Info info;
    info.name = "connected-preflight-cancellation-" + suffix;
    info.root = root.path();
    info.cache_root = root.path() / "cache";
    GraphRuntime runtime(info);
    runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision,
                                    "serial_debug");
    runtime.replace_execution_route(ComputeIntent::RealTimeUpdate,
                                    "serial_debug");
    runtime.start();
    GraphModel& graph = runtime.model();
    populate_host_preparation_failure_graph(graph);
    GraphTraversalService traversal;
    GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                            testing::make_yaml_cache_metadata_codec()};
    GraphEventService events;
    compute::ExecutionService execution_service(1U);
    ComputeService service(traversal, cache, events, execution_service);
    testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                           graph);
    ComputeService::Request request;
    request.node_id = 2;
    request.cache.precision = "float32";
    request.cache.disable_disk_cache = true;
    (void)service.compute_parallel(graph, runtime, request);

    ASSERT_TRUE(graph.node(1).cached_output_high_precision.has_value());
    ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
    ASSERT_TRUE(graph.node(3).cached_output_high_precision.has_value());
    const cv::Mat source_pixels_before =
        toCvMat(graph.node(1).cached_output_high_precision->image_buffer)
            .clone();
    const cv::Mat target_pixels_before =
        toCvMat(graph.node(2).cached_output_high_precision->image_buffer)
            .clone();
    const int source_version_before = graph.node(1).hp_version;
    const int target_version_before = graph.node(2).hp_version;
    const int parameter_version_before = graph.node(3).hp_version;
    const std::optional<RegionSet> source_region_before =
        graph.node(1).hp_region;
    const std::optional<RegionSet> target_region_before =
        graph.node(2).hp_region;
    const std::optional<RegionSet> parameter_region_before =
        graph.node(3).hp_region;
    const std::size_t dirty_snapshots_before =
        graph.recent_dirty_region_snapshots.size();
    const std::size_t compute_plans_before = graph.recent_compute_plans.size();
    const std::size_t plan_summaries_before =
        graph.recent_compute_plan_summaries.size();
    const std::uint64_t request_generation_before =
        graph.dirty_generation_counter;
    ASSERT_EQ(runtime.realtime_proxy_graph().find_state(2), nullptr);

    g_host_preparation_emit_malformed_value.store(false,
                                                  std::memory_order_release);
    g_host_preparation_source_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_plugin_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_hp_target_calls.store(0, std::memory_order_relaxed);
    g_host_preparation_rt_target_calls.store(0, std::memory_order_relaxed);
    auto cancellation_source =
        std::make_shared<compute::ComputeRequestCancellationSource>();
    request.intent = intent;
    request.dirty_roi = PixelRect{8, 0, 4, 4};
    request.cancellation_source = cancellation_source;
    bool saw_cancellation = false;
    {
      ScopedHostPreparationCancellationSource cancellation_hook(
          cancellation_source);
      try {
        (void)service.compute_parallel(graph, runtime, request);
      } catch (const GraphError& error) {
        saw_cancellation = true;
        EXPECT_EQ(error.code(), GraphErrc::ComputeError);
        EXPECT_NE(std::string(error.what())
                      .find("ComputeRun cancelled: explicit request."),
                  std::string::npos)
            << error.what();
      }
    }

    EXPECT_TRUE(saw_cancellation);
    EXPECT_EQ(cancellation_source->accepted_reason(),
              compute::ComputeRunCancellationReason::ExplicitRequest);
    EXPECT_EQ(g_host_preparation_source_calls.load(std::memory_order_relaxed),
              1);
    EXPECT_EQ(g_host_preparation_plugin_calls.load(std::memory_order_relaxed),
              0)
        << "the dependent parameter callback must not enter";
    EXPECT_EQ(
        g_host_preparation_hp_target_calls.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(
        g_host_preparation_rt_target_calls.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(graph.dirty_generation_counter, request_generation_before + 1);
    EXPECT_EQ(graph.recent_dirty_region_snapshots.size(),
              dirty_snapshots_before);
    EXPECT_EQ(graph.recent_compute_plans.size(), compute_plans_before);
    EXPECT_EQ(graph.recent_compute_plan_summaries.size(),
              plan_summaries_before);
    EXPECT_EQ(graph.node(1).hp_version, source_version_before);
    EXPECT_EQ(graph.node(2).hp_version, target_version_before);
    EXPECT_EQ(graph.node(3).hp_version, parameter_version_before);
    EXPECT_EQ(graph.node(1).hp_region, source_region_before);
    EXPECT_EQ(graph.node(2).hp_region, target_region_before);
    EXPECT_EQ(graph.node(3).hp_region, parameter_region_before);
    EXPECT_DOUBLE_EQ(
        cv::norm(
            source_pixels_before,
            toCvMat(graph.node(1).cached_output_high_precision->image_buffer),
            cv::NORM_INF),
        0.0);
    EXPECT_DOUBLE_EQ(
        cv::norm(
            target_pixels_before,
            toCvMat(graph.node(2).cached_output_high_precision->image_buffer),
            cv::NORM_INF),
        0.0);
    EXPECT_EQ(runtime.realtime_proxy_graph().find_state(2), nullptr);
    runtime.stop();
  }
}

/**
 * @brief Proves nonparallel connected cancellation releases direct operation
 * authority and permits a later request.
 *
 * @return Nothing; GoogleTest reports cancellation typing, residue, or retry
 * failures.
 * @throws Graph, service, allocation, or provider exceptions unchanged.
 * @note The source requests cancellation immediately before returning while
 * its direct lease is active. The dependent provider must not enter, all
 * ledger state must settle, and an uncancelled retry must traverse both
 * preflight nodes plus phase two.
 */
TEST(ComputeServiceCancellation,
     NonparallelConnectedCancellationReleasesDirectAuthorityAndRecovers) {
  register_split_ops();
  GraphModel graph("cache/nonparallel-connected-direct-cancellation");
  populate_host_preparation_failure_graph(graph);
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  (void)service.compute(graph, request);

  g_host_preparation_source_calls.store(0, std::memory_order_relaxed);
  g_host_preparation_plugin_calls.store(0, std::memory_order_relaxed);
  g_host_preparation_hp_target_calls.store(0, std::memory_order_relaxed);
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.dirty_roi = PixelRect{8, 0, 4, 4};
  auto cancellation_source =
      std::make_shared<compute::ComputeRequestCancellationSource>();
  request.cancellation_source = cancellation_source;
  bool saw_cancellation = false;
  {
    ScopedHostPreparationCancellationSource cancellation_hook(
        cancellation_source);
    try {
      (void)service.compute(graph, request);
    } catch (const GraphError& error) {
      saw_cancellation = true;
      EXPECT_EQ(error.code(), GraphErrc::ComputeError);
      EXPECT_NE(std::string(error.what())
                    .find("ComputeRun cancelled: explicit request."),
                std::string::npos);
    }
  }
  EXPECT_TRUE(saw_cancellation);
  EXPECT_EQ(g_host_preparation_source_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_host_preparation_plugin_calls.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(g_host_preparation_hp_target_calls.load(std::memory_order_relaxed),
            0);
  EXPECT_EQ(execution_service.resource_snapshot().reserved, ResourceVector{});

  request.cancellation_source.reset();
  EXPECT_NO_THROW((void)service.compute(graph, request));
  EXPECT_GT(g_host_preparation_source_calls.load(std::memory_order_relaxed), 1);
  EXPECT_GT(g_host_preparation_plugin_calls.load(std::memory_order_relaxed), 0);
  EXPECT_GT(g_host_preparation_hp_target_calls.load(std::memory_order_relaxed),
            0);
  EXPECT_EQ(execution_service.resource_snapshot().reserved, ResourceVector{});
}

TEST(ComputeServiceSplit,
     ExecutionBackedRtRequestStabilizesDataOnlyProducerExactlyOnce) {
  register_split_ops();
  g_dynamic_parameter_fail.store(false, std::memory_order_release);
  g_dynamic_blur_ksize.store(3, std::memory_order_release);
  GraphRuntime::Info info;
  info.name = "split-parallel-data-only-preflight";
  info.root = "cache/split-parallel-data-only-preflight";
  info.cache_root = "cache/split-parallel-data-only-preflight/cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision,
                                  "serial_debug");
  runtime.replace_execution_route(ComputeIntent::RealTimeUpdate,
                                  "serial_debug");
  runtime.start();
  GraphModel& graph = runtime.model();
  populate_dynamic_blur_graph(graph);
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(1U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  (void)service.compute(graph, request);

  g_dynamic_parameter_calls.store(0, std::memory_order_relaxed);
  g_dynamic_blur_ksize.store(21, std::memory_order_release);
  request.intent = ComputeIntent::RealTimeUpdate;
  request.dirty_roi = (PixelRect{270, 16, 3, 3});
  auto compute_future = std::async(std::launch::async, [&]() {
    return &service.compute_parallel(graph, runtime, request);
  });
  ASSERT_EQ(compute_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready)
      << "initial execution preflight batch must not lose work";
  ASSERT_NE(compute_future.get(), nullptr);
  EXPECT_EQ(g_dynamic_parameter_calls.load(std::memory_order_relaxed), 1);
  ASSERT_GE(graph.recent_dirty_region_snapshots.size(), 2u);
  const auto end = graph.recent_dirty_region_snapshots.end();
  EXPECT_EQ((end - 1)->graph_generation, (end - 2)->graph_generation);
  runtime.stop();
}

TEST(ComputeServiceSplit,
     ExecutionPreflightFailureRetryStartsFreshBatchWithoutHanging) {
  register_split_ops();
  g_dynamic_parameter_fail.store(false, std::memory_order_release);
  g_dynamic_blur_ksize.store(3, std::memory_order_release);
  g_dynamic_parameter_calls.store(0, std::memory_order_relaxed);
  GraphRuntime::Info info;
  info.name = "split-preflight-failure-retry";
  info.root = "cache/split-preflight-failure-retry";
  info.cache_root = "cache/split-preflight-failure-retry/cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision,
                                  "serial_debug");
  runtime.replace_execution_route(ComputeIntent::RealTimeUpdate,
                                  "serial_debug");
  runtime.start();
  GraphModel& graph = runtime.model();
  populate_dynamic_blur_graph(graph);
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(1U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  (void)service.compute(graph, request);
  const int target_version_before = graph.node(2).hp_version;
  g_dynamic_parameter_calls.store(0, std::memory_order_relaxed);

  request.intent = ComputeIntent::RealTimeUpdate;
  request.dirty_roi = (PixelRect{270, 16, 3, 3});
  g_dynamic_parameter_fail.store(true, std::memory_order_release);
  auto failed = std::async(std::launch::async, [&]() {
    return &service.compute_parallel(graph, runtime, request);
  });
  ASSERT_EQ(failed.wait_for(std::chrono::seconds(2)), std::future_status::ready)
      << "a failed execution preflight batch must settle its wait";
  EXPECT_THROW((void)failed.get(), GraphError);
  EXPECT_EQ(graph.node(2).hp_version, target_version_before);

  g_dynamic_parameter_fail.store(false, std::memory_order_release);
  g_dynamic_blur_ksize.store(21, std::memory_order_release);
  auto retried = std::async(std::launch::async, [&]() {
    return &service.compute_parallel(graph, runtime, request);
  });
  ASSERT_EQ(retried.wait_for(std::chrono::seconds(2)),
            std::future_status::ready)
      << "retry must open a fresh execution batch after prior failure";
  NodeOutput* output = retried.get();
  ASSERT_NE(output, nullptr);
  EXPECT_GT(output->image_buffer.width, 0);
  EXPECT_EQ(g_dynamic_parameter_calls.load(std::memory_order_relaxed), 2);
  EXPECT_GT(graph.node(2).hp_version, target_version_before);
  runtime.stop();
}

TEST(TaskGraphPlanningSplit, UsesDomainSpecificMetadataForTileShape) {
  register_split_ops();
  GraphModel graph("cache/split-domain-specific-tile-shape");
  Node node = make_node(1, "split_plan", "domain_tile");
  node.parameters["width"] = 512;
  node.parameters["height"] = 16;
  graph.add_node(node);

  const auto hp_graph =
      expand_full_task_graph(graph, ComputeIntent::GlobalHighPrecision);
  const auto rt_graph =
      expand_full_task_graph(graph, ComputeIntent::RealTimeUpdate);

  std::vector<const compute::PlannedTask*> hp_tiles;
  for (const auto& task : hp_graph.task_graph.tasks) {
    if (task.kind == compute::PlannedTaskKind::Tile) {
      hp_tiles.push_back(&task);
    }
  }
  ASSERT_EQ(hp_tiles.size(), 2u);
  for (const auto* task : hp_tiles) {
    EXPECT_EQ(task->domain, compute::DirtyDomain::HighPrecision);
    EXPECT_EQ(task->tile_size, compute::kHpMacroTileSize);
  }

  std::vector<const compute::PlannedTask*> rt_tiles;
  for (const auto& task : rt_graph.task_graph.tasks) {
    if (task.kind == compute::PlannedTaskKind::Tile) {
      rt_tiles.push_back(&task);
    }
  }
  ASSERT_EQ(rt_tiles.size(), 32u);
  for (const auto* task : rt_tiles) {
    EXPECT_EQ(task->domain, compute::DirtyDomain::RealTime);
    EXPECT_EQ(task->tile_size, compute::kRtTileSize);
  }
}

TEST(TaskGraphPlanningSplit, RtDependencyPlanningUsesRtMetadata) {
  register_split_ops();
  GraphModel graph("cache/split-rt-domain-metadata-dependencies");
  Node source = make_node(1, "split_plan", "domain_tile");
  source.parameters["width"] = 64;
  source.parameters["height"] = 16;
  Node downstream = make_node(2, "split_plan", "domain_random_tile");
  downstream.parameters["width"] = 64;
  downstream.parameters["height"] = 16;
  downstream.parameters["radius"] = 16;
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(downstream);
  graph.validate_topology();

  compute::ComputeRequest request;
  request.intent = ComputeIntent::RealTimeUpdate;
  request.target_node_id = 2;
  const auto plan = node_cache_pruned_plan(graph, request, {1, 2});

  const compute::PlannedTask* middle_downstream_task = nullptr;
  for (const auto& task : plan.task_graph.tasks) {
    if (task.node_id == 2 && task.output_roi == (PixelRect{16, 0, 16, 16})) {
      middle_downstream_task = &task;
      break;
    }
  }
  ASSERT_NE(middle_downstream_task, nullptr);
  ASSERT_EQ(middle_downstream_task->dependency_task_ids.size(), 3u)
      << "RT random-access metadata expands the middle RT micro tile input "
         "ROI across three upstream RT micro tiles";
  for (int dependency_task_id : middle_downstream_task->dependency_task_ids) {
    const auto& upstream_task = plan.task_graph.tasks.at(dependency_task_id);
    EXPECT_EQ(upstream_task.node_id, 1);
    EXPECT_EQ(upstream_task.domain, compute::DirtyDomain::RealTime);
    EXPECT_EQ(upstream_task.tile_size, compute::kRtTileSize);
  }
}

TEST(TaskGraphPlanningSplit, CachesFullTaskGraphPerIntentAndTopology) {
  register_split_ops();
  GraphModel graph("cache/split-full-task-graph-cache");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  graph.add_node(source);

  const auto hp_first = compute::get_or_expand_full_task_graph(
      graph, ComputeIntent::GlobalHighPrecision);
  const auto hp_second = compute::get_or_expand_full_task_graph(
      graph, ComputeIntent::GlobalHighPrecision);
  const auto rt_first = compute::get_or_expand_full_task_graph(
      graph, ComputeIntent::RealTimeUpdate);
  EXPECT_EQ(hp_first.get(), hp_second.get());
  EXPECT_NE(hp_first.get(), rt_first.get())
      << "HP and RT keep sibling task graphs with separate task pools";

  Node downstream = make_node(2, "split_plan", "tile");
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(downstream);
  const auto hp_after_topology_change = compute::get_or_expand_full_task_graph(
      graph, ComputeIntent::GlobalHighPrecision);
  EXPECT_NE(hp_first.get(), hp_after_topology_change.get());
}

TEST(TaskGraphPlanningSplit, ForceRecacheClearsFullTaskGraphCacheBeforePlan) {
  register_split_ops();
  GraphRuntime::Info info;
  info.name = "split-force-recache-task-graph-cache";
  info.root = "cache/split-force-recache-task-graph-cache";
  info.cache_root = "cache/split-force-recache-task-graph-cache/cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision,
                                  "serial_debug");
  runtime.start();

  GraphModel& graph = runtime.model();
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  graph.add_node(source);

  const auto cached_before = compute::get_or_expand_full_task_graph(
      graph, ComputeIntent::GlobalHighPrecision);
  ASSERT_NE(cached_before, nullptr);

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(1U);
  ComputeService compute(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  ComputeService::Request request;
  request.node_id = 1;
  request.cache.precision = "float32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  NodeOutput& output = compute.compute_parallel(graph, runtime, request);
  EXPECT_EQ(output.image_buffer.width, 32);
  EXPECT_EQ(output.image_buffer.height, 16);

  const auto cached_after = compute::get_or_expand_full_task_graph(
      graph, ComputeIntent::GlobalHighPrecision);
  EXPECT_NE(cached_before.get(), cached_after.get())
      << "force-recache must discard stale task ROIs before planning";
  runtime.stop();
}

TEST(GraphCacheServiceSplit,
     SkipsOpaqueBackendImageWhilePersistingNamedOutputMetadata) {
  const ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                                 "photospider-backend-cache-persistence");
  GraphModel graph(root.path());
  Node node = make_node(1, "split_plan", "source");
  node.caches.push_back({"image", "output.png"});
  node.cached_output_high_precision = NodeOutput{};
  node.cached_output_high_precision->image_buffer.width = 8;
  node.cached_output_high_precision->image_buffer.height = 8;
  node.cached_output_high_precision->image_buffer.channels = 1;
  node.cached_output_high_precision->image_buffer.device = Device::GPU_CUDA;
  node.cached_output_high_precision->image_buffer.context =
      std::make_shared<int>(7);
  node.cached_output_high_precision->data["marker"] = 9;
  node.hp_region = value_image_adapter::full_node_output_region(
      *node.cached_output_high_precision);
  graph.add_node(node);

  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  cache.save_cache_if_configured(graph, graph.node(1), "int8");
  const auto image_path = cache.node_cache_dir(graph, 1) / "output.png";
  auto metadata_path = image_path;
  metadata_path.replace_extension(".yml");
  EXPECT_FALSE(std::filesystem::exists(image_path));
  EXPECT_TRUE(std::filesystem::exists(metadata_path));
}

TEST(DownsampleExecutorSplit,
     RepeatedOpaqueBackendPassthroughPreservesCompleteDescriptor) {
  GraphModel graph("cache/split-opaque-downsample-passthrough");
  Node node = make_node(1, "split_plan", "opaque_backend");
  auto backend_owner = std::make_shared<int>(42);
  std::shared_ptr<void> expected_owner = backend_owner;
  node.cached_output_high_precision = NodeOutput{};
  ImageBuffer& hp = node.cached_output_high_precision->image_buffer;
  hp.width = 96;
  hp.height = 48;
  hp.channels = 4;
  hp.type = DataType::UINT8;
  hp.device = Device::GPU_CUDA;
  hp.context = expected_owner;
  node.cached_output_high_precision->data["marker"] = 17;
  node.hp_version = 5;
  graph.add_node(node);

  compute::RealtimeProxyGraph proxy;
  proxy.synchronize_with_graph(graph);
  GraphEventService events;
  compute::DownsampleExecutor downsample(graph, proxy, nullptr, events);
  downsample.execute({{1, (PixelRect{8, 4, 16, 8}), 5}});

  const auto assert_passthrough = [&](int version,
                                      const PixelRect& expected_roi) {
    const auto* state = proxy.find_state(1);
    ASSERT_NE(state, nullptr);
    ASSERT_TRUE(state->output.has_value());
    const ImageBuffer& output = state->output->image_buffer;
    EXPECT_EQ(output.width, 96);
    EXPECT_EQ(output.height, 48);
    EXPECT_EQ(output.channels, 4);
    EXPECT_EQ(output.type, DataType::UINT8);
    EXPECT_EQ(output.device, Device::GPU_CUDA);
    EXPECT_EQ(output.data, nullptr);
    ASSERT_NE(output.context, nullptr);
    EXPECT_EQ(output.context.get(), expected_owner.get());
    EXPECT_FALSE(output.context.owner_before(expected_owner));
    EXPECT_FALSE(expected_owner.owner_before(output.context));
    EXPECT_EQ(state->output->data.at("marker").as_int64(), 17);
    EXPECT_EQ(state->version, version);
    ASSERT_TRUE(state->region_hp.has_value());
    EXPECT_EQ(*state->region_hp,
              RegionSet::from_image_rect(
                  {image_region_domain(), expected_roi.x,
                   expected_roi.x + expected_roi.width, expected_roi.y,
                   expected_roi.y + expected_roi.height}));
  };
  assert_passthrough(5, (PixelRect{8, 4, 16, 8}));

  graph.mutate_node_runtime_state(1, [](auto& state) { state.hp_version = 6; });
  downsample.execute({{1, (PixelRect{40, 20, 8, 4}), 6}});
  assert_passthrough(6, (PixelRect{40, 20, 8, 4}));

  const ComputeEventBatch recorded = events.drain(kComputeEventDrainMaxLimit);
  ASSERT_EQ(recorded.events.size(), 2u);
  EXPECT_TRUE(std::all_of(recorded.events.begin(), recorded.events.end(),
                          [](const ComputeEventSnapshot& event) {
                            return event.source == "downsample_passthrough";
                          }));
}

/**
 * @brief Proves a Whole parallel read recomputes an exact-partial producer.
 *
 * @return Nothing; GoogleTest reports producer execution, dependency value,
 * validity, or final-output failures.
 * @throws Graph, runtime, cache, allocation, Region, or image-adaptation
 * exceptions when the production fixture cannot execute.
 * @note The test publishes value 91 through the real HP dirty write buffer with
 * partial TensorSlice validity. The planned producer must execute and publish
 * value 5 before its whole-output consumer runs; raw optional-presence checks
 * would instead bypass the producer and expose 91 downstream.
 */
TEST(ComputeTaskRunnerSplit,
     ParallelWholeReadRecomputesPartialProducerBeforeConsumer) {
  register_split_ops();
  const ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                                 "photospider-partial-producer-whole-read");
  GraphRuntime::Info info;
  info.name = "partial-producer-whole-read";
  info.root = root.path();
  info.cache_root = root.path() / "cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
  runtime.start();

  GraphModel& graph = runtime.model();
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 4;
  source.parameters["height"] = 4;
  source.cached_output_high_precision = make_image_output(4, 4, 1, 3.0f);
  source.hp_region = value_image_adapter::full_node_output_region(
      *source.cached_output_high_precision);

  Node producer = make_node(2, "split_plan", "partial_cache_producer");
  producer.image_inputs.push_back({1, "image"});
  Node consumer = make_node(3, "split_plan", "partial_cache_consumer");
  consumer.image_inputs.push_back({2, "image"});

  graph.add_node(source);
  graph.add_node(producer);
  graph.add_node(consumer);
  graph.validate_topology();

  const RegionSet partial_region = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 2U}, {0U, 4U}, {0U, 1U}}});
  compute::HighPrecisionDirtyWriteBuffer dirty_buffer(false);
  NodeOutput& staged = dirty_buffer.ensure_output(graph.node(2));
  staged = make_image_output(4, 4, 1, 91.0f);
  (void)dirty_buffer.mark_updated(graph.node(2), partial_region, true, 1U);
  dirty_buffer.commit_to_graph(graph);

  ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
  ASSERT_TRUE(graph.node(2).hp_region.has_value());
  EXPECT_EQ(*graph.node(2).hp_region, partial_region);
  EXPECT_FALSE(compute::ComputeCachePolicy::has_reusable_output(graph.node(2)));
  EXPECT_FLOAT_EQ(
      toCvMat(graph.node(2).cached_output_high_precision->image_buffer)
          .at<float>(0, 0),
      91.0f);

  g_partial_cache_producer_calls.store(0, std::memory_order_relaxed);
  g_partial_cache_consumer_calls.store(0, std::memory_order_relaxed);
  g_partial_cache_consumer_observed_value.store(0, std::memory_order_relaxed);

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(2U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  ComputeService::Request request;
  request.node_id = 3;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  NodeOutput& output = service.compute_parallel(graph, runtime, request);

  EXPECT_EQ(g_partial_cache_producer_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_partial_cache_consumer_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(
      g_partial_cache_consumer_observed_value.load(std::memory_order_acquire),
      5);
  EXPECT_FLOAT_EQ(toCvMat(output.image_buffer).at<float>(0, 0), 5.0f);
  EXPECT_TRUE(compute::ComputeCachePolicy::has_reusable_output(graph.node(2)));
  EXPECT_TRUE(compute::ComputeCachePolicy::has_reusable_output(graph.node(3)));
  ASSERT_TRUE(graph.node(2).hp_region.has_value());
  EXPECT_FALSE(*graph.node(2).hp_region == partial_region);

  runtime.stop();
}

/**
 * @brief Proves a tiled callback receives its own exact sibling metadata.
 *
 * @return Nothing; GoogleTest reports callback choice, tile count, input ROI,
 * output, or cache publication mismatches.
 * @throws Graph, runtime, cache, registry, allocation, or image-adaptation
 * exceptions when the product parallel path cannot execute.
 * @note The cached source removes producer work from this request. Two MICRO
 * target tiles must each receive the complete source ROI through the selected
 * RandomAccess device implementation; a generic same-key lookup would expose
 * the monolithic SpatialAligned sibling metadata.
 */
TEST(ComputeTaskRunnerSplit,
     SelectedTiledSiblingExecutesWithExactMetadataSnapshot) {
  register_split_ops();
  g_exact_sibling_tiled_calls.store(0, std::memory_order_relaxed);
  g_exact_sibling_input_roi_mismatches.store(0, std::memory_order_relaxed);
  g_exact_sibling_monolithic_calls.store(0, std::memory_order_relaxed);

  const ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                                 "photospider-exact-sibling-metadata");
  GraphRuntime::Info info;
  info.name = "exact-sibling-metadata";
  info.root = root.path();
  info.cache_root = root.path() / "cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
  runtime.start();

  GraphModel& graph = runtime.model();
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  source.cached_output_high_precision = make_image_output(32, 16, 1, 3.0f);
  source.hp_region = value_image_adapter::full_node_output_region(
      *source.cached_output_high_precision);
  source.hp_version = 1;
  Node target = make_node(2, "split_plan", "exact_sibling_metadata");
  target.parameters["width"] = 32;
  target.parameters["height"] = 16;
  target.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(target);
  graph.validate_topology();

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(2U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  NodeOutput& output = service.compute_parallel(graph, runtime, request);

  EXPECT_EQ(g_exact_sibling_tiled_calls.load(std::memory_order_relaxed), 2);
  EXPECT_EQ(
      g_exact_sibling_input_roi_mismatches.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(g_exact_sibling_monolithic_calls.load(std::memory_order_relaxed),
            0);
  ASSERT_EQ(output.image_buffer.width, 32);
  ASSERT_EQ(output.image_buffer.height, 16);
  EXPECT_FLOAT_EQ(toCvMat(output.image_buffer).at<float>(0, 0), 13.0f);
  EXPECT_FLOAT_EQ(toCvMat(output.image_buffer).at<float>(0, 31), 13.0f);
  EXPECT_TRUE(compute::ComputeCachePolicy::has_reusable_output(graph.node(2)));

  runtime.stop();
}

TEST(ComputeTaskRunnerSplit, TiledDiskCacheHitStopsSiblingTileTasks) {
  register_split_ops();
  g_disk_cache_guard_tile_calls.store(0, std::memory_order_relaxed);

  const std::filesystem::path root = "cache/split-tiled-disk-cache-hit-guard";
  std::filesystem::remove_all(root);
  GraphRuntime::Info info;
  info.name = "split-tiled-disk-cache-hit-guard";
  info.root = root;
  info.cache_root = root / "cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
  runtime.start();

  GraphModel& graph = runtime.model();
  Node cached_tile = make_node(1, "split_plan", "disk_cache_guard_tile");
  cached_tile.parameters["width"] = 256;
  cached_tile.parameters["height"] = 256;
  cached_tile.caches.push_back({"image", "output.png"});
  cached_tile.cached_output_high_precision =
      make_image_output(256, 256, 1, 64.0f / 255.0f);
  cached_tile.hp_region = value_image_adapter::full_node_output_region(
      *cached_tile.cached_output_high_precision);
  graph.add_node(cached_tile);
  graph.validate_topology();

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  cache.save_cache_if_configured(graph, graph.node(1), "int8");
  const auto cache_file = cache.node_cache_dir(graph, 1) / "output.png";
  ASSERT_TRUE(std::filesystem::exists(cache_file));
  graph.mutate_node_runtime_state(1, [](auto& state) {
    state.cached_output_high_precision.reset();
    state.hp_region.reset();
  });

  compute::ExecutionService execution_service(1U);
  ComputeService compute(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);
  ComputeService::Request request;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.telemetry.enable_timing = true;
  NodeOutput& output = compute.compute_parallel(graph, runtime, request);

  const auto result = graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, GraphModel::DiskCacheLoadStatus::Hit);
  EXPECT_EQ(result->node_id, 1);
  EXPECT_EQ(g_disk_cache_guard_tile_calls.load(std::memory_order_relaxed), 0)
      << "disk-cache hit must stop sibling tile tasks before tile execution";

  ASSERT_TRUE(graph.node(1).cached_output_high_precision.has_value());
  EXPECT_EQ(&output, &*graph.node(1).cached_output_high_precision);
  EXPECT_EQ(graph.node(1).hp_version, 1);
  ASSERT_TRUE(graph.node(1).hp_region.has_value());
  EXPECT_EQ(
      *graph.node(1).hp_region,
      RegionSet::from_image_rect({image_region_domain(), 0, 256, 0, 256}));
  ASSERT_EQ(output.image_buffer.width, 256);
  ASSERT_EQ(output.image_buffer.height, 256);
  const cv::Mat output_mat = toCvMat(output.image_buffer);
  ASSERT_EQ(output_mat.rows, 256);
  ASSERT_EQ(output_mat.cols, 256);
  EXPECT_NEAR(output_mat.at<float>(0, 0), 64.0f / 255.0f, 1.0f / 255.0f);
  EXPECT_NEAR(output_mat.at<float>(128, 128), 64.0f / 255.0f, 1.0f / 255.0f);
  EXPECT_NEAR(output_mat.at<float>(255, 255), 64.0f / 255.0f, 1.0f / 255.0f);

  runtime.stop();
  std::filesystem::remove_all(root);
}

TEST(TaskGraphPlanningSplit,
     DirtySnapshotTaskGraphPrunerExcludesSourceBoundaryTasks) {
  register_split_ops();
  GraphModel graph("cache/split-dirty-pruner");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(downstream);
  graph.validate_topology();

  compute::DirtyRegionSnapshot snapshot;
  snapshot.graph_generation = 3;
  snapshot.dirty_source_nodes.push_back(1);
  snapshot.source_roi_records[1].push_back(
      {1, compute::DirtyDomain::HighPrecision, (PixelRect{0, 0, 16, 16}), 3});
  snapshot.per_node_dirty_rois[2].push_back((PixelRect{0, 0, 16, 16}));
  snapshot.actual_dirty_rois = snapshot.per_node_dirty_rois;

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 2;
  request.dirty_roi = (PixelRect{0, 0, 16, 16});
  const auto base_plan = node_cache_pruned_plan(graph, request, {1, 2});
  const auto plan = dirty_snapshot_pruned_plan(base_plan, snapshot, graph);

  compute::DirtySnapshotTaskGraphPruner pruner;
  const auto selection = pruner.select(base_plan, snapshot, graph);
  const auto work_set = pruner.materialize(selection);
  EXPECT_EQ(work_set.generation, 3u);
  EXPECT_EQ(selection.generation, 3u);
  EXPECT_EQ(selection.active_task_ids.size(), 2u);
  ASSERT_EQ(work_set.dirty_source_task_ids.size(), 1u);
  const auto& source_task =
      base_plan.task_graph.tasks.at(work_set.dirty_source_task_ids.front());
  EXPECT_EQ(source_task.node_id, 1);
  EXPECT_EQ(source_task.output_roi, (PixelRect{0, 0, 16, 16}));
  EXPECT_TRUE(selection.active_task_flags.at(source_task.task_id));
  EXPECT_TRUE(selection.source_boundary_task_flags.at(source_task.task_id));
  ASSERT_EQ(work_set.downstream_task_ids.size(), 1u);
  const auto& downstream_task =
      base_plan.task_graph.tasks.at(work_set.downstream_task_ids.front());
  EXPECT_EQ(downstream_task.node_id, 2);
  EXPECT_TRUE(selection.active_task_flags.at(downstream_task.task_id));
  EXPECT_FALSE(
      selection.source_boundary_task_flags.at(downstream_task.task_id));

  compute::TaskGraphReadyChecker ready_checker;
  const auto ready = ready_checker.initial_ready_task_ids(
      plan.task_graph, &work_set.downstream_task_ids);
  EXPECT_EQ(ready, work_set.downstream_task_ids)
      << "source-boundary dependencies are satisfied by the source lane";
  EXPECT_EQ(selection.initial_downstream_task_ids, work_set.downstream_task_ids)
      << "overlay ready set must preserve task-level source/downstream split";
}

TEST(TaskGraphPlanningSplit,
     DirtySnapshotTaskGraphPrunerFiltersCrossDomainEdges) {
  register_split_ops();
  GraphModel graph("cache/split-cross-domain-pruner");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 64;
  source.parameters["height"] = 16;
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.parameters["width"] = 64;
  downstream.parameters["height"] = 16;
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(downstream);
  graph.validate_topology();

  compute::DirtyRegionSnapshot snapshot;
  snapshot.graph_generation = 11;
  snapshot.per_node_dirty_rois[1].push_back((PixelRect{0, 0, 64, 64}));
  snapshot.per_node_dirty_rois[2].push_back((PixelRect{0, 0, 64, 64}));
  snapshot.dirty_tiles.push_back({1, compute::DirtyDomain::RealTime,
                                  compute::DirtyTileLevel::Micro, 0, 0, 16,
                                  (PixelRect{0, 0, 16, 16})});
  snapshot.dirty_tiles.push_back({2, compute::DirtyDomain::RealTime,
                                  compute::DirtyTileLevel::Micro, 1, 0, 16,
                                  (PixelRect{16, 0, 16, 16})});
  snapshot.edge_mappings.push_back(
      {1, 2, compute::DirtyDomain::RealTime, (PixelRect{0, 0, 64, 64}),
       (PixelRect{0, 0, 64, 64}), compute::DirtyEdgeDirection::BackwardDemand});
  snapshot.edge_mappings.push_back(
      {1, 2, compute::DirtyDomain::HighPrecision, (PixelRect{0, 0, 64, 64}),
       (PixelRect{0, 0, 64, 64}), compute::DirtyEdgeDirection::BackwardDemand});

  compute::ComputeRequest request;
  request.intent = ComputeIntent::RealTimeUpdate;
  request.target_node_id = 2;
  request.dirty_roi = (PixelRect{0, 0, 64, 64});

  const auto base_plan = node_cache_pruned_plan(graph, request, {1, 2});
  const auto plan = dirty_snapshot_pruned_plan(base_plan, snapshot, graph);
  compute::DirtySnapshotTaskGraphPruner pruner;
  const auto selection = pruner.select(base_plan, snapshot, graph);

  ASSERT_EQ(plan.task_graph.dependencies.size(), 1u);
  EXPECT_EQ(plan.task_graph.dependencies[0].domain,
            compute::DirtyDomain::RealTime);
  EXPECT_EQ(plan.task_graph.dependencies[0].from_roi,
            (PixelRect{0, 0, 64, 64}));
  ASSERT_EQ(selection.dependencies.size(), 1u);
  EXPECT_EQ(selection.dependencies[0].domain, compute::DirtyDomain::RealTime);
  EXPECT_EQ(selection.dependencies[0].from_roi, (PixelRect{0, 0, 64, 64}));
  ASSERT_EQ(plan.task_graph.tasks.size(), 8u);
  for (const auto& task : plan.task_graph.tasks) {
    EXPECT_EQ(task.domain, compute::DirtyDomain::RealTime);
  }
}

TEST(IntentUpdateCoordinatorSplit,
     ValidatesRtDirtyRoiAndCoordinatesRtFirstConcurrency) {
  EXPECT_THROW(compute::IntentUpdateCoordinator::validate(
                   ComputeIntent::RealTimeUpdate, std::nullopt),
               GraphError);
  EXPECT_NO_THROW(compute::IntentUpdateCoordinator::validate(
      ComputeIntent::RealTimeUpdate, (PixelRect{0, 0, 4, 4})));

  auto decision = compute::IntentUpdateCoordinator::decide(
      ComputeIntent::RealTimeUpdate, true, true);
  EXPECT_TRUE(decision.requires_dirty_roi);
  EXPECT_TRUE(decision.run_high_precision_update);
  EXPECT_TRUE(decision.run_real_time_update);
  EXPECT_TRUE(decision.submit_updates_concurrently);

  auto inline_decision = compute::IntentUpdateCoordinator::decide(
      ComputeIntent::RealTimeUpdate, false, true);
  EXPECT_TRUE(inline_decision.requires_dirty_roi);
  EXPECT_TRUE(inline_decision.run_high_precision_update);
  EXPECT_TRUE(inline_decision.run_real_time_update);
  EXPECT_FALSE(inline_decision.submit_updates_concurrently);

  std::atomic_bool ran_hp{false};
  std::atomic_bool ran_rt{false};
  std::atomic_int active_callbacks{0};
  std::atomic_int max_active_callbacks{0};
  bool ran_global_dirty = false;
  std::vector<std::string> stages;
  std::mutex stages_mutex;
  NodeOutput rt_output = make_image_output(4, 4);
  auto update_max_active = [&]() {
    const int active = active_callbacks.fetch_add(1) + 1;
    int observed = max_active_callbacks.load();
    while (active > observed &&
           !max_active_callbacks.compare_exchange_weak(observed, active)) {
    }
  };
  compute::IntentUpdateCallbacks callbacks;
  callbacks.run_global_high_precision = [&]() -> NodeOutput& {
    return rt_output;
  };
  callbacks.run_global_high_precision_dirty_update = [&]() -> NodeOutput& {
    ran_global_dirty = true;
    return rt_output;
  };
  callbacks.run_high_precision_update = [&]() {
    update_max_active();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ran_hp.store(true);
    active_callbacks.fetch_sub(1);
  };
  callbacks.run_real_time_update = [&]() -> NodeOutput& {
    update_max_active();
    ran_rt.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    active_callbacks.fetch_sub(1);
    return rt_output;
  };
  callbacks.real_time_output = [&]() -> NodeOutput& { return rt_output; };
  callbacks.record_stage = [&](const std::string& stage) {
    std::lock_guard<std::mutex> lock(stages_mutex);
    stages.push_back(stage);
  };

  NodeOutput& coordinated =
      compute::IntentUpdateCoordinator::coordinate_intent_update(
          ComputeIntent::RealTimeUpdate, false, (PixelRect{0, 0, 4, 4}),
          callbacks);
  EXPECT_EQ(&coordinated, &rt_output);
  EXPECT_TRUE(ran_hp.load());
  EXPECT_TRUE(ran_rt.load());
  EXPECT_NE(std::find(stages.begin(), stages.end(),
                      "intent_coordinator_decision_inline"),
            stages.end());
  EXPECT_NE(
      std::find(stages.begin(), stages.end(), "intent_coordinator_inline_hp"),
      stages.end());
  EXPECT_NE(
      std::find(stages.begin(), stages.end(), "intent_coordinator_inline_rt"),
      stages.end());
  auto inline_rt_stage =
      std::find(stages.begin(), stages.end(), "intent_coordinator_inline_rt");
  auto inline_hp_stage =
      std::find(stages.begin(), stages.end(), "intent_coordinator_inline_hp");
  ASSERT_NE(inline_rt_stage, stages.end());
  ASSERT_NE(inline_hp_stage, stages.end());
  EXPECT_LT(std::distance(stages.begin(), inline_rt_stage),
            std::distance(stages.begin(), inline_hp_stage));

  stages.clear();
  NodeOutput& coordinated_global_dirty =
      compute::IntentUpdateCoordinator::coordinate_intent_update(
          ComputeIntent::GlobalHighPrecision, false, (PixelRect{0, 0, 4, 4}),
          callbacks);
  EXPECT_EQ(&coordinated_global_dirty, &rt_output);
  EXPECT_TRUE(ran_global_dirty);
  EXPECT_NE(std::find(stages.begin(), stages.end(),
                      "intent_coordinator_global_dirty_update"),
            stages.end());
  EXPECT_FALSE(
      std::any_of(stages.begin(), stages.end(), [](const std::string& stage) {
        return stage.find("full_recompute") != std::string::npos;
      }));

  ran_hp.store(false);
  ran_rt.store(false);
  stages.clear();
  NodeOutput& coordinated_without_runtime =
      compute::IntentUpdateCoordinator::coordinate_intent_update(
          ComputeIntent::RealTimeUpdate, false, (PixelRect{0, 0, 4, 4}),
          callbacks);
  EXPECT_EQ(&coordinated_without_runtime, &rt_output);
  EXPECT_TRUE(ran_hp.load());
  EXPECT_TRUE(ran_rt.load());
  EXPECT_NE(std::find(stages.begin(), stages.end(),
                      "intent_coordinator_decision_inline"),
            stages.end());
  inline_rt_stage =
      std::find(stages.begin(), stages.end(), "intent_coordinator_inline_rt");
  inline_hp_stage =
      std::find(stages.begin(), stages.end(), "intent_coordinator_inline_hp");
  ASSERT_NE(inline_rt_stage, stages.end());
  ASSERT_NE(inline_hp_stage, stages.end());
  EXPECT_LT(std::distance(stages.begin(), inline_rt_stage),
            std::distance(stages.begin(), inline_hp_stage));

  ran_hp.store(false);
  ran_rt.store(false);
  active_callbacks.store(0);
  max_active_callbacks.store(0);
  stages.clear();
  std::mutex concurrent_callbacks_mutex;
  std::condition_variable concurrent_callbacks_cv;
  bool hp_callback_entered = false;
  bool rt_callback_entered = false;
  bool concurrent_callbacks_timed_out = false;
  auto mark_concurrent_callback_entered = [&](bool is_rt_callback) {
    std::unique_lock<std::mutex> lock(concurrent_callbacks_mutex);
    if (is_rt_callback) {
      rt_callback_entered = true;
    } else {
      hp_callback_entered = true;
    }
    concurrent_callbacks_cv.notify_all();
    if (!concurrent_callbacks_cv.wait_for(lock, std::chrono::seconds(2), [&]() {
          return hp_callback_entered && rt_callback_entered;
        })) {
      concurrent_callbacks_timed_out = true;
    }
  };
  callbacks.run_high_precision_update = [&]() {
    update_max_active();
    mark_concurrent_callback_entered(false);
    ran_hp.store(true);
    active_callbacks.fetch_sub(1);
  };
  callbacks.run_real_time_update = [&]() -> NodeOutput& {
    update_max_active();
    mark_concurrent_callback_entered(true);
    ran_rt.store(true);
    active_callbacks.fetch_sub(1);
    return rt_output;
  };
  NodeOutput& coordinated_with_runtimes =
      compute::IntentUpdateCoordinator::coordinate_intent_update(
          ComputeIntent::RealTimeUpdate, true, (PixelRect{0, 0, 4, 4}),
          callbacks);
  EXPECT_EQ(&coordinated_with_runtimes, &rt_output);
  EXPECT_TRUE(ran_hp.load());
  EXPECT_TRUE(ran_rt.load());
  EXPECT_NE(std::find(stages.begin(), stages.end(),
                      "intent_coordinator_decision_concurrent"),
            stages.end());
  auto concurrent_rt_start = std::find(
      stages.begin(), stages.end(), "intent_coordinator_concurrent_rt_start");
  auto concurrent_hp_start = std::find(
      stages.begin(), stages.end(), "intent_coordinator_concurrent_hp_start");
  ASSERT_NE(concurrent_rt_start, stages.end());
  ASSERT_NE(concurrent_hp_start, stages.end());
  EXPECT_LT(std::distance(stages.begin(), concurrent_rt_start),
            std::distance(stages.begin(), concurrent_hp_start));
  EXPECT_TRUE(rt_callback_entered);
  EXPECT_TRUE(hp_callback_entered);
  EXPECT_FALSE(concurrent_callbacks_timed_out);
  EXPECT_GE(max_active_callbacks.load(), 2);
}

TEST(RealtimeProxyWriteBuffer, StagesDeepCopyAndCommitsToProxyGraph) {
  GraphModel graph("cache/rt-proxy-write-buffer");
  Node node = make_node(1, "split_plan", "tile");
  graph.add_node(node);

  compute::RealtimeProxyGraph proxy_graph;
  proxy_graph.synchronize_with_graph(graph);
  compute::RealtimeProxyGraph::NodeState initial_state;
  initial_state.output = make_image_output(4, 4, 1, 3.0f);
  initial_state.version = 7;
  initial_state.region_hp =
      RegionSet::from_image_rect({image_region_domain(), 0, 1, 0, 1});
  proxy_graph.commit_node_state(1, std::move(initial_state));

  compute::RealtimeProxyWriteBuffer buffer(proxy_graph);
  NodeOutput& staged = buffer.ensure_output(1);
  toCvMat(staged.image_buffer).setTo(9.0f);
  buffer.mark_updated(
      1, RegionSet::from_image_rect({image_region_domain(), 1, 3, 1, 3}), true,
      42);

  ASSERT_NE(proxy_graph.find_output(1), nullptr);
  EXPECT_FLOAT_EQ(
      toCvMat(proxy_graph.find_output(1)->image_buffer).at<float>(0, 0), 3.0f);
  ASSERT_EQ(graph.find_node(1)->cached_output_high_precision, std::nullopt);

  buffer.commit_to_proxy_graph();

  const auto* committed_state = proxy_graph.find_state(1);
  ASSERT_NE(committed_state, nullptr);
  ASSERT_TRUE(committed_state->output.has_value());
  EXPECT_FLOAT_EQ(
      toCvMat(committed_state->output->image_buffer).at<float>(0, 0), 9.0f);
  EXPECT_EQ(committed_state->version, 8);
  EXPECT_EQ(committed_state->region_hp,
            RegionSet::from_image_rect({image_region_domain(), 1, 3, 1, 3}))
      << "a corner-touching union is not one exact rectangle; proxy validity "
         "must retain the fresh update instead of a false bounding superset";
  ASSERT_TRUE(committed_state->dirty_source_generation.has_value());
  EXPECT_EQ(*committed_state->dirty_source_generation, 42u);
}

/**
 * @brief Verifies isolated proxy cloning, no-throw publication, and reset.
 * @return Nothing.
 * @throws Test fixture allocation and Graph mutation exceptions unchanged.
 * @note Publication swaps the complete prepared proxy state while leaving the
 * displaced visible state in the request-owned snapshot.
 */
TEST(RealtimeProxyGraph,
     ClonesPublishesAndResetsAcrossGraphTopologyGenerations) {
  GraphModel graph("cache/rt-proxy-generation-reset");
  Node node = make_node(1, "split_plan", "tile");
  graph.add_node(node);

  compute::RealtimeProxyGraph proxy_graph;
  proxy_graph.synchronize_with_graph(graph);
  compute::RealtimeProxyGraph::NodeState initial_state;
  initial_state.output = make_image_output(4, 4, 1, 3.0f);
  initial_state.version = 7;
  initial_state.region_hp =
      RegionSet::from_image_rect({image_region_domain(), 0, 4, 0, 4});
  initial_state.dirty_source_generation = 42;
  proxy_graph.commit_node_state(1, std::move(initial_state));

  proxy_graph.synchronize_with_graph(graph);
  const auto* preserved_state = proxy_graph.find_state(1);
  ASSERT_NE(preserved_state, nullptr);
  ASSERT_TRUE(preserved_state->output.has_value());
  EXPECT_EQ(preserved_state->version, 7);
  ASSERT_TRUE(preserved_state->dirty_source_generation.has_value());
  EXPECT_EQ(*preserved_state->dirty_source_generation, 42u);

  std::unique_ptr<compute::RealtimeProxyGraph> snapshot =
      proxy_graph.clone_for_compute();
  const auto* cloned_state = snapshot->find_state(1);
  ASSERT_NE(cloned_state, nullptr);
  compute::RealtimeProxyGraph::NodeState prepared_state = *cloned_state;
  prepared_state.version = 11;
  prepared_state.dirty_source_generation = 77;
  snapshot->commit_node_state(1, std::move(prepared_state));
  ASSERT_NE(proxy_graph.find_state(1), nullptr);
  EXPECT_EQ(proxy_graph.find_state(1)->version, 7);
  static_assert(noexcept(proxy_graph.publish_compute_snapshot(*snapshot)));
  proxy_graph.publish_compute_snapshot(*snapshot);
  ASSERT_NE(proxy_graph.find_state(1), nullptr);
  EXPECT_EQ(proxy_graph.find_state(1)->version, 11);
  ASSERT_NE(snapshot->find_state(1), nullptr);
  EXPECT_EQ(snapshot->find_state(1)->version, 7);

  GraphModel::NodeMap replacement_nodes;
  Node replacement = make_node(1, "split_plan", "domain_tile");
  replacement.parameters["width"] = 16;
  replacement.parameters["height"] = 16;
  replacement_nodes.emplace(1, std::move(replacement));
  graph.replace_nodes(std::move(replacement_nodes));
  proxy_graph.synchronize_with_graph(graph);

  const auto* replaced_state = proxy_graph.find_state(1);
  ASSERT_NE(replaced_state, nullptr);
  EXPECT_FALSE(replaced_state->output.has_value());
  EXPECT_EQ(replaced_state->version, 0);
  EXPECT_FALSE(replaced_state->dirty_source_generation.has_value());

  compute::RealtimeProxyGraph::NodeState stale_after_replacement;
  stale_after_replacement.output = make_image_output(4, 4, 1, 9.0f);
  stale_after_replacement.version = 3;
  stale_after_replacement.dirty_source_generation = 88;
  proxy_graph.commit_node_state(1, std::move(stale_after_replacement));

  graph.clear();
  Node reloaded = make_node(1, "split_plan", "tile");
  reloaded.parameters["width"] = 16;
  reloaded.parameters["height"] = 16;
  graph.add_node(reloaded);
  proxy_graph.synchronize_with_graph(graph);

  const auto* reloaded_state = proxy_graph.find_state(1);
  ASSERT_NE(reloaded_state, nullptr);
  EXPECT_FALSE(reloaded_state->output.has_value());
  EXPECT_EQ(reloaded_state->version, 0);
  EXPECT_FALSE(reloaded_state->dirty_source_generation.has_value());
  EXPECT_EQ(proxy_graph.topology_generation(), graph.topology_generation());
}

TEST(HighPrecisionDirtyWriteBuffer, StagesGraphWritesUntilCommit) {
  GraphModel graph("cache/hp-dirty-write-buffer");
  Node node = make_node(1, "split_plan", "tile");
  node.cached_output_high_precision = make_image_output(4, 4, 1, 2.0f);
  node.hp_version = 3;
  node.hp_region =
      RegionSet::from_image_rect({image_region_domain(), 0, 1, 0, 1});
  graph.add_node(node);

  compute::HighPrecisionDirtyWriteBuffer buffer;
  NodeOutput& staged = buffer.ensure_output(graph.node(1));
  toCvMat(staged.image_buffer).setTo(6.0f);
  buffer.mark_updated(
      graph.node(1),
      RegionSet::from_image_rect({image_region_domain(), 1, 3, 1, 3}), true,
      77);

  ASSERT_TRUE(graph.node(1).cached_output_high_precision.has_value());
  EXPECT_FLOAT_EQ(
      toCvMat(graph.node(1).cached_output_high_precision->image_buffer)
          .at<float>(0, 0),
      2.0f);
  EXPECT_EQ(graph.node(1).hp_version, 3);
  EXPECT_EQ(graph.node(1).hp_region,
            RegionSet::from_image_rect({image_region_domain(), 0, 1, 0, 1}));
  EXPECT_FALSE(graph.dirty_source_hp_commit_generation.count(1));

  buffer.commit_to_graph(graph);

  ASSERT_TRUE(graph.node(1).cached_output_high_precision.has_value());
  EXPECT_FLOAT_EQ(
      toCvMat(graph.node(1).cached_output_high_precision->image_buffer)
          .at<float>(0, 0),
      6.0f);
  EXPECT_EQ(graph.node(1).hp_version, 4);
  EXPECT_EQ(graph.node(1).hp_region,
            RegionSet::from_image_rect({image_region_domain(), 1, 3, 1, 3}))
      << "a non-representable disjoint validity union must retain the fresh "
         "exact update, never publish an invalid rectangular superset";
  EXPECT_EQ(graph.dirty_source_hp_commit_generation[1], 77u);
}

TEST(GlobalHighPrecisionDirtyUpdate, UsesDirtyPlanningForGlobalHpDirtyRoi) {
  register_split_ops();
  GraphModel graph("cache/global-hp-dirty-update");
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 64;
  source.parameters["height"] = 64;
  graph.add_node(source);
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(downstream);
  graph.rebuild_topology_index();

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService compute(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  ComputeService::Request request;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.dirty_roi = (PixelRect{8, 8, 16, 16});
  NodeOutput& output = compute.compute(graph, request);

  EXPECT_EQ(output.image_buffer.width, 64);
  EXPECT_EQ(output.image_buffer.height, 64);
  ASSERT_TRUE(graph.last_dirty_region_snapshot.has_value());
  EXPECT_FALSE(graph.last_dirty_region_snapshot->actual_dirty_rois.empty());
  ASSERT_TRUE(graph.last_compute_plan.has_value());
  EXPECT_EQ(graph.last_compute_plan->intent,
            ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(graph.last_compute_plan->target_node_id, 2);
  EXPECT_FALSE(graph.last_compute_plan->task_graph.tasks.empty());
  EXPECT_TRUE(std::any_of(
      graph.last_compute_plan->task_graph.tasks.begin(),
      graph.last_compute_plan->task_graph.tasks.end(),
      [](const compute::PlannedTask& task) { return task.dirty_selected; }));
  ASSERT_TRUE(graph.last_compute_plan_summary.has_value());
  EXPECT_EQ(graph.last_compute_plan_summary->intent,
            ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(graph.last_compute_plan_summary->target_node_id, 2);
  EXPECT_GT(graph.last_compute_plan_summary->task_count, 0u);
  EXPECT_GT(graph.last_compute_plan_summary->tile_task_count, 0u);
  EXPECT_GT(graph.last_compute_plan_summary->active_task_count, 0u);
  EXPECT_GT(graph.last_compute_plan_summary->downstream_task_count, 0u);
  EXPECT_LE(graph.last_compute_plan_summary->active_task_count,
            graph.last_compute_plan_summary->task_count);

  auto recorded_events = events.drain(kComputeEventDrainMaxLimit);
  EXPECT_TRUE(std::any_of(
      recorded_events.events.begin(), recorded_events.events.end(),
      [](const ComputeEventSnapshot& event) {
        return event.source == "intent_coordinator_global_dirty_update";
      }));
  EXPECT_TRUE(std::any_of(recorded_events.events.begin(),
                          recorded_events.events.end(),
                          [](const ComputeEventSnapshot& event) {
                            return event.source == "hp_update";
                          }));
}

TEST(GlobalHighPrecisionDirtyUpdate, ForceRecacheRecomputesFullHpFrame) {
  register_split_ops();
  GraphModel graph("cache/global-hp-force-dirty-full-frame");
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 128;
  source.parameters["height"] = 128;
  graph.add_node(source);
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(downstream);
  graph.rebuild_topology_index();

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService compute(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  ComputeService::Request full_request;
  full_request.node_id = 2;
  full_request.cache.precision = "float32";
  full_request.cache.disable_disk_cache = true;
  NodeOutput& initial_output = compute.compute(graph, full_request);
  ASSERT_EQ(initial_output.image_buffer.width, 128);
  ASSERT_EQ(initial_output.image_buffer.height, 128);

  graph.mutate_node_runtime_state(2, [](GraphModel::NodeRuntimeState& state) {
    ASSERT_TRUE(state.cached_output_high_precision.has_value());
    toCvMat(state.cached_output_high_precision->image_buffer).setTo(9.0f);
  });

  ComputeService::Request dirty_request;
  dirty_request.node_id = 2;
  dirty_request.cache.precision = "float32";
  dirty_request.cache.force_recache = true;
  dirty_request.cache.disable_disk_cache = true;
  dirty_request.intent = ComputeIntent::GlobalHighPrecision;
  dirty_request.dirty_roi = (PixelRect{16, 16, 16, 16});
  NodeOutput& forced_output = compute.compute(graph, dirty_request);

  ASSERT_EQ(forced_output.image_buffer.width, 128);
  ASSERT_EQ(forced_output.image_buffer.height, 128);
  const cv::Mat forced_mat = toCvMat(forced_output.image_buffer);
  for (int y = 0; y < forced_mat.rows; ++y) {
    for (int x = 0; x < forced_mat.cols; ++x) {
      EXPECT_FLOAT_EQ(forced_mat.at<float>(y, x), 2.0f)
          << "forced HP dirty update must recompute full-frame pixel " << x
          << "," << y;
    }
  }

  ASSERT_TRUE(graph.last_dirty_region_snapshot.has_value());
  ASSERT_TRUE(graph.last_dirty_region_snapshot->actual_dirty_rois.count(2));
  EXPECT_EQ(graph.last_dirty_region_snapshot->actual_dirty_rois.at(2).front(),
            (PixelRect{0, 0, 128, 128}));
  ASSERT_TRUE(graph.last_compute_plan_summary.has_value());
  EXPECT_EQ(graph.last_compute_plan_summary->active_task_count,
            graph.last_compute_plan_summary->task_count);
}

TEST(RealTimeDirtyUpdate, ForceRecacheHpSiblingCommitsCompleteHpOutput) {
  register_split_ops();
  GraphModel graph("cache/rt-force-dirty-hp-full-frame");
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 128;
  source.parameters["height"] = 128;
  graph.add_node(source);
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(downstream);
  graph.rebuild_topology_index();

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service;
  ComputeService compute(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle graph_lifecycle(execution_service,
                                                         graph);

  ComputeService::Request full_request;
  full_request.node_id = 2;
  full_request.cache.precision = "float32";
  full_request.cache.disable_disk_cache = true;
  NodeOutput& initial_output = compute.compute(graph, full_request);
  ASSERT_EQ(initial_output.image_buffer.width, 128);
  ASSERT_EQ(initial_output.image_buffer.height, 128);

  graph.mutate_node_runtime_state(2, [](GraphModel::NodeRuntimeState& state) {
    ASSERT_TRUE(state.cached_output_high_precision.has_value());
    toCvMat(state.cached_output_high_precision->image_buffer).setTo(9.0f);
  });

  ComputeService::Request rt_request;
  rt_request.node_id = 2;
  rt_request.cache.precision = "float32";
  rt_request.cache.force_recache = true;
  rt_request.cache.disable_disk_cache = true;
  rt_request.intent = ComputeIntent::RealTimeUpdate;
  rt_request.dirty_roi = (PixelRect{16, 16, 16, 16});
  NodeOutput& rt_output = compute.compute(graph, rt_request);

  EXPECT_GT(rt_output.image_buffer.width, 0);
  EXPECT_GT(rt_output.image_buffer.height, 0);
  ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
  const cv::Mat hp_mat =
      toCvMat(graph.node(2).cached_output_high_precision->image_buffer);
  ASSERT_EQ(hp_mat.cols, 128);
  ASSERT_EQ(hp_mat.rows, 128);
  for (int y = 0; y < hp_mat.rows; ++y) {
    for (int x = 0; x < hp_mat.cols; ++x) {
      EXPECT_FLOAT_EQ(hp_mat.at<float>(y, x), 2.0f)
          << "RT HP sibling must commit full-frame HP pixel " << x << "," << y;
    }
  }
  ASSERT_TRUE(graph.last_dirty_region_snapshot.has_value());
  ASSERT_TRUE(graph.last_dirty_region_snapshot->actual_dirty_rois.count(2));
  EXPECT_EQ(graph.last_dirty_region_snapshot->actual_dirty_rois.at(2).front(),
            (PixelRect{0, 0, 128, 128}));
}

TEST(KernelComputeRuntimeSplit, SequentialAndParallelHpProduceIdenticalPixels) {
  register_split_ops();
  ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                           "photospider-split-hp-parity");
  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  Kernel::ExecutionConfig execution_config;
  execution_config.worker_count = 2;
  kernel.set_execution_config(execution_config);
  InteractionService interaction(kernel);
  constexpr char kGraphName[] = "split_hp_parity";
  ASSERT_TRUE(interaction.cmd_load_graph(kGraphName, root.path().string(), "")
                  .has_value());
  const auto service_route =
      testing::KernelTestAccess::runtime(kernel, kGraphName)
          .execution_route(ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(service_route.execution_type, "cpu");
  EXPECT_GT(service_route.generation, 0U);

  GraphModel& graph = testing::KernelTestAccess::model(kernel, kGraphName);
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  Node target = make_node(2, "split_plan", "tile");
  target.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(target);
  graph.validate_topology();

  Kernel::ComputeRequest request;
  request.name = kGraphName;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  request.intent = ComputeIntent::GlobalHighPrecision;
  auto sequential = interaction.cmd_compute_and_get_image(request);
  ASSERT_TRUE(sequential.has_value());
  const uint64_t sequential_hp_version = graph.node(2).hp_version;
  EXPECT_GT(sequential_hp_version, 0u);
  ASSERT_TRUE(graph.node(2).hp_region.has_value());
  EXPECT_EQ(*graph.node(2).hp_region,
            RegionSet::from_image_rect({image_region_domain(), 0, 32, 0, 16}));

  testing::KernelTestAccess::clear_execution_trace(kernel, kGraphName);
  request.execution.parallel = true;
  auto parallel = interaction.cmd_compute_and_get_image(request);
  ASSERT_TRUE(parallel.has_value());
  ASSERT_TRUE(graph.last_compute_plan_summary.has_value());
  const auto& parallel_summary = *graph.last_compute_plan_summary;
  EXPECT_EQ(parallel_summary.intent, ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(parallel_summary.target_node_id, 2);
  EXPECT_TRUE(parallel_summary.parallel);
  EXPECT_GT(parallel_summary.task_count, 0u);
  EXPECT_GT(parallel_summary.tile_task_count, 0u);
  EXPECT_LE(parallel_summary.tile_task_count, parallel_summary.task_count);
  const auto execution_events =
      testing::KernelTestAccess::execution_trace(kernel, kGraphName);
  EXPECT_TRUE(std::any_of(execution_events.events.begin(),
                          execution_events.events.end(), [](const auto& event) {
                            return event.action ==
                                   GraphRuntime::ExecutionEvent::EXECUTE_TILE;
                          }));
  EXPECT_GT(graph.node(2).hp_version, sequential_hp_version);
  ASSERT_TRUE(graph.node(2).hp_region.has_value());
  EXPECT_EQ(*graph.node(2).hp_region,
            RegionSet::from_image_rect({image_region_domain(), 0, 32, 0, 16}));
  const cv::Mat sequential_matrix = toCvMat(*sequential);
  const cv::Mat parallel_matrix = toCvMat(*parallel);
  ASSERT_EQ(sequential_matrix.size(), parallel_matrix.size());
  ASSERT_EQ(sequential_matrix.type(), parallel_matrix.type());
  EXPECT_DOUBLE_EQ(cv::sum(sequential_matrix)[0], cv::sum(parallel_matrix)[0]);
  EXPECT_DOUBLE_EQ(cv::norm(sequential_matrix, parallel_matrix, cv::NORM_INF),
                   0.0);
}

TEST(KernelComputeRuntimeSplit,
     AsyncHpThenParallelRtDirtyUsesSharedServiceAndExposesState) {
  register_split_ops();
  ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                           "photospider-split-async-inline-dirty");
  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  Kernel::ExecutionConfig execution_config;
  execution_config.worker_count = 2;
  kernel.set_execution_config(execution_config);
  InteractionService interaction(kernel);
  constexpr char kGraphName[] = "split_async_inline_dirty";
  ASSERT_TRUE(interaction.cmd_load_graph(kGraphName, root.path().string(), "")
                  .has_value());

  GraphModel& graph = testing::KernelTestAccess::model(kernel, kGraphName);
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 64;
  source.parameters["height"] = 64;
  Node target = make_node(2, "split_plan", "tile");
  target.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(target);
  graph.validate_topology();

  Kernel::ComputeRequest hp_request;
  hp_request.name = kGraphName;
  hp_request.node_id = 2;
  hp_request.cache.precision = "float32";
  hp_request.cache.force_recache = true;
  hp_request.cache.disable_disk_cache = true;
  hp_request.cache.nosave = true;
  hp_request.execution.parallel = true;
  hp_request.intent = ComputeIntent::GlobalHighPrecision;
  auto hp_future = interaction.cmd_compute_async(hp_request);
  ASSERT_TRUE(hp_future.has_value());
  const Kernel::AsyncComputeResult hp_outcome = hp_future->get();
  ASSERT_TRUE(hp_outcome.ok);
  EXPECT_FALSE(hp_outcome.error.has_value());
  auto hp_events = interaction.cmd_drain_compute_events(
      kGraphName, kComputeEventDrainMaxLimit);
  ASSERT_TRUE(hp_events.has_value());
  EXPECT_TRUE(contains_event_sources(
      hp_events->events,
      {"intent_coordinator_global_high_precision", "computed"}));
  ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());

  Kernel::ComputeRequest rt_request = hp_request;
  rt_request.cache.force_recache = false;
  rt_request.execution.parallel = true;
  rt_request.intent = ComputeIntent::RealTimeUpdate;
  rt_request.dirty_roi = (PixelRect{8, 8, 16, 16});
  auto rt_image = interaction.cmd_compute_and_get_image(rt_request);
  ASSERT_TRUE(rt_image.has_value());
  EXPECT_GT(rt_image->width, 0);
  EXPECT_GT(rt_image->height, 0);

  auto rt_events = interaction.cmd_drain_compute_events(
      kGraphName, kComputeEventDrainMaxLimit);
  ASSERT_TRUE(rt_events.has_value());
  EXPECT_TRUE(contains_event_sources(
      rt_events->events,
      {"intent_coordinator_decision_concurrent",
       "intent_coordinator_concurrent_rt_start",
       "intent_coordinator_concurrent_hp_start", "rt_update", "hp_update"}));
  std::vector<std::string> event_sources;
  event_sources.reserve(rt_events->events.size());
  std::transform(rt_events->events.begin(), rt_events->events.end(),
                 std::back_inserter(event_sources),
                 [](const auto& event) { return event.source; });
  const auto concurrent_rt_start =
      std::find(event_sources.begin(), event_sources.end(),
                "intent_coordinator_concurrent_rt_start");
  const auto concurrent_hp_start =
      std::find(event_sources.begin(), event_sources.end(),
                "intent_coordinator_concurrent_hp_start");
  ASSERT_NE(concurrent_rt_start, event_sources.end());
  ASSERT_NE(concurrent_hp_start, event_sources.end());
  EXPECT_LT(concurrent_rt_start, concurrent_hp_start);

  ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
  EXPECT_GT(graph.node(2).hp_version, 0);
  ASSERT_TRUE(graph.last_dirty_region_snapshot.has_value());
  EXPECT_TRUE(graph.last_dirty_region_snapshot->actual_dirty_rois.count(2));
  EXPECT_TRUE(std::any_of(
      graph.recent_compute_plan_summaries.begin(),
      graph.recent_compute_plan_summaries.end(), [](const auto& summary) {
        return summary.intent == ComputeIntent::GlobalHighPrecision;
      }));
  EXPECT_TRUE(std::any_of(
      graph.recent_compute_plan_summaries.begin(),
      graph.recent_compute_plan_summaries.end(), [](const auto& summary) {
        return summary.intent == ComputeIntent::RealTimeUpdate;
      }));
}

TEST(KernelComputeRuntimeSplit,
     ParallelOperationFailureMessageSurvivesInteractionLastError) {
  register_split_ops();
  ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                           "photospider-split-parallel-error");
  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  Kernel::ExecutionConfig execution_config;
  execution_config.worker_count = 2;
  kernel.set_execution_config(execution_config);
  InteractionService interaction(kernel);
  constexpr char kGraphName[] = "split_parallel_error";
  ASSERT_TRUE(interaction.cmd_load_graph(kGraphName, root.path().string(), "")
                  .has_value());

  GraphModel& graph = testing::KernelTestAccess::model(kernel, kGraphName);
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 16;
  source.parameters["height"] = 16;
  Node target = make_node(2, "split_plan", "parallel_failure");
  target.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(target);
  graph.validate_topology();

  Kernel::ComputeRequest request;
  request.name = kGraphName;
  request.node_id = 2;
  request.cache.precision = "float32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  request.execution.parallel = true;
  EXPECT_FALSE(interaction.cmd_compute(request));
  const auto error = interaction.cmd_last_error(kGraphName);
  ASSERT_TRUE(error.has_value());
  EXPECT_NE(error->message.find(kOpFailureMessage), std::string::npos);
  const auto execution_events =
      testing::KernelTestAccess::execution_trace(kernel, kGraphName);
  EXPECT_TRUE(std::any_of(
      execution_events.events.begin(), execution_events.events.end(),
      [](const auto& event) {
        return event.action == GraphRuntime::ExecutionEvent::RETHROW_EXCEPTION;
      }));
}

TEST(KernelComputeRuntimeSplit,
     MissingPropagatorsProjectDirtyRoiThroughIdentityFallback) {
  register_split_ops();
  ScopedTestDirectory root(std::filesystem::temp_directory_path() /
                           "photospider-split-identity-projection");
  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  InteractionService interaction(kernel);
  constexpr char kGraphName[] = "split_identity_projection";
  ASSERT_TRUE(interaction.cmd_load_graph(kGraphName, root.path().string(), "")
                  .has_value());

  GraphModel& graph = testing::KernelTestAccess::model(kernel, kGraphName);
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 40;
  source.parameters["height"] = 30;
  Node target = make_node(2, "split_plan", "tile");
  target.parameters["width"] = 40;
  target.parameters["height"] = 30;
  target.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(target);
  graph.validate_topology();

  const PixelRect dirty_roi{3, 4, 5, 6};
  const auto forward = interaction.cmd_project_roi(kGraphName, 1, dirty_roi, 2);
  const auto backward =
      interaction.cmd_project_roi_backward(kGraphName, 2, dirty_roi, 1);
  ASSERT_TRUE(forward.has_value());
  ASSERT_TRUE(backward.has_value());
  EXPECT_EQ(*forward, dirty_roi);
  EXPECT_EQ(*backward, dirty_roi);
  EXPECT_EQ(OpRegistry::instance().dirty_propagation_contract_status(
                "split_plan", "tile"),
            PropagationContractStatus::LegacyIdentityFallback);
  EXPECT_EQ(OpRegistry::instance().forward_propagation_contract_status(
                "split_plan", "tile"),
            PropagationContractStatus::LegacyIdentityFallback);
}

TEST(DirtySourceLifecycleFacade, UsesHostPublicBoundary) {
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto seed = host->seed_builtin_ops();
  ASSERT_TRUE(seed.status.ok) << seed.status.message;

  GraphLoadRequest load_request;
  load_request.session = GraphSessionId{"dirty_facade"};
  load_request.root_dir = "sessions";
  load_request.yaml_path = "tests/fixtures/graphs/dirty_region_test.yaml";
  const auto loaded = host->load_graph(load_request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  const auto begin = host->begin_dirty_source(loaded.value, NodeId{1},
                                              DirtyDomain::HighPrecision,
                                              (PixelRect{0, 0, 32, 32}));
  ASSERT_TRUE(begin.status.ok) << begin.status.message;
  EXPECT_EQ(begin.value.graph_generation, 1u);
  ASSERT_EQ(begin.value.sources.size(), 1u);
  EXPECT_EQ(begin.value.sources.front().node.value, 1);
  EXPECT_EQ(begin.value.sources.front().lifecycle,
            DirtySourceLifecycleState::Updating);

  const auto update = host->update_dirty_source(loaded.value, NodeId{1},
                                                DirtyDomain::HighPrecision,
                                                (PixelRect{16, 16, 16, 16}));
  ASSERT_TRUE(update.status.ok) << update.status.message;
  EXPECT_EQ(update.value.graph_generation, begin.value.graph_generation);
  ASSERT_EQ(update.value.sources.size(), 1u);
  EXPECT_EQ(update.value.sources.front().source_rois.size(), 2u);

  const auto snapshot = host->dirty_region_snapshot(loaded.value);
  ASSERT_TRUE(snapshot.status.ok) << snapshot.status.message;
  EXPECT_EQ(snapshot.value.graph_generation, update.value.graph_generation);
  EXPECT_TRUE(snapshot.value.actual_dirty_rois.count(1));

  const auto end = host->end_dirty_source(loaded.value, NodeId{1},
                                          DirtyDomain::HighPrecision);
  ASSERT_TRUE(end.status.ok) << end.status.message;
  ASSERT_EQ(end.value.sources.size(), 1u);
  EXPECT_EQ(end.value.sources.front().lifecycle,
            DirtySourceLifecycleState::Settled);
}

TEST(DirtyControlLaneFacade, ExposesWakeupAndCutoffThroughInteractionService) {
  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  InteractionService svc(kernel);
  svc.cmd_seed_builtin_ops();

  auto loaded =
      svc.cmd_load_graph("dirty_control_lane", "sessions",
                         "tests/fixtures/graphs/dirty_region_test.yaml");
  ASSERT_TRUE(loaded.has_value());

  auto begin = svc.cmd_begin_dirty_source_control(
      *loaded, 1, compute::DirtyDomain::HighPrecision,
      (PixelRect{0, 0, 32, 32}));
  ASSERT_TRUE(begin.has_value());
  EXPECT_EQ(begin->event, compute::DirtyControlEvent::Begin);
  EXPECT_EQ(begin->generation, 1u);
  EXPECT_EQ(begin->dirty_updating_count, 1u);
  EXPECT_TRUE(begin->should_wake_dispatcher);
  EXPECT_FALSE(begin->cutoff_after_downstream);

  auto update = svc.cmd_update_dirty_source_control(
      *loaded, 1, compute::DirtyDomain::HighPrecision,
      (PixelRect{16, 16, 16, 16}));
  ASSERT_TRUE(update.has_value());
  EXPECT_EQ(update->event, compute::DirtyControlEvent::Update);
  EXPECT_EQ(update->generation, begin->generation);
  EXPECT_EQ(update->dirty_updating_count, 1u);
  EXPECT_TRUE(update->should_wake_dispatcher);
  EXPECT_FALSE(update->cutoff_after_downstream);
  ASSERT_TRUE(update->snapshot.source_roi_records.count(1));
  EXPECT_EQ(update->snapshot.source_roi_records.at(1).size(), 2u);

  auto end = svc.cmd_end_dirty_source_control(
      *loaded, 1, compute::DirtyDomain::HighPrecision);
  ASSERT_TRUE(end.has_value());
  EXPECT_EQ(end->event, compute::DirtyControlEvent::End);
  EXPECT_EQ(end->generation, begin->generation);
  EXPECT_EQ(end->dirty_updating_count, 0u);
  EXPECT_TRUE(end->should_wake_dispatcher);
  EXPECT_TRUE(end->cutoff_after_downstream);
  ASSERT_TRUE(end->snapshot.dirty_source_state.count(1));
  EXPECT_EQ(end->snapshot.dirty_source_state.at(1).lifecycle,
            compute::DirtySourceLifecycleState::Settled);
}

}  // namespace ps
