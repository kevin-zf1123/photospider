#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "compute/compute_run.hpp"
#include "compute/compute_task_submission.hpp"
#include "compute/dirty_execution_common.hpp"
#include "compute/dirty_update_executor.hpp"
#include "compute/dirty_write_buffers.hpp"
#include "compute/execution_service.hpp"
#include "compute/resource_demand_estimator.hpp"
#include "core/image_buffer_processing.hpp"
#include "execution/execution_task_runtime.hpp"
#include "graph/graph_cache_service.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_traversal_service.hpp"
#include "photospider/core/graph_error.hpp"
#include "providers/configured_image_artifact_codec.hpp"
#include "runtime/graph_event_service.hpp"
#include "runtime/graph_runtime.hpp"
#include "support/cache_test_dependencies.hpp"
#include "support/execution_service_test_access.hpp"
#include "support/graph_model_test_access.hpp"
#include "support/graph_revision_test_access.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Builds deterministic descriptor input for ComputeRun unit tests.
 *
 * @param graph_identity Graph/session label to capture.
 * @param graph_revision Nonzero authoritative Graph revision and deterministic
 * strong Graph identity representation.
 * @param target_node_id Requested graph-local target.
 * @return Valid full-quality HP submission with explicit throughput QoS.
 * @throws std::bad_alloc if graph identity ownership cannot allocate.
 * @note The returned deadline is absent so test equality does not depend on
 * wall-clock timing.
 */
ComputeRunSubmission make_test_submission(std::string graph_identity,
                                          uint64_t graph_revision,
                                          int target_node_id) {
  return ComputeRunSubmission{
      std::move(graph_identity),
      GraphInstanceId{graph_revision},
      GraphRevision{graph_revision},
      target_node_id,
      ComputeIntent::GlobalHighPrecision,
      ComputeRunQuality::Full,
      ComputeRunQos{ComputeRunQosClass::Throughput, std::nullopt, 3, 2},
      SupersessionIdentity{
          SupersessionKey(target_node_id, ComputeIntent::GlobalHighPrecision),
          SupersessionGeneration(1)}};
}

/**
 * @brief Releases and joins identity-mint workers before their storage expires.
 * @throws Nothing from destruction.
 * @note The worker vector reserves capacity before this guard is created. If
 * any later launch or assertion throws, destruction opens the shared start gate
 * and consumes every valid ready future before the vector is destroyed.
 */
class ScopedIdentityMintWorkerRecovery {
 public:
  /** @brief Future result produced by one terminal identity-mint worker. */
  using WorkerResult = std::future<std::optional<GraphInstanceId>>;

  /**
   * @brief Borrows one start promise and its pre-reserved worker vector.
   * @param start Promise awaited by every admitted worker.
   * @param workers Worker handles that outlive this guard.
   * @throws Nothing.
   */
  ScopedIdentityMintWorkerRecovery(std::promise<void>& start,
                                   std::vector<WorkerResult>& workers) noexcept
      : start_(start), workers_(workers) {}

  /**
   * @brief Opens the start gate and best-effort consumes unclaimed workers.
   * @throws Nothing; promise, worker, and future exceptions are contained.
   * @note Each wait is bounded. Gate release happens before any wait, so no
   * worker can remain blocked on test-owned synchronization.
   */
  ~ScopedIdentityMintWorkerRecovery() noexcept {
    release();
    for (auto& worker : workers_) {
      if (!worker.valid()) {
        continue;
      }
      try {
        if (worker.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready) {
          (void)worker.get();
        }
      } catch (...) {
      }
    }
  }

  /**
   * @brief Opens the shared start gate exactly once.
   * @return Nothing.
   * @throws Nothing; an unexpected promise failure is contained for cleanup.
   */
  void release() noexcept {
    if (released_) {
      return;
    }
    released_ = true;
    try {
      start_.set_value();
    } catch (...) {
    }
  }

  /**
   * @brief Disables copying of single-gate recovery ownership.
   * @param other Guard whose cleanup ownership remains unique.
   * @throws Nothing because construction is unavailable.
   */
  ScopedIdentityMintWorkerRecovery(
      const ScopedIdentityMintWorkerRecovery& other) = delete;

  /**
   * @brief Disables assignment of single-gate recovery ownership.
   * @param other Guard whose ownership cannot replace this instance.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedIdentityMintWorkerRecovery& operator=(
      const ScopedIdentityMintWorkerRecovery& other) = delete;

 private:
  /** @brief Borrowed one-shot start promise. */
  std::promise<void>& start_;

  /** @brief Borrowed pre-reserved future storage. */
  std::vector<WorkerResult>& workers_;

  /** @brief True after gate publication was attempted. */
  bool released_ = false;
};

/**
 * @brief Builds deterministic descriptor input for built-in policy tests.
 *
 * @param graph_identity Stable Graph/session fairness identity.
 * @param graph_revision Nonzero authoritative Graph revision and deterministic
 * strong Graph identity representation.
 * @param target_node_id Diagnostic graph-local target.
 * @param service_class Explicit built-in policy selector.
 * @param weight Positive immutable weighted-fairness input.
 * @param deadline Optional monotonic deadline used only by interactive policy.
 * @return Valid full-quality HP submission with the requested explicit QoS.
 * @throws std::bad_alloc if Graph identity ownership cannot allocate.
 * @note Intent and quality deliberately remain unchanged so these tests prove
 * policy selection does not infer QoS from either field.
 */
ComputeRunSubmission make_policy_submission(
    std::string graph_identity, uint64_t graph_revision, int target_node_id,
    ComputeRunQosClass service_class, std::uint32_t weight = 1U,
    std::optional<std::chrono::steady_clock::time_point> deadline =
        std::nullopt) {
  ComputeRunSubmission submission = make_test_submission(
      std::move(graph_identity), graph_revision, target_node_id);
  submission.qos = ComputeRunQos{service_class, deadline, weight, std::nullopt};
  return submission;
}

/**
 * @brief Builds a minimal valid graph node for Run-owned plan construction.
 *
 * @param id Stable graph-local node id.
 * @return Node with deterministic name and intentionally unresolved operation.
 * @throws std::bad_alloc if string assignment cannot allocate.
 * @note TaskSubmissionPlan preserves a missing operation for worker-time error,
 * so plan/storage construction does not require registry mutation.
 */
Node make_plan_node(int id) {
  Node node;
  node.id = id;
  node.name = "compute_run_node_" + std::to_string(id);
  node.type = "compute_run_test";
  node.subtype = "source";
  return node;
}

/**
 * @brief Counts actual product-operation entry in resource-admission tests.
 *
 * @note The operation registry is process-global, so the callback references
 * only this process-lifetime counter and never test-local storage.
 */
std::atomic_int g_resource_product_operation_calls{0};

/**
 * @brief Counts connected-parameter preflight operation entry.
 *
 * @note Tests reset the counter only after all preceding service work has
 * synchronously drained.
 */
std::atomic_int g_resource_preflight_operation_calls{0};

/**
 * @brief Counts real HP dirty product-operation entry.
 *
 * @note The owning test resets the process-lifetime counter after all prior
 * service work has synchronously drained.
 */
std::atomic_int g_resource_dirty_hp_operation_calls{0};

/**
 * @brief Counts real RT dirty product-operation entry.
 *
 * @note The RT executor selects the dedicated HP-monolithic fallback key, so
 * this counter still observes the real RT staging and service adapter path.
 */
std::atomic_int g_resource_dirty_rt_operation_calls{0};

/** @brief Counts Issue #75 CPU implementation entries. */
std::atomic_int g_issue75_cpu_operation_calls{0};

/** @brief Counts Issue #75 fake-Metal implementation entries. */
std::atomic_int g_issue75_gpu_operation_calls{0};

/** @brief Records the worker id that entered the latest fake-Metal callback. */
std::atomic_int g_issue75_gpu_worker_id{-1};

/** @brief Serializes process-global legacy cancellation callback configuration.
 */
std::mutex g_legacy_cancellation_source_mutex;

/**
 * @brief Optional weak Run authority copied by the legacy source operation.
 * @note The scoped test owner resets this value after synchronous drainage.
 */
std::optional<ComputeRunCancellationSource> g_legacy_cancellation_source;

/** @brief Counts legacy chain source operation entry. */
std::atomic_int g_legacy_cancellation_source_calls{0};

/** @brief Counts legacy chain dependent operation entry. */
std::atomic_int g_legacy_cancellation_dependent_calls{0};

/** @brief Stable provider failure emitted after legacy cancellation wins. */
constexpr auto kLegacyPostCancellationFailure =
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    "legacy source failure after cancellation";

/**
 * @brief Installs and later clears one source used by a process-global test op.
 *
 * @throws std::system_error when configuration mutex locking fails.
 * @note The stored source is weak with respect to Run lifetime. This guard must
 * outlive deterministic CompletionTrackingRuntime drainage.
 */
class ScopedLegacyCancellationSource final {
 public:
  /**
   * @brief Publishes the source copied by the registered source operation.
   * @param source Weak-lifetime matching Run authority.
   * @throws std::system_error when configuration mutex locking fails.
   */
  explicit ScopedLegacyCancellationSource(ComputeRunCancellationSource source) {
    std::lock_guard<std::mutex> lock(g_legacy_cancellation_source_mutex);
    g_legacy_cancellation_source = std::move(source);
  }

  /**
   * @brief Clears process-global callback configuration.
   * @throws Nothing; synchronization failure terminates the test process.
   */
  ~ScopedLegacyCancellationSource() noexcept {
    try {
      std::lock_guard<std::mutex> lock(g_legacy_cancellation_source_mutex);
      g_legacy_cancellation_source.reset();
    } catch (...) {
      std::terminate();
    }
  }

  /**
   * @brief Prevents copying process-global test configuration ownership.
   * @param other Guard that cannot be copied.
   * @throws Nothing because the operation is deleted.
   */
  ScopedLegacyCancellationSource(const ScopedLegacyCancellationSource&) =
      delete;

  /**
   * @brief Prevents copy assignment of process-global test configuration.
   * @param other Guard that cannot be assigned.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ScopedLegacyCancellationSource& operator=(
      const ScopedLegacyCancellationSource&) = delete;
};

/**
 * @brief Executes ordinary source A without requesting cancellation.
 * @return Staged source output whose completed call arms the injected deadline.
 * @throws std::bad_alloc when output storage cannot allocate.
 * @note The source-call counter changes before return. The matching test clock
 * keeps the deadline open for observations inside NodeTaskRunner and expires it
 * only on the next observation after run_task() returns.
 */
NodeOutput execute_legacy_cancellation_source() {
  NodeOutput output;
  output.data["value"] = 1;
  g_legacy_cancellation_source_calls.store(1, std::memory_order_release);
  return output;
}

/**
 * @brief Requests cancellation and then throws source A's injected failure.
 *
 * @return Never returns.
 * @throws std::logic_error when no scoped Run source is installed.
 * @throws std::runtime_error with kLegacyPostCancellationFailure for the
 * injected provider exception.
 * @throws std::bad_alloc or std::system_error from source copying, locking, or
 * cancellation cleanup unchanged.
 * @note Explicit cancellation wins before exception transport exercises losing
 * failure publication and callback-token retirement.
 */
[[noreturn]] NodeOutput execute_legacy_post_cancellation_failure() {
  g_legacy_cancellation_source_calls.fetch_add(1, std::memory_order_release);
  std::optional<ComputeRunCancellationSource> source;
  {
    std::lock_guard<std::mutex> lock(g_legacy_cancellation_source_mutex);
    source = g_legacy_cancellation_source;
  }
  if (!source.has_value()) {
    throw std::logic_error("Legacy cancellation source is not configured.");
  }
  (void)source->request_cancellation(
      ComputeRunCancellationReason::ExplicitRequest);
  throw std::runtime_error(kLegacyPostCancellationFailure);
}

/**
 * @brief Registers the deterministic legacy A-to-B cancellation operations.
 *
 * @return Nothing.
 * @throws Registry or callback allocation exceptions unchanged.
 * @note Registration is process-persistent and idempotent. The ordinary source
 * returns staged output for post-provider deadline observation; the failure
 * source requests explicit cancellation before throwing.
 */
void ensure_legacy_cancellation_operations_registered() {
  static std::once_flag once;
  std::call_once(once, [] {
    OpRegistry::instance().register_op_hp_monolithic(
        "compute_run_cancel_chain", "source",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              return execute_legacy_cancellation_source();
            }));
    OpRegistry::instance().register_op_hp_monolithic(
        "compute_run_cancel_chain", "source_throw_after_cancel",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              return execute_legacy_post_cancellation_failure();
            }));
    OpRegistry::instance().register_op_hp_monolithic(
        "compute_run_cancel_chain", "dependent",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              g_legacy_cancellation_dependent_calls.fetch_add(
                  1, std::memory_order_relaxed);
              NodeOutput output;
              output.data["value"] = 2;
              return output;
            }));
  });
}

/**
 * @brief Builds one small CPU image output for dirty product tests.
 * @param value Named scalar retained with the deterministic image.
 * @return Owned 8-by-8 FLOAT32 output.
 * @throws GraphError or std::bad_alloc when CPU storage cannot allocate.
 * @note Pixel capacity is intentionally outside issue #70 retained admission;
 * the named value keeps successful output validation deterministic.
 */
NodeOutput make_resource_dirty_output(int value) {
  NodeOutput output;
  output.image_buffer =
      make_aligned_cpu_image_buffer(8, 8, 1, DataType::FLOAT32);
  output.data["value"] = value;
  return output;
}

/**
 * @brief Registers deterministic CPU and fake-Metal operation candidates.
 *
 * @return Nothing.
 * @throws Registry allocation or callback-copy exceptions unchanged.
 * @note Registration is process-persistent and idempotent. Both candidates
 * return the same shaped output while distinct values/counters expose the
 * selected implementation and the GPU callback records its private worker.
 */
void ensure_issue75_device_operations_registered() {
  static std::once_flag once;
  std::call_once(once, [] {
    OpMetadata metadata;
    metadata.cost_score = 1;
    OpRegistry::instance().register_impl(
        "issue75_device_route", "source", Device::CPU,
        MonolithicOpFunc([](const Node&,
                            const std::vector<const NodeOutput*>&) {
          g_issue75_cpu_operation_calls.fetch_add(1, std::memory_order_relaxed);
          return make_resource_dirty_output(75);
        }),
        metadata);
    OpRegistry::instance().register_impl(
        "issue75_device_route", "source", Device::GPU_METAL,
        MonolithicOpFunc([](const Node&,
                            const std::vector<const NodeOutput*>&) {
          g_issue75_gpu_operation_calls.fetch_add(1, std::memory_order_relaxed);
          g_issue75_gpu_worker_id.store(GraphRuntime::this_worker_id(),
                                        std::memory_order_relaxed);
          return make_resource_dirty_output(76);
        }),
        metadata);
  });
}

/**
 * @brief Registers deterministic operations used by product-adapter tests.
 *
 * @return Nothing.
 * @throws Registry allocation or callback-copy exceptions unchanged.
 * @note Registration is process-persistent and idempotent. Both callbacks
 * return named data so successful execution cannot be confused with an empty
 * output.
 */
void ensure_resource_product_operations_registered() {
  static std::once_flag once;
  std::call_once(once, [] {
    OpRegistry::instance().register_op_hp_monolithic(
        "compute_run_resource", "full_source",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              g_resource_product_operation_calls.fetch_add(
                  1, std::memory_order_relaxed);
              NodeOutput output;
              output.data["value"] = 7;
              return output;
            }));
    OpRegistry::instance().register_op_hp_monolithic(
        "compute_run_resource", "parameter_source",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              g_resource_preflight_operation_calls.fetch_add(
                  1, std::memory_order_relaxed);
              NodeOutput output;
              output.data["value"] = 9;
              return output;
            }));
    OpRegistry::instance().register_op_hp_monolithic(
        "compute_run_resource", "dirty_hp",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              g_resource_dirty_hp_operation_calls.fetch_add(
                  1, std::memory_order_relaxed);
              return make_resource_dirty_output(11);
            }));
    OpRegistry::instance().register_op_hp_monolithic(
        "compute_run_resource", "dirty_rt",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              g_resource_dirty_rt_operation_calls.fetch_add(
                  1, std::memory_order_relaxed);
              return make_resource_dirty_output(12);
            }));
  });
}

/**
 * @brief Builds one full-HP node with a registered deterministic operation.
 *
 * @param id Stable graph-local node id.
 * @return Source node resolved by the resource product operation.
 * @throws std::bad_alloc when node strings cannot allocate.
 */
Node make_resource_product_node(int id) {
  Node node;
  node.id = id;
  node.name = "resource_product_node_" + std::to_string(id);
  node.type = "compute_run_resource";
  node.subtype = "full_source";
  return node;
}

/**
 * @brief Builds one real dirty-product node with an existing HP extent.
 *
 * @param node_id Graph-local node identity.
 * @param subtype Registered HP or RT-fallback operation subtype.
 * @return Node with an 8-by-8 reusable HP output and full-frame ROI.
 * @throws GraphError or std::bad_alloc when node/output ownership allocates.
 * @note The reusable output supplies deterministic dirty planning geometry;
 * successful execution replaces it through the production staging buffer.
 */
Node make_resource_dirty_product_node(int node_id, const std::string& subtype) {
  Node node = make_resource_product_node(node_id);
  node.subtype = subtype;
  node.cached_output_high_precision = make_resource_dirty_output(3);
  node.hp_roi = PixelRect{0, 0, 8, 8};
  node.hp_version = 1;
  return node;
}

/**
 * @brief Removes one test-owned GraphRuntime directory at scope exit.
 *
 * @throws Nothing from cleanup.
 * @note Removal uses error-code overloads so temporary cleanup never masks a
 * resource-admission assertion.
 */
class ScopedResourceRuntimeDirectory final {
 public:
  /**
   * @brief Prepares one deterministic temporary directory.
   * @param label Stable test-case suffix.
   * @throws std::bad_alloc when path ownership cannot allocate.
   * @throws std::filesystem::filesystem_error when the system temporary root
   * cannot be resolved.
   */
  explicit ScopedResourceRuntimeDirectory(const std::string& label)
      : path_(std::filesystem::temp_directory_path() /
              ("photospider-issue70-" + label)) {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  /**
   * @brief Removes runtime directories created by the owning test.
   * @throws Nothing.
   */
  ~ScopedResourceRuntimeDirectory() noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  /**
   * @brief Returns the runtime root.
   * @return Immutable path borrowed for this guard's lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  /** @brief Test-owned root removed at destruction. */
  std::filesystem::path path_;
};

/**
 * @brief GraphRuntime test double with deterministic fake Metal capability.
 *
 * @throws Standard GraphRuntime construction exceptions unchanged.
 * @note The runtime still owns real graph, trace, route, and proxy state. Only
 * capability discovery is overridden; no native Metal object is fabricated.
 */
class FakeMetalGraphRuntime final : public GraphRuntime {
 public:
  /**
   * @brief Constructs a runtime with controlled fake Metal availability.
   * @param info Ordinary graph-runtime ownership configuration.
   * @param metal_available Whether `GPU_METAL` is reported available.
   * @throws Standard GraphRuntime construction exceptions unchanged.
   */
  FakeMetalGraphRuntime(const Info& info, bool metal_available)
      : GraphRuntime(info), metal_available_(metal_available) {}

  /** @copydoc GraphRuntime::is_device_available */
  bool is_device_available(Device device) const noexcept override {
    return device == Device::CPU ||
           (metal_available_ && device == Device::GPU_METAL);
  }

 private:
  /** @brief Controlled fake Metal capability. */
  bool metal_available_ = false;
};

/**
 * @brief Deterministic owned-callback runtime for completion-isolation tests.
 *
 * The fixture accepts only the empty full-HP epoch marker, stores owned
 * callbacks until wait_for_completion(), and validates that every explicitly
 * added completion unit is released before the wait returns. All methods run
 * on the calling test thread, so a leaked unit fails deterministically instead
 * of hanging on a condition variable.
 *
 * @throws std::logic_error for borrowed work, missing batch initialization,
 * completion overflow/underflow, or a nonzero count after callback drainage.
 * @throws The exact first callback exception from wait_for_completion().
 * @note This fixture models execution completion ownership only; it owns no
 * worker, epoch filter, graph state, or operation implementation.
 */
class CompletionTrackingRuntime final : public ExecutionTaskRuntime {
 public:
  /**
   * @brief Starts a fresh completion batch from the required empty handle set.
   *
   * @param handles Borrowed handles, which must be empty.
   * @param total_task_count Initial count, which must be zero.
   * @param priority Ignored because the fixture has one deterministic lane.
   * @return Nothing.
   * @throws std::logic_error for nonempty handles or a nonzero initial count.
   * @note Starting a batch clears queued callbacks and all per-batch state.
   */
  void submit_initial_task_handles(
      std::vector<ExecutionTaskHandle>&& handles, int total_task_count,
      ExecutionTaskPriority priority = ExecutionTaskPriority::Normal) override {
    (void)priority;
    if (!handles.empty() || total_task_count != 0) {
      throw std::logic_error(
          "CompletionTrackingRuntime requires an empty zero-count batch.");
    }
    callbacks_.clear();
    next_callback_ = 0;
    first_exception_ = nullptr;
    tasks_to_complete_ = 0;
    batch_initialized_ = true;
  }

  /**
   * @brief Rejects borrowed worker-ready work from the full-HP test path.
   *
   * @param handles Borrowed handles, which must be empty.
   * @param priority Ignored because no borrowed work is accepted.
   * @return Nothing.
   * @throws std::logic_error when any borrowed handle is supplied.
   * @note Empty input is accepted as a no-op for interface completeness.
   */
  void submit_ready_task_handles_from_worker(
      std::vector<ExecutionTaskHandle>&& handles,
      ExecutionTaskPriority priority = ExecutionTaskPriority::Normal) override {
    (void)priority;
    if (!handles.empty()) {
      throw std::logic_error(
          "CompletionTrackingRuntime rejects borrowed ready handles.");
    }
  }

  /**
   * @brief Retains one owned callback until deterministic batch drainage.
   *
   * @param task Callback whose ownership transfers to this fixture.
   * @param priority Ignored deterministic priority.
   * @param epoch Ignored because the fixture has one active batch.
   * @return Nothing.
   * @throws std::logic_error when no batch has been initialized.
   * @throws std::bad_alloc when callback storage cannot grow.
   * @note Empty callbacks are ignored and do not change observation counters.
   */
  void submit_ready_task_any_thread(
      Task&& task,
      ExecutionTaskPriority priority = ExecutionTaskPriority::Normal,
      std::optional<std::uint64_t> epoch = std::nullopt) override {
    (void)priority;
    (void)epoch;
    if (!task) {
      return;
    }
    if (!batch_initialized_) {
      throw std::logic_error(
          "CompletionTrackingRuntime has no initialized batch.");
    }
    callbacks_.push_back(std::move(task));
    ++callbacks_submitted_;
  }

  /**
   * @brief Drains accepted callbacks and validates exact completion settlement.
   *
   * @return Nothing when callbacks finish and the count reaches zero.
   * @throws The exact first callback exception.
   * @throws std::logic_error when completion units remain after drainage.
   * @note Callbacks may append dependent callbacks while this loop is active.
   */
  void wait_for_completion() override {
    while (!first_exception_ && next_callback_ < callbacks_.size()) {
      Task callback = std::move(callbacks_.at(next_callback_));
      ++next_callback_;
      ++callbacks_invoked_;
      try {
        callback();
      } catch (...) {
        set_exception(std::current_exception());
      }
    }
    callbacks_.clear();
    next_callback_ = 0;
    if (first_exception_) {
      std::rethrow_exception(first_exception_);
    }
    if (tasks_to_complete_ != 0) {
      throw std::logic_error(
          "CompletionTrackingRuntime retained an unsettled completion unit.");
    }
  }

  /**
   * @brief Retains the first callback exception for wait-time rethrow.
   *
   * @param error Exact callback exception; null input is ignored.
   * @return Nothing.
   * @throws Nothing.
   * @note Later exceptions do not replace the first retained identity.
   */
  void set_exception(std::exception_ptr error) noexcept override {
    if (error && !first_exception_) {
      first_exception_ = std::move(error);
    }
  }

  /**
   * @brief Adds positive units to the current completion count.
   *
   * @param delta Positive unit count.
   * @return Nothing.
   * @throws std::logic_error for a missing batch or integer overflow.
   * @note Nonpositive input is a no-op, matching current execution behavior.
   */
  void inc_tasks_to_complete(int delta) override {
    if (delta <= 0) {
      return;
    }
    if (!batch_initialized_) {
      throw std::logic_error(
          "CompletionTrackingRuntime has no initialized batch.");
    }
    if (tasks_to_complete_ > std::numeric_limits<int>::max() - delta) {
      throw std::logic_error(
          "CompletionTrackingRuntime completion count overflow.");
    }
    tasks_to_complete_ += delta;
    total_units_added_ += delta;
  }

  /**
   * @brief Releases exactly one unit from the current completion count.
   *
   * @return Nothing.
   * @throws std::logic_error when no completion unit remains.
   * @note The release counter increments only after a valid decrement.
   */
  void dec_tasks_to_complete() override {
    if (tasks_to_complete_ <= 0) {
      throw std::logic_error(
          "CompletionTrackingRuntime completion count underflow.");
    }
    --tasks_to_complete_;
    ++completion_releases_;
  }

  /**
   * @brief Records whether planned operation execution was entered.
   *
   * @param action Stable execution trace action.
   * @param node_id Backend node id, unused by this fixture.
   * @return Nothing.
   * @throws Nothing.
   * @note Assignment traces are intentionally excluded from the execution
   * count.
   */
  void log_event(ExecutionTraceAction action, int node_id) noexcept override {
    (void)node_id;
    if (action == ExecutionTraceAction::Execute ||
        action == ExecutionTraceAction::ExecuteTile) {
      ++planned_execution_events_;
    }
  }

  /**
   * @brief Returns the current logical completion count.
   *
   * @return Outstanding units in the active batch.
   * @throws Nothing.
   */
  int tasks_to_complete() const noexcept { return tasks_to_complete_; }

  /**
   * @brief Returns all units added across the fixture lifetime.
   *
   * @return Sum of positive completion increments.
   * @throws Nothing.
   */
  int total_units_added() const noexcept { return total_units_added_; }

  /**
   * @brief Returns successful completion decrement calls.
   *
   * @return Number of exactly-one unit releases.
   * @throws Nothing.
   */
  int completion_releases() const noexcept { return completion_releases_; }

  /**
   * @brief Returns owned callbacks accepted by the fixture.
   *
   * @return Number of nonempty callback submissions.
   * @throws Nothing.
   */
  int callbacks_submitted() const noexcept { return callbacks_submitted_; }

  /**
   * @brief Returns owned callbacks invoked by batch drainage.
   *
   * @return Number of callbacks whose callable body was entered.
   * @throws Nothing.
   */
  int callbacks_invoked() const noexcept { return callbacks_invoked_; }

  /**
   * @brief Returns planned execution trace entries.
   *
   * @return Number of Execute or ExecuteTile actions.
   * @throws Nothing.
   * @note Zero proves the accepted callback did not enter NodeTaskRunner work.
   */
  int planned_execution_events() const noexcept {
    return planned_execution_events_;
  }

 private:
  /** @brief Owned callbacks retained until wait_for_completion(). */
  std::vector<Task> callbacks_;

  /** @brief Index of the next callback selected for deterministic drainage. */
  std::size_t next_callback_ = 0;

  /** @brief Exact first callback failure retained for wait-time rethrow. */
  std::exception_ptr first_exception_;

  /** @brief Outstanding logical completion units in the active batch. */
  int tasks_to_complete_ = 0;

  /** @brief Sum of positive units added across completed test batches. */
  int total_units_added_ = 0;

  /** @brief Number of successful exactly-one completion releases. */
  int completion_releases_ = 0;

  /** @brief Number of accepted nonempty owned callbacks. */
  int callbacks_submitted_ = 0;

  /** @brief Number of accepted callback bodies entered during drainage. */
  int callbacks_invoked_ = 0;

  /** @brief Number of planned Execute or ExecuteTile trace actions. */
  int planned_execution_events_ = 0;

  /** @brief Whether the required empty epoch marker initialized a batch. */
  bool batch_initialized_ = false;
};

/**
 * @brief Minimal CPU-only observation target for ExecutionService unit tests.
 *
 * @throws std::bad_alloc when a copied trace snapshot cannot allocate.
 * @throws std::system_error when trace snapshot locking fails.
 * @note The fixture owns no Graph or cache state. Worker-facing callbacks
 * remain noexcept; trace recording failures are retained as an explicit test
 * signal.
 */
class ExecutionServiceHost final : public ExecutionHostContext {
 public:
  /**
   * @brief Creates a trace Host with optional fake Metal capability.
   * @param metal_available Whether `GPU_METAL` is reported available.
   * @throws Nothing.
   */
  explicit ExecutionServiceHost(bool metal_available = false) noexcept
      : metal_available_(metal_available) {}

  /**
   * @brief Complete immutable execution trace tuple observed by one Host.
   *
   * @throws Nothing for scalar value construction and movement.
   * @note Epoch is the opaque ComputeRunId value forwarded by ExecutionService.
   */
  struct TraceEvent final {
    /** @brief Execution action forwarded by the active service worker. */
    ExecutionTraceAction action = ExecutionTraceAction::AssignInitial;

    /** @brief Graph-local diagnostic node id supplied by the ready task. */
    int node_id = -1;

    /** @brief Fixed-pool worker id that emitted this event. */
    int worker_id = -1;

    /** @brief Opaque Run epoch active on that worker. */
    std::uint64_t epoch = 0;
  };

  /**
   * @brief Reports the fixture's only physical capability.
   * @param device Capability requested by the execution service.
   * @return True for CPU and for the configured fake Metal capability.
   * @throws Nothing.
   */
  bool is_device_available(Device device) const noexcept override {
    return device == Device::CPU ||
           (metal_available_ && device == Device::GPU_METAL);
  }

  /**
   * @brief Records one worker-context entry.
   * @param worker_id Fixed private CPU or GPU worker id.
   * @param epoch Active nonzero execution epoch.
   * @return Nothing.
   * @throws Nothing.
   */
  void set_task_context(int worker_id, std::uint64_t epoch) noexcept override {
    (void)epoch;
    last_worker_id_.store(worker_id, std::memory_order_relaxed);
    context_entries_.fetch_add(1, std::memory_order_relaxed);
  }

  /**
   * @brief Records one balanced worker-context exit.
   * @return Nothing.
   * @throws Nothing.
   */
  void clear_task_context() noexcept override {
    context_exits_.fetch_add(1, std::memory_order_relaxed);
  }

  /**
   * @brief Records one forwarded execution trace.
   * @param action Trace action copied from the service CPU runtime.
   * @param node_id Planned node id.
   * @param worker_id Active CPU worker id.
   * @param epoch Active execution epoch.
   * @return Nothing.
   * @throws Nothing.
   */
  void log_event(ExecutionTraceAction action, int node_id, int worker_id,
                 std::uint64_t epoch) noexcept override {
    try {
      std::lock_guard<std::mutex> lock(trace_mutex_);
      trace_events_.push_back(TraceEvent{action, node_id, worker_id, epoch});
    } catch (...) {
      trace_recording_failed_.store(true, std::memory_order_relaxed);
    }
    if (action == ExecutionTraceAction::AssignInitial) {
      const ComputeRunCancellationSource* source =
          cancel_on_assignment_.exchange(nullptr, std::memory_order_acq_rel);
      if (source != nullptr) {
        try {
          (void)source->request_cancellation(
              ComputeRunCancellationReason::ExplicitRequest);
        } catch (...) {
          trace_recording_failed_.store(true, std::memory_order_relaxed);
        }
      }
    }
  }

  /**
   * @brief Arms deterministic cancellation at the next initial assignment
   * trace.
   * @param source Borrowed source that outlives the armed callback and Run
   * wait.
   * @return Nothing.
   * @throws Nothing.
   * @note The worker has already dequeued and counted the task in flight when
   * this hook runs, but ReadyTaskSubmission has not entered its executable.
   */
  void cancel_on_next_initial_assignment(
      const ComputeRunCancellationSource& source) noexcept {
    cancel_on_assignment_.store(&source, std::memory_order_release);
  }

  /**
   * @brief Returns worker-context entries observed by the proxy.
   * @return Total top-level CPU callback entries.
   * @throws Nothing.
   */
  int context_entries() const noexcept {
    return context_entries_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Returns balanced worker-context exits.
   * @return Total top-level CPU callback exits.
   * @throws Nothing.
   */
  int context_exits() const noexcept {
    return context_exits_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Returns the latest fixed worker id observed at callback entry.
   * @return Worker id, or -1 before any callback enters.
   * @throws Nothing.
   */
  int last_worker_id() const noexcept {
    return last_worker_id_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Returns a thread-safe snapshot of every forwarded trace tuple.
   * @return Complete trace events in Host observation order.
   * @throws std::bad_alloc when snapshot storage cannot allocate.
   * @throws std::system_error when mutex locking fails.
   */
  std::vector<TraceEvent> trace_events() const {
    std::lock_guard<std::mutex> lock(trace_mutex_);
    return trace_events_;
  }

  /**
   * @brief Reports whether a noexcept callback failed to retain a trace.
   * @return True after any trace allocation or mutex failure.
   * @throws Nothing.
   */
  bool trace_recording_failed() const noexcept {
    return trace_recording_failed_.load(std::memory_order_relaxed);
  }

 private:
  /** @brief Controlled fake Metal capability. */
  bool metal_available_ = false;

  /** @brief Top-level CPU callback context entries. */
  std::atomic_int context_entries_{0};

  /** @brief Balanced top-level CPU callback context exits. */
  std::atomic_int context_exits_{0};

  /** @brief Latest fixed private worker id. */
  std::atomic_int last_worker_id_{-1};

  /** @brief Serializes concurrent trace publication and snapshot copying. */
  mutable std::mutex trace_mutex_;

  /** @brief Complete execution traces forwarded to this Host proxy. */
  std::vector<TraceEvent> trace_events_;

  /** @brief Whether noexcept trace observation lost any event. */
  std::atomic_bool trace_recording_failed_{false};

  /** @brief Optional one-shot source used by the deterministic dequeue race. */
  std::atomic<const ComputeRunCancellationSource*> cancel_on_assignment_{
      nullptr};
};

/**
 * @brief Releases one promise-backed test gate exactly once during cleanup.
 *
 * The guard is declared after every async future it protects so stack
 * unwinding releases blocked callbacks before `std::future` destruction can
 * wait for them. Tests may call release() early; destruction then becomes a
 * no-op.
 *
 * @throws Nothing from release or destruction.
 * @note A failed explicit release is observable through the boolean result.
 * Destruction suppresses the same exception because cleanup must not throw.
 */
class ScopedPromiseRelease final {
 public:
  /**
   * @brief Borrows the promise whose shared future guards callback progress.
   *
   * @param promise Promise that remains alive longer than this guard.
   * @throws Nothing.
   */
  explicit ScopedPromiseRelease(std::promise<void>& promise) noexcept
      : promise_(&promise) {}

  /**
   * @brief Releases the gate if explicit test flow did not already release it.
   *
   * @throws Nothing; promise publication failures are suppressed during
   * cleanup.
   */
  ~ScopedPromiseRelease() noexcept { (void)release(); }

  /**
   * @brief Prevents duplicate ownership of one promise release.
   *
   * @param other Guard that retains sole release responsibility.
   * @throws Nothing because copying is unavailable.
   */
  ScopedPromiseRelease(const ScopedPromiseRelease& other) = delete;

  /**
   * @brief Prevents replacing one guard's release responsibility.
   *
   * @param other Guard that remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedPromiseRelease& operator=(const ScopedPromiseRelease& other) = delete;

  /**
   * @brief Publishes the gate value once and disarms this guard.
   *
   * @return True when the gate was already released or publication succeeds;
   * false when `std::promise::set_value()` throws.
   * @throws Nothing.
   * @note The guard remains disarmed after a failed publication because a
   * second attempt cannot repair an invalid promise state.
   */
  bool release() noexcept {
    if (promise_ == nullptr) {
      return true;
    }
    std::promise<void>* promise = promise_;
    promise_ = nullptr;
    try {
      promise->set_value();
      return true;
    } catch (...) {
      return false;
    }
  }

 private:
  /** @brief Borrowed promise, or null after the first release attempt. */
  std::promise<void>* promise_ = nullptr;
};

/**
 * @brief Complete callback-side identity retained for one service submission.
 *
 * @throws std::bad_alloc when graph_identity is copied.
 * @note Numeric Run/local components preserve the complete non-forgeable task
 * identity in a promise-friendly value while descriptor fields prove the
 * callback retained the intended HP or RT child lease.
 */
struct RetainedSubmissionObservation final {
  /** @brief Diagnostic node marker paired with the submission callback. */
  int trace_node_id = -1;

  /** @brief Run id exposed by the callback's retained lease descriptor. */
  uint64_t descriptor_run_id = 0;

  /** @brief Graph/session identity exposed by the retained descriptor. */
  std::string graph_identity;

  /** @brief Authoritative Graph revision exposed by the retained descriptor. */
  uint64_t graph_revision = 0;

  /** @brief Target node exposed by the retained descriptor. */
  int target_node_id = -1;

  /** @brief Single-domain intent exposed by the retained descriptor. */
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;

  /** @brief Output quality exposed by the retained descriptor. */
  ComputeRunQuality quality = ComputeRunQuality::Full;

  /** @brief Run component of the callback's retained task identity. */
  uint64_t identity_run_id = 0;

  /** @brief Local component of the callback's retained task identity. */
  uint64_t local_task_id = 0;
};

/**
 * @brief Snapshots one callback's retained lease and complete task identity.
 *
 * @param trace_node_id Diagnostic marker captured by the callback.
 * @param lease Service-retained lease delivered to the callback.
 * @param identity Service-retained composite task identity.
 * @return Owned observation suitable for cross-thread promise publication.
 * @throws std::bad_alloc when graph identity ownership cannot allocate.
 * @note The helper intentionally records rather than validates the values so
 * final test assertions can distinguish descriptor, identity, and trace-route
 * failures.
 */
RetainedSubmissionObservation observe_retained_submission(
    int trace_node_id, const ComputeRunLease& lease,
    const ComputeRunTaskIdentity& identity) {
  const ComputeRunDescriptor& descriptor = lease.descriptor();
  return RetainedSubmissionObservation{trace_node_id,
                                       descriptor.id().value(),
                                       descriptor.graph_identity(),
                                       descriptor.revision().value(),
                                       descriptor.target_node_id(),
                                       descriptor.intent(),
                                       descriptor.quality(),
                                       identity.run_id().value(),
                                       identity.local_task_id().value()};
}

/**
 * @brief Builds one test submission that explicitly retires its logical unit.
 *
 * @param lease Matching Run lease transferred into the submission.
 * @param local_task_id Run-local identity value.
 * @param trace_node_id Diagnostic node id.
 * @param entered Counter incremented when the service executes the value.
 * @param priority Ready-store ordering hint.
 * @param resource_demand Exact trusted-host task demand.
 * @return Move-owned ready task.
 * @throws std::bad_alloc from metadata or executable ownership.
 * @note The helper is intentionally independent from TaskSubmissionPlan so
 * service boundary validation can be tested in isolation.
 */
ReadyTaskSubmission make_counted_ready_submission(
    ComputeRunLease lease, uint64_t local_task_id, int trace_node_id,
    std::atomic_int& entered,
    ExecutionTaskPriority priority = ExecutionTaskPriority::Normal,
    ReadyTaskResourceDemand resource_demand =
        ReadyTaskSubmission::default_resource_demand()) {
  const ComputeRunTaskIdentity identity = lease.task_identity(local_task_id);
  return ReadyTaskSubmission(
      std::move(lease), identity, trace_node_id, true,
      [&entered](ComputeRunLease& retained_lease,
                 const ComputeRunTaskIdentity& retained_identity,
                 ExecutionTaskRuntime& runtime) {
        if (retained_lease.descriptor().id() != retained_identity.run_id()) {
          throw std::logic_error(
              "Counted submission observed a mismatched retained Run.");
        }
        entered.fetch_add(1, std::memory_order_relaxed);
        runtime.dec_tasks_to_complete();
      },
      priority, resource_demand);
}

/**
 * @brief Waits until public service diagnostics expose an exact ready count.
 *
 * @param service Configured service under concurrent test activity.
 * @param expected Exact number of store-owned ready entries to observe.
 * @return True when the count appears within two seconds; false on timeout.
 * @throws std::bad_alloc from diagnostic string construction.
 * @throws std::system_error when service-state locking fails.
 * @note Dispatch ordering never depends on this polling helper. Tests use it
 * only to prove that every intended candidate is published behind a blocker
 * before releasing the sole worker.
 */
bool wait_for_ready_task_count(const ExecutionService& service,
                               std::uint64_t expected) {
  const std::string needle = ", Ready tasks: " + std::to_string(expected) + ",";
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  do {
    if (service.get_stats().find(needle) != std::string::npos) {
      return true;
    }
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  return service.get_stats().find(needle) != std::string::npos;
}

/**
 * @brief Waits until the authoritative ledger exposes one exact commitment.
 *
 * @param service Configured service whose asynchronous cleanup may still run.
 * @param expected Exact root vector expected after Run settlement.
 * @return True when the vector appears within two seconds; false on timeout.
 * @throws std::system_error when service-state locking fails.
 * @note A Run completion can become observable just before its worker finishes
 * destroying the final ready grant, so release-lifetime tests poll the ledger
 * rather than assuming future readiness is the release linearization point.
 */
bool wait_for_resource_reservation(const ExecutionService& service,
                                   const ResourceVector& expected) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  do {
    if (service.resource_snapshot().reserved == expected) {
      return true;
    }
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  return service.resource_snapshot().reserved == expected;
}

/**
 * @brief Owns one asynchronously submitted policy-test Run and its Host.
 *
 * @throws Nothing for default construction and movement.
 * @note Field order destroys the asynchronous completion first, so Host and
 * Run ownership remain alive until `execute_run()` has returned.
 * Callback-borrowed stack state must be declared before this owner, while any
 * gate release guard must be declared after it, so exceptional unwinding
 * releases gates, joins the Run, and only then destroys borrowed state.
 * Every post-launch adoption moves only these three owners. The compile-time
 * checks below keep those moves non-throwing, while each owning vector reserves
 * its final size before any asynchronous launch so adoption cannot allocate.
 */
struct AsyncPolicyRun final {
  /** @brief Host observation target retained through synchronous settlement. */
  std::unique_ptr<ExecutionServiceHost> host;

  /** @brief Request owner whose leases are captured by ready submissions. */
  std::unique_ptr<ComputeRun> run;

  /** @brief Asynchronous synchronous-Run call completion. */
  std::future<void> completion;
};

static_assert(std::is_nothrow_move_constructible_v<std::future<void>>,
              "Asynchronous completion ownership must move without throwing.");
static_assert(std::is_nothrow_move_assignable_v<std::future<void>>,
              "Asynchronous completion replacement must not throw.");
static_assert(
    std::is_nothrow_move_constructible_v<decltype(AsyncPolicyRun::host)>,
    "Asynchronous Host ownership must move without throwing.");
static_assert(
    std::is_nothrow_move_constructible_v<decltype(AsyncPolicyRun::run)>,
    "Asynchronous Run ownership must move without throwing.");
static_assert(std::is_nothrow_move_constructible_v<AsyncPolicyRun>,
              "Async policy owner adoption must not throw during movement.");
static_assert(std::is_nothrow_move_assignable_v<AsyncPolicyRun>,
              "Async policy owner replacement must not throw during movement.");

/**
 * @brief Publishes one policy-test Run whose initial tasks record markers.
 *
 * @param service Fixed-worker service shared by all competing Runs.
 * @param submission Descriptor carrying explicit immutable QoS.
 * @param markers Ordered marker values assigned to Run-local initial tasks.
 * @param resource_demand Uniform trusted demand for every marker task.
 * @param execution_order Shared callback-order destination.
 * @param execution_order_mutex Serializes marker publication.
 * @return Owned asynchronous Run, Host, and settlement future.
 * @throws std::bad_alloc from Run, Host, vector, callback, or future storage.
 * @throws std::future_error from asynchronous state construction.
 * @throws std::system_error when asynchronous launch fails.
 * @note The helper launches one real `execute_run()` call. It does not
 * bypass admission, ready grants, policy selection, worker dispatch, or Run
 * settlement.
 */
AsyncPolicyRun launch_ordered_policy_run(
    ExecutionService& service, ComputeRunSubmission submission,
    const std::vector<int>& markers, ReadyTaskResourceDemand resource_demand,
    std::vector<int>& execution_order, std::mutex& execution_order_mutex) {
  if (markers.empty()) {
    throw std::invalid_argument("Policy test Run requires one marker.");
  }
  auto host = std::make_unique<ExecutionServiceHost>();
  auto run = std::make_unique<ComputeRun>(std::move(submission));
  std::vector<ReadyTaskSubmission> ready;
  ready.reserve(markers.size());
  for (std::size_t index = 0U; index < markers.size(); ++index) {
    ComputeRunLease lease = run->acquire_lease();
    const ComputeRunTaskIdentity identity = lease.task_identity(index);
    const int marker = markers[index];
    ready.emplace_back(
        std::move(lease), identity, marker, true,
        [&execution_order, &execution_order_mutex, marker](
            ComputeRunLease&, const ComputeRunTaskIdentity&,
            ExecutionTaskRuntime& runtime) {
          {
            std::lock_guard<std::mutex> lock(execution_order_mutex);
            execution_order.push_back(marker);
          }
          runtime.dec_tasks_to_complete();
        },
        ExecutionTaskPriority::Normal, resource_demand);
  }

  ExecutionServiceHost* host_pointer = host.get();
  const int task_count = static_cast<int>(markers.size());
  const CpuRunResourceDemand run_demand{0U, resource_demand};
  std::future<void> completion = std::async(
      std::launch::async, [&service, host_pointer, ready = std::move(ready),
                           task_count, run_demand]() mutable {
        service.execute_run(*host_pointer, "cpu", std::move(ready), task_count,
                            run_demand);
      });
  return AsyncPolicyRun{std::move(host), std::move(run), std::move(completion)};
}

/**
 * @brief Launches one real Run whose sole callback occupies the fixed worker.
 *
 * @param service Fixed-worker service whose policy store will be staged.
 * @param submission Descriptor carrying explicit immutable QoS.
 * @param resource_demand Uniform trusted demand for the sole task.
 * @param entered Promise published immediately after callback entry.
 * @param release Shared gate that controls callback completion.
 * @return Owned asynchronous blocker Run, Host, and settlement future.
 * @throws std::bad_alloc from Run, Host, callback, vector, or future storage.
 * @throws std::future_error from promise or asynchronous state operations.
 * @throws std::system_error when asynchronous launch fails.
 * @note While the gate remains closed, one-worker tests can publish all
 * competing Runs into the real bounded store without dispatch races.
 */
AsyncPolicyRun launch_blocking_policy_run(
    ExecutionService& service, ComputeRunSubmission submission,
    ReadyTaskResourceDemand resource_demand, std::promise<void>& entered,
    std::shared_future<void> release) {
  auto host = std::make_unique<ExecutionServiceHost>();
  auto run = std::make_unique<ComputeRun>(std::move(submission));
  ComputeRunLease lease = run->acquire_lease();
  const ComputeRunTaskIdentity identity = lease.task_identity(0U);
  std::vector<ReadyTaskSubmission> ready;
  ready.emplace_back(
      std::move(lease), identity, 0, true,
      [&entered, release](ComputeRunLease&, const ComputeRunTaskIdentity&,
                          ExecutionTaskRuntime& runtime) {
        entered.set_value();
        release.wait();
        runtime.dec_tasks_to_complete();
      },
      ExecutionTaskPriority::Normal, resource_demand);

  ExecutionServiceHost* host_pointer = host.get();
  const CpuRunResourceDemand run_demand{0U, resource_demand};
  std::future<void> completion = std::async(
      std::launch::async,
      [&service, host_pointer, ready = std::move(ready), run_demand]() mutable {
        service.execute_run(*host_pointer, "cpu", std::move(ready), 1,
                            run_demand);
      });
  return AsyncPolicyRun{std::move(host), std::move(run), std::move(completion)};
}

/**
 * @brief Launches one blocker behind a shared concurrent-admission gate.
 *
 * @param service Fixed-worker service shared by all contenders.
 * @param submission Descriptor carrying explicit immutable QoS.
 * @param resource_demand Uniform trusted demand for the sole task.
 * @param admission_start Gate released after every contender is launched.
 * @param entered Number of callbacks that reached a service worker.
 * @param callback_release Shared gate controlling callback completion.
 * @return Owned asynchronous Run, Host, and settlement future.
 * @throws std::bad_alloc from Run, Host, callback, vector, or future storage.
 * @throws std::future_error from asynchronous state construction.
 * @throws std::system_error when asynchronous launch fails.
 * @note The outer future waits before calling `execute_run()`, so one
 * release exposes concurrent policy admission rather than merely concurrent
 * callback execution.
 */
AsyncPolicyRun launch_concurrent_blocking_policy_run(
    ExecutionService& service, ComputeRunSubmission submission,
    ReadyTaskResourceDemand resource_demand,
    std::shared_future<void> admission_start, std::atomic_int& entered,
    std::shared_future<void> callback_release) {
  auto host = std::make_unique<ExecutionServiceHost>();
  auto run = std::make_unique<ComputeRun>(std::move(submission));
  ComputeRunLease lease = run->acquire_lease();
  const ComputeRunTaskIdentity identity = lease.task_identity(0U);
  std::vector<ReadyTaskSubmission> ready;
  ready.emplace_back(
      std::move(lease), identity, 0, true,
      [&entered, callback_release](ComputeRunLease&,
                                   const ComputeRunTaskIdentity&,
                                   ExecutionTaskRuntime& runtime) {
        entered.fetch_add(1, std::memory_order_release);
        callback_release.wait();
        runtime.dec_tasks_to_complete();
      },
      ExecutionTaskPriority::Normal, resource_demand);

  ExecutionServiceHost* host_pointer = host.get();
  const CpuRunResourceDemand run_demand{0U, resource_demand};
  std::future<void> completion = std::async(
      std::launch::async, [&service, host_pointer, ready = std::move(ready),
                           run_demand, admission_start]() mutable {
        admission_start.wait();
        service.execute_run(*host_pointer, "cpu", std::move(ready), 1,
                            run_demand);
      });
  return AsyncPolicyRun{std::move(host), std::move(run), std::move(completion)};
}

/**
 * @brief Builds one dependent policy-test task that publishes the next link.
 *
 * @param lease Matching Run lease transferred into the current submission.
 * @param index Zero-based marker and task identity for the current link.
 * @param task_count Positive total number of links in the chain.
 * @param resource_demand Uniform trusted demand for every link.
 * @param execution_order Shared callback-order destination.
 * @param execution_order_mutex Serializes marker publication.
 * @return One ready submission whose callback publishes at most one successor.
 * @throws std::invalid_argument for an empty chain or out-of-range index.
 * @throws std::bad_alloc from submission/callback ownership.
 * @note Successors enter the ready store only after the preceding start has
 * advanced the class dispatch counter. This creates genuinely younger
 * competitors for dispatch-aging tests instead of prepublishing peers with
 * the same age.
 */
ReadyTaskSubmission make_dependent_policy_chain_submission(
    ComputeRunLease lease, std::uint64_t index, std::uint64_t task_count,
    ReadyTaskResourceDemand resource_demand, std::vector<int>& execution_order,
    std::mutex& execution_order_mutex) {
  if (task_count == 0U || index >= task_count ||
      index > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("Policy chain task index is invalid.");
  }
  const ComputeRunTaskIdentity identity = lease.task_identity(index);
  const int marker = static_cast<int>(index);
  return ReadyTaskSubmission(
      std::move(lease), identity, marker, index == 0U,
      [index, task_count, resource_demand, &execution_order,
       &execution_order_mutex](ComputeRunLease& retained_lease,
                               const ComputeRunTaskIdentity&,
                               ExecutionTaskRuntime& runtime) {
        {
          std::lock_guard<std::mutex> lock(execution_order_mutex);
          execution_order.push_back(static_cast<int>(index));
        }
        if (index + 1U < task_count) {
          auto& ready_runtime =
              dynamic_cast<ReadyTaskSubmissionRuntime&>(runtime);
          ready_runtime.submit_ready_submission(
              make_dependent_policy_chain_submission(
                  retained_lease, index + 1U, task_count, resource_demand,
                  execution_order, execution_order_mutex));
        }
        runtime.dec_tasks_to_complete();
      },
      ExecutionTaskPriority::Normal, resource_demand);
}

/**
 * @brief Waits for and rethrows every asynchronously submitted policy Run.
 *
 * @param runs Runs that must synchronously settle within two seconds each.
 * @return Nothing.
 * @throws Any service failure retained by a Run future.
 * @note GoogleTest assertions retain per-Run diagnostics while preserving the
 * exact production exception path through `future::get()`.
 */
void expect_policy_runs_settle(std::vector<AsyncPolicyRun>& runs) {
  for (AsyncPolicyRun& run : runs) {
    EXPECT_EQ(run.completion.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
  }
  for (AsyncPolicyRun& run : runs) {
    if (run.completion.wait_for(std::chrono::seconds(0)) ==
        std::future_status::ready) {
      EXPECT_NO_THROW(run.completion.get());
    }
  }
}

/**
 * @brief Converts an exact vector into private service composition limits.
 * @param resources Complete capacity vector.
 * @return Private limits with identical dimensions.
 * @throws Nothing.
 */
ExecutionResourceLimits execution_limits(const ResourceVector& resources) {
  return ExecutionResourceLimits{
      resources.cpu_slots,     resources.retained_memory_bytes,
      resources.scratch_bytes, resources.ready_entries,
      resources.ready_bytes,   ResourceVector{},
  };
}

/**
 * @brief Observes owned large-callable copies across dirty phase boundaries.
 *
 * @throws Nothing from construction or atomic counter access.
 * @note `live_targets` counts only active, non-moved-from task objects. A task
 * move transfers one registration without incrementing the count, while a
 * copy creates a separately owned callable target and increments it.
 */
struct LargeDirtyPhaseTaskProbe final {
  /** @brief Number of large callable bodies entered by service workers. */
  std::atomic_int callback_entries{0};

  /** @brief Number of active large callable target owners. */
  std::atomic_int live_targets{0};
};

/**
 * @brief Large copyable dirty task used to force owned `std::function` storage.
 *
 * The production source-first adapter first moves this value into an outer
 * `std::function`, then copies that function into its source context. The
 * deliberately oversized payload cannot fit inside the complete
 * `std::function` object, so the source copy exercises independently owned
 * callable target storage instead of relying on an implementation's SSO
 * threshold.
 *
 * @throws Nothing from copying, movement, construction, destruction, or valid
 * invocation.
 * @note The test charges the visible callable payload with
 * `owned_callable_retained_memory_bytes(sizeof(LargeDirtyPhaseTask))`; opaque
 * allocator metadata remains outside the same production estimator boundary.
 * Explicit copy/move lifetime accounting lets the source-to-downstream
 * regression observe target ownership without inspecting `std::function`.
 */
class LargeDirtyPhaseTask final {
 public:
  /**
   * @brief Binds the lifetime probe retained by the owning test case.
   * @param probe Probe that outlives every synchronous phase invocation.
   * @throws Nothing.
   * @note Construction registers one active callable target.
   */
  explicit LargeDirtyPhaseTask(LargeDirtyPhaseTaskProbe& probe) noexcept
      : probe_(&probe) {
    probe_->live_targets.fetch_add(1, std::memory_order_relaxed);
  }

  /**
   * @brief Copies one independently owned callable target.
   * @param other Active source target whose payload and probe are copied.
   * @throws Nothing.
   * @note A copy registers one additional active target.
   */
  LargeDirtyPhaseTask(const LargeDirtyPhaseTask& other) noexcept
      : retained_payload_(other.retained_payload_), probe_(other.probe_) {
    if (probe_ != nullptr) {
      probe_->live_targets.fetch_add(1, std::memory_order_relaxed);
    }
  }

  /**
   * @brief Transfers one callable-target registration without duplicating it.
   * @param other Source object made inactive by this move.
   * @throws Nothing.
   * @note The moved-from object retains no probe and contributes no live
   * target.
   */
  LargeDirtyPhaseTask(LargeDirtyPhaseTask&& other) noexcept
      : retained_payload_(std::move(other.retained_payload_)),
        probe_(std::exchange(other.probe_, nullptr)) {}

  /**
   * @brief Releases this object's active callable-target registration.
   * @throws Nothing.
   */
  ~LargeDirtyPhaseTask() noexcept {
    if (probe_ != nullptr) {
      probe_->live_targets.fetch_sub(1, std::memory_order_relaxed);
    }
  }

  /**
   * @brief Prevents replacement of one independently tracked target.
   * @param other Source target that remains unchanged.
   * @return No value because copy assignment is unavailable.
   * @throws Nothing because copy assignment is unavailable.
   */
  LargeDirtyPhaseTask& operator=(const LargeDirtyPhaseTask& other) = delete;

  /**
   * @brief Prevents replacement of one transferred target registration.
   * @param other Source target that remains unchanged.
   * @return No value because move assignment is unavailable.
   * @throws Nothing because move assignment is unavailable.
   */
  LargeDirtyPhaseTask& operator=(LargeDirtyPhaseTask&& other) = delete;

  /**
   * @brief Records execution of one fixture dense task.
   * @param task_id Dense task id that must address a zero-filled payload slot.
   * @return Nothing.
   * @throws std::logic_error if an invalid task id reaches this fixture.
   * @note Production `DirtyReadyTaskContext` owns the invoked copy.
   */
  void operator()(int task_id) const {
    if (task_id < 0 ||
        static_cast<std::size_t>(task_id) >= retained_payload_.size() ||
        retained_payload_.at(static_cast<std::size_t>(task_id)) != 0U) {
      throw std::logic_error("Large dirty phase task received an invalid id.");
    }
    probe_->callback_entries.fetch_add(1, std::memory_order_relaxed);
  }

 private:
  /** @brief Auditable target payload large enough to exclude callable SSO. */
  std::array<std::uint64_t, 64U> retained_payload_{};

  /** @brief Process-local lifetime probe borrowed through settlement. */
  LargeDirtyPhaseTaskProbe* probe_ = nullptr;
};

static_assert(sizeof(LargeDirtyPhaseTask) > sizeof(std::function<void(int)>),
              "Large dirty phase task must exceed std::function storage.");

/**
 * @brief Adversarial holder whose move deliberately preserves its source.
 *
 * The move constructor copies the owned `std::function` target instead of
 * clearing the source. This models the valid-but-unspecified nonempty
 * moved-from state that production must handle without depending on the
 * current standard-library implementation.
 *
 * @throws std::bad_alloc when callable ownership must allocate.
 * @note Null assignment is non-throwing and releases exactly this holder's
 * target. The fixture is private test code and never crosses a product ABI.
 */
class MovePreservingDirtyCallableHolder final {
 public:
  /**
   * @brief Takes one oversized callable into holder ownership.
   * @param task Callable target moved into private `std::function` storage.
   * @throws std::bad_alloc when the non-SSO target allocation fails.
   * @note Successful construction retains exactly one registered target.
   */
  explicit MovePreservingDirtyCallableHolder(LargeDirtyPhaseTask task)
      : task_(std::move(task)) {}

  /**
   * @brief Prevents ordinary copying from bypassing the adversarial move seam.
   * @param other Source holder that remains unchanged.
   * @throws Nothing because copying is unavailable.
   */
  MovePreservingDirtyCallableHolder(
      const MovePreservingDirtyCallableHolder& other) = delete;

  /**
   * @brief Creates a destination target while intentionally preserving source.
   * @param other Source holder left nonempty after this move.
   * @throws std::bad_alloc when copying the non-SSO function target fails.
   * @note Both holders own independently tracked targets after success.
   */
  MovePreservingDirtyCallableHolder(MovePreservingDirtyCallableHolder&& other)
      : task_(other.task_) {}

  /**
   * @brief Prevents replacing an existing adversarial holder by copy.
   * @param other Source holder that remains unchanged.
   * @return No value because copy assignment is unavailable.
   * @throws Nothing because copying is unavailable.
   */
  MovePreservingDirtyCallableHolder& operator=(
      const MovePreservingDirtyCallableHolder& other) = delete;

  /**
   * @brief Prevents replacing an existing adversarial holder by move.
   * @param other Source holder that remains unchanged.
   * @return No value because move assignment is unavailable.
   * @throws Nothing because move assignment is unavailable.
   */
  MovePreservingDirtyCallableHolder& operator=(
      MovePreservingDirtyCallableHolder&& other) = delete;

  /**
   * @brief Explicitly releases this holder's callable target.
   * @param null_value Required null marker.
   * @return This now-empty holder.
   * @throws Nothing.
   */
  MovePreservingDirtyCallableHolder& operator=(
      std::nullptr_t null_value) noexcept {
    task_ = null_value;
    return *this;
  }

  /**
   * @brief Reports whether this holder still owns a callable target.
   * @return True while a target remains live.
   * @throws Nothing.
   */
  bool has_target() const noexcept { return static_cast<bool>(task_); }

  /**
   * @brief Transfers this destination copy into context callable ownership.
   * @return Nonempty function previously owned by this holder.
   * @throws Nothing.
   * @note The explicit exchange leaves this temporary holder empty even when
   * the standard library preserves a moved-from function target.
   */
  std::function<void(int)> release_for_context() && noexcept {
    return std::exchange(task_, nullptr);
  }

 private:
  /** @brief Oversized callable storage copied by the source-preserving move. */
  std::function<void(int)> task_;
};

/**
 * @brief Captures one real execute_run initial-storage retirement event.
 *
 * @throws Nothing from construction or observation.
 * @note The callback stores ordinary fields before publishing
 * observation_count with release ordering. Tests acquire-load that count only
 * after the observed Run has reached callback entry or synchronous settlement.
 */
struct InitialSubmissionStorageProbe final {
  /** @brief Complete root vector admitted by the production Run. */
  ResourceVector admitted_resources;

  /** @brief Moved-from vector element count before retirement. */
  std::size_t staged_size = 0U;

  /** @brief Moved-from vector backing capacity before retirement. */
  std::size_t staged_capacity = 0U;

  /** @brief Vector element count after production retirement. */
  std::size_t released_size = 0U;

  /** @brief Vector backing capacity after production retirement. */
  std::size_t released_capacity = 0U;

  /** @brief Number of production Run boundaries observed. */
  std::atomic_int observation_count{0};

  /**
   * @brief Records one allocation-free production boundary observation.
   * @param context Opaque pointer to this probe.
   * @param resources Complete checked vector admitted for the Run.
   * @param before_size Moved-from element count before retirement.
   * @param before_capacity Backing capacity before retirement.
   * @param after_size Element count after retirement.
   * @param after_capacity Backing capacity after retirement.
   * @return Nothing.
   * @throws Nothing.
   * @note Each owning test observes one synchronous service segment. The
   * release-store makes preceding scalar writes visible to its assertions.
   */
  static void observe(void* context, const ResourceVector& resources,
                      std::size_t before_size, std::size_t before_capacity,
                      std::size_t after_size,
                      std::size_t after_capacity) noexcept {
    auto* probe = static_cast<InitialSubmissionStorageProbe*>(context);
    if (probe == nullptr) {
      return;
    }
    probe->admitted_resources = resources;
    probe->staged_size = before_size;
    probe->staged_capacity = before_capacity;
    probe->released_size = after_size;
    probe->released_capacity = after_capacity;
    probe->observation_count.fetch_add(1, std::memory_order_release);
  }
};

/**
 * @brief Installs and clears one private service observer for a scoped call.
 *
 * @throws Nothing.
 * @note The owning call is synchronous, so destruction always happens after
 * the last possible observation and before the probe leaves scope.
 */
class ScopedInitialSubmissionStorageObserver final {
 public:
  /**
   * @brief Installs the probe on one isolated service.
   * @param service Service used by the owning synchronous product call.
   * @param probe Probe that outlives this guard.
   * @throws Nothing.
   */
  ScopedInitialSubmissionStorageObserver(
      ExecutionService& service, InitialSubmissionStorageProbe& probe) noexcept
      : service_(&service) {
    testing::ExecutionServiceTestAccess::
        set_initial_submission_storage_observer(
            service, &InitialSubmissionStorageProbe::observe, &probe);
  }

  /**
   * @brief Clears the test-only observer.
   * @throws Nothing.
   */
  ~ScopedInitialSubmissionStorageObserver() noexcept {
    testing::ExecutionServiceTestAccess::
        clear_initial_submission_storage_observer(*service_);
  }

  /**
   * @brief Prevents duplicate ownership of one observer installation.
   * @param other Guard that retains sole clearing responsibility.
   * @throws Nothing because copying is unavailable.
   */
  ScopedInitialSubmissionStorageObserver(
      const ScopedInitialSubmissionStorageObserver& other) = delete;

  /**
   * @brief Prevents replacing one observer installation.
   * @param other Guard that remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedInitialSubmissionStorageObserver& operator=(
      const ScopedInitialSubmissionStorageObserver& other) = delete;

 private:
  /** @brief Borrowed isolated service whose observer is cleared. */
  ExecutionService* service_ = nullptr;
};

/**
 * @brief Result of one real process-service dirty source-first phase.
 *
 * @throws Nothing for value construction and movement.
 * @note Expected ledger rejection is captured as a code; all other exception
 * categories remain visible to the calling regression.
 */
struct DirtyCallablePhaseRunResult final {
  /** @brief Graph error code when complete phase admission was rejected. */
  std::optional<GraphErrc> failure_code;

  /** @brief Complete vector observed after successful production admission. */
  std::optional<ResourceVector> admitted_resources;

  /** @brief Root commitments after synchronous success or rejection returns. */
  ResourceVector reserved_after;

  /** @brief Number of large callable bodies entered by service workers. */
  int callback_entries = 0;

  /** @brief Number of production initial-storage boundaries observed. */
  int observation_count = 0;
};

/**
 * @brief Independently calculates one large-callable dirty phase vector.
 *
 * @param compute_plan One-task dirty plan copied by the owned phase context.
 * @param phase_task_ids Exact active source or downstream task ids.
 * @param graph_identity Stable metadata copied into service submissions.
 * @param additional_shared_bytes Other request-owned retained phase storage.
 * @param source_phase True to include the simultaneously live outer callable
 * copy; false for the downstream move-owned target.
 * @return Complete checked service vector for the selected phase.
 * @throws Standard Run, context, estimator, or service exceptions unchanged.
 * @note This calculation never calls `run_dirty_source_first()`. It constructs
 * one context-owned callable explicitly and adds the second source-only target
 * independently, so deleting the production source addition cannot
 * recalibrate this expected vector.
 */
ResourceVector independently_estimate_large_dirty_phase_resources(
    const ComputePlan& compute_plan, const std::vector<int>& phase_task_ids,
    const std::string& graph_identity, std::uint64_t additional_shared_bytes,
    bool source_phase) {
  ExecutionService probe(1U);
  ComputeRun run(
      make_test_submission(graph_identity, 1U, compute_plan.target_node_id));
  if (!run.advance_to(ComputeRunPhase::Admitted) ||
      !run.advance_to(ComputeRunPhase::Queued) ||
      !run.advance_to(ComputeRunPhase::Running)) {
    throw std::logic_error("Dirty callable estimate Run did not start.");
  }

  LargeDirtyPhaseTaskProbe task_probe;
  const std::uint64_t callable_bytes = owned_callable_retained_memory_bytes(
      static_cast<std::uint64_t>(sizeof(LargeDirtyPhaseTask)));
  const std::vector<Device> task_devices(compute_plan.task_graph.tasks.size(),
                                         Device::CPU);
  auto context = std::make_shared<DirtyReadyTaskContext>(
      compute_plan, nullptr, phase_task_ids, task_devices,
      std::function<void(int)>(LargeDirtyPhaseTask(task_probe)), callable_bytes,
      run.acquire_lease(), !source_phase,
      source_phase ? ExecutionTaskPriority::High
                   : ExecutionTaskPriority::Normal);
  std::vector<ReadyTaskSubmission> submissions =
      context->make_submissions(phase_task_ids, true);

  RetainedMemoryEstimator additional(
      "test dirty callable phase retained demand");
  additional.add_bytes(additional_shared_bytes);
  if (source_phase) {
    additional.add_bytes(callable_bytes);
  }
  const CpuRunResourceDemand demand =
      context->run_resource_demand(additional.bytes());
  return probe.estimate_cpu_run_resources(
      submissions.front(), static_cast<int>(phase_task_ids.size()), demand);
}

/**
 * @brief Executes one real large-callable source or downstream dirty phase.
 *
 * @param compute_plan One-task dirty plan used by production source-first code.
 * @param phase_task_ids Exact active task ids for the selected phase.
 * @param graph_identity Stable metadata copied into service submissions.
 * @param additional_shared_bytes Other request-owned retained phase storage.
 * @param source_phase True for a source-only run; false for downstream-only.
 * @param limits Immutable ledger limits applied to this fresh service.
 * @return Admission observation, callback count, and post-settlement ledger.
 * @throws Unexpected standard allocation, Run, context, or worker exceptions
 * unchanged. Expected `GraphError` admission rejection is captured.
 * @note The call enters the same `run_dirty_source_first()` process-service
 * branch used by HP and RT dirty product executors. Every case owns a fresh
 * service and Run so prior callback or ledger state cannot affect its cap.
 */
DirtyCallablePhaseRunResult execute_large_dirty_callable_phase_case(
    const ComputePlan& compute_plan, const std::vector<int>& phase_task_ids,
    const std::string& graph_identity, std::uint64_t additional_shared_bytes,
    bool source_phase, ExecutionResourceLimits limits) {
  ExecutionService service(1U, limits);
  ExecutionServiceHost host;
  InitialSubmissionStorageProbe observation;
  ScopedInitialSubmissionStorageObserver observer(service, observation);
  ComputeRun run(
      make_test_submission(graph_identity, 1U, compute_plan.target_node_id));
  if (!run.advance_to(ComputeRunPhase::Admitted) ||
      !run.advance_to(ComputeRunPhase::Queued) ||
      !run.advance_to(ComputeRunPhase::Running)) {
    throw std::logic_error("Dirty callable product Run did not start.");
  }

  std::vector<int> source_task_ids =
      source_phase ? phase_task_ids : std::vector<int>{};
  std::vector<int> downstream_task_ids =
      source_phase ? std::vector<int>{} : phase_task_ids;
  const std::vector<Device> task_devices(compute_plan.task_graph.tasks.size(),
                                         Device::CPU);
  DirtySourceFirstRunRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.execution_service = &service;
  request.host = &host;
  request.run = &run;
  request.compute_plan = &compute_plan;
  request.task_devices = &task_devices;
  request.additional_shared_retained_memory_bytes = additional_shared_bytes;
  request.source_task_ids = &source_task_ids;
  request.downstream_task_ids = &downstream_task_ids;

  LargeDirtyPhaseTaskProbe task_probe;
  DirtyCallablePhaseRunResult result;
  try {
    run_dirty_source_first(request, LargeDirtyPhaseTask(task_probe));
  } catch (const GraphError& error) {
    result.failure_code = error.code();
  }

  result.observation_count =
      observation.observation_count.load(std::memory_order_acquire);
  if (result.observation_count > 0) {
    result.admitted_resources = observation.admitted_resources;
  }
  result.reserved_after = service.resource_snapshot().reserved;
  result.callback_entries =
      task_probe.callback_entries.load(std::memory_order_relaxed);
  return result;
}

/**
 * @brief Result snapshot for one fresh real dirty product execution.
 *
 * @throws Nothing for value construction and movement.
 */
struct DirtyProductResourceResult final {
  /** @brief Graph error code when product execution was rejected. */
  std::optional<GraphErrc> failure_code;

  /** @brief Complete Run vector observed after successful admission. */
  std::optional<ResourceVector> admitted_resources;

  /** @brief Complete retained bytes owned by the supplied synchronization. */
  std::uint64_t synchronization_bytes = 0U;

  /** @brief Ledger commitments after success or rejection returns. */
  ResourceVector reserved_after;

  /** @brief Committed HP named value retained by the fresh graph. */
  std::int64_t high_precision_value = 0;

  /** @brief Committed RT named value, absent before RT publication. */
  std::optional<std::int64_t> real_time_value;

  /** @brief Run phase reached by the product executor. */
  ComputeRunPhase run_phase = ComputeRunPhase::Created;

  /** @brief Number of initial-storage production boundaries observed. */
  int observation_count = 0;
};

/**
 * @brief Declares the HP/RT descriptor builder used by product-case setup.
 * @param graph_identity Stable GraphRuntime/session identity.
 * @param graph_revision Current authoritative Graph revision.
 * @param node_id Dirty target node.
 * @param intent HP or RT single-domain intent.
 * @return Valid full-HP or interactive-RT submission.
 * @throws std::bad_alloc when graph identity ownership cannot allocate.
 */
ComputeRunSubmission make_dirty_resource_submission(
    const std::string& graph_identity, std::uint64_t graph_revision,
    int node_id, ComputeIntent intent);

/**
 * @brief Executes one fresh real HP or RT dirty product case.
 * @param directory_label Unique temporary-runtime suffix.
 * @param graph_identity Stable service metadata identity shared by interval
 * endpoints.
 * @param node_id Sole real graph/plan node.
 * @param subtype Registered dirty product operation subtype.
 * @param intent HP or RT single-domain path.
 * @param synchronization_node_ids Owner ids; these must include node_id and
 * may add unused ids that do not change the product plan.
 * @param limits Immutable service limits for this endpoint.
 * @return Result containing exact admission, callback-visible output, and
 * post-settlement ledger state.
 * @throws Unexpected allocation, filesystem, graph, or service exceptions
 * unchanged. Expected GraphError rejection is captured in the result.
 * @note Every call rebuilds the same one-node graph and request state so a
 * successful endpoint cannot seed cache/proxy state for the next endpoint.
 */
DirtyProductResourceResult execute_dirty_product_resource_case(
    const std::string& directory_label, const std::string& graph_identity,
    int node_id, const std::string& subtype, ComputeIntent intent,
    const std::vector<int>& synchronization_node_ids,
    ExecutionResourceLimits limits) {
  ScopedResourceRuntimeDirectory directory(directory_label);
  GraphRuntime::Info info;
  info.name = graph_identity;
  info.root = directory.path();
  info.cache_root = directory.path() / "cache";
  GraphRuntime runtime(info);
  GraphModel& graph = runtime.model();
  graph.add_node(make_resource_dirty_product_node(node_id, subtype));
  graph.validate_topology();

  GraphTraversalService traversal;
  GraphEventService events;
  DirtyUpdateRequest request;
  request.node_id = node_id;
  request.cache_precision = "float32";
  request.disable_disk_cache = true;
  request.dirty_roi = PixelRect{0, 0, 8, 8};
  request.suppress_graph_downsample = intent != ComputeIntent::RealTimeUpdate;
  request.node_synchronization =
      std::make_shared<DirtyNodeSynchronization>(synchronization_node_ids);

  DirtyProductResourceResult result;
  result.synchronization_bytes =
      request.node_synchronization->retained_memory_bytes();
  ExecutionService service(1U, limits);
  InitialSubmissionStorageProbe observation;
  ScopedInitialSubmissionStorageObserver observer(service, observation);
  ComputeRun run(make_dirty_resource_submission(
      graph_identity, graph.revision().value(), node_id, intent));
  if (!run.advance_to(ComputeRunPhase::Admitted)) {
    throw std::logic_error("Dirty product Run did not enter admission.");
  }

  HighPrecisionDirtyExecutor hp_executor(traversal, events);
  RealTimeDirtyExecutor rt_executor(traversal, events);
  try {
    if (intent == ComputeIntent::RealTimeUpdate) {
      (void)rt_executor.execute(graph, runtime.realtime_proxy_graph(), &runtime,
                                request, &run, &service);
    } else {
      (void)hp_executor.execute(graph, runtime.realtime_proxy_graph(), &runtime,
                                request, &run, &service);
    }
  } catch (const GraphError& error) {
    result.failure_code = error.code();
  }

  result.observation_count =
      observation.observation_count.load(std::memory_order_acquire);
  if (result.observation_count > 0) {
    result.admitted_resources = observation.admitted_resources;
  }
  result.reserved_after = service.resource_snapshot().reserved;
  result.run_phase = run.phase();
  result.high_precision_value =
      graph.node(node_id)
          .cached_output_high_precision->data.at("value")
          .as_int64();
  if (const NodeOutput* rt_output =
          runtime.realtime_proxy_graph().find_output(node_id);
      rt_output != nullptr) {
    result.real_time_value = rt_output->data.at("value").as_int64();
  }
  return result;
}

/**
 * @brief Estimates the mandatory zero-adapter service envelope.
 * @param graph_identity Metadata identity copied by every logical submission.
 * @param total_task_count Positive logical submission count.
 * @param worker_count Positive fixed service worker count.
 * @return Complete service-only resource vector.
 * @throws Standard Run, service, metadata, or estimator exceptions unchanged.
 * @note The representative declares no adapter bytes, so the result isolates
 * the service Run/submission/ready envelopes used to construct limit
 * intervals for real product adapters.
 */
ResourceVector estimate_service_only_resources(
    const std::string& graph_identity, int total_task_count = 1,
    unsigned int worker_count = 1U) {
  ExecutionService probe(worker_count);
  ComputeRun run(make_test_submission(graph_identity, 1U, 1));
  std::atomic_int entered{0};
  ReadyTaskSubmission representative =
      make_counted_ready_submission(run.acquire_lease(), 0U, 1, entered);
  return probe.estimate_cpu_run_resources(representative, total_task_count,
                                          CpuRunResourceDemand{});
}

/**
 * @brief Proves one exact service-only limit admits a zero-adapter Run.
 * @param graph_identity Metadata identity matching the product rejection path.
 * @param service_only Exact envelope previously calculated for one task.
 * @return Nothing.
 * @throws Standard Run, service, or worker exceptions unchanged.
 * @note Successful settlement and a zero five-dimensional snapshot prove the
 * lower endpoint of the product-test interval is itself usable.
 */
void expect_service_only_envelope_executes(const std::string& graph_identity,
                                           const ResourceVector& service_only) {
  ExecutionService service(1U, execution_limits(service_only));
  ExecutionServiceHost host;
  ComputeRun run(make_test_submission(graph_identity, 1U, 1));
  std::atomic_int entered{0};
  std::vector<ReadyTaskSubmission> submissions;
  submissions.push_back(
      make_counted_ready_submission(run.acquire_lease(), 0U, 1, entered));
  EXPECT_NO_THROW(service.execute_run(host, "cpu", std::move(submissions), 1,
                                      CpuRunResourceDemand{}));
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Proves one predictable map-entry charge closes an exact interval.
 * @param graph_identity Stable metadata label reused for every interval point.
 * @param current_shared_bytes Already retained staging/snapshot bytes.
 * @param missing_entry_bytes Predictable structural bytes for one insertion.
 * @return Nothing.
 * @throws Standard Run, service, metadata, or estimator exceptions unchanged.
 * @note The lower exact capacity admits the current state but rejects the
 * inevitable insertion before callback entry. Adding exactly the missing
 * entry bytes admits the same callback and every path releases its root
 * reservation.
 */
void expect_pending_entry_interval(const std::string& graph_identity,
                                   std::uint64_t current_shared_bytes,
                                   std::uint64_t missing_entry_bytes) {
  ASSERT_GT(missing_entry_bytes, 0U);
  ExecutionService probe(1U);
  std::atomic_int entered{0};
  ComputeRun representative_run(make_test_submission(graph_identity, 1U, 1));
  ReadyTaskSubmission representative = make_counted_ready_submission(
      representative_run.acquire_lease(), 0U, 1, entered);
  const ResourceVector current_required = probe.estimate_cpu_run_resources(
      representative, 1, CpuRunResourceDemand{current_shared_bytes, {}});
  const ResourceVector complete_required = probe.estimate_cpu_run_resources(
      representative, 1,
      CpuRunResourceDemand{current_shared_bytes + missing_entry_bytes, {}});
  EXPECT_EQ(complete_required.retained_memory_bytes -
                current_required.retained_memory_bytes,
            missing_entry_bytes);
  EXPECT_EQ(complete_required.cpu_slots, current_required.cpu_slots);
  EXPECT_EQ(complete_required.scratch_bytes, current_required.scratch_bytes);
  EXPECT_EQ(complete_required.ready_entries, current_required.ready_entries);
  EXPECT_EQ(complete_required.ready_bytes, current_required.ready_bytes);

  ExecutionService lower_service(1U, execution_limits(current_required));
  ExecutionServiceHost lower_host;
  ComputeRun lower_run(make_test_submission(graph_identity, 2U, 1));
  std::vector<ReadyTaskSubmission> lower_ready;
  lower_ready.push_back(
      make_counted_ready_submission(lower_run.acquire_lease(), 0U, 1, entered));
  EXPECT_NO_THROW(lower_service.execute_run(
      lower_host, "cpu", std::move(lower_ready), 1,
      CpuRunResourceDemand{current_shared_bytes, {}}));
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(lower_service.resource_snapshot().reserved, ResourceVector{});

  ComputeRun rejected_run(make_test_submission(graph_identity, 3U, 1));
  std::vector<ReadyTaskSubmission> rejected_ready;
  rejected_ready.push_back(make_counted_ready_submission(
      rejected_run.acquire_lease(), 0U, 1, entered));
  EXPECT_THROW(
      lower_service.execute_run(
          lower_host, "cpu", std::move(rejected_ready), 1,
          CpuRunResourceDemand{current_shared_bytes + missing_entry_bytes, {}}),
      GraphError);
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(lower_service.resource_snapshot().reserved, ResourceVector{});

  ExecutionService complete_service(1U, execution_limits(complete_required));
  ExecutionServiceHost complete_host;
  ComputeRun complete_run(make_test_submission(graph_identity, 4U, 1));
  std::vector<ReadyTaskSubmission> complete_ready;
  complete_ready.push_back(make_counted_ready_submission(
      complete_run.acquire_lease(), 0U, 1, entered));
  EXPECT_NO_THROW(complete_service.execute_run(
      complete_host, "cpu", std::move(complete_ready), 1,
      CpuRunResourceDemand{current_shared_bytes + missing_entry_bytes, {}}));
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 2);
  EXPECT_EQ(complete_service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Builds one HP or RT descriptor for dirty product paths.
 * @param graph_identity Stable GraphRuntime/session identity.
 * @param graph_revision Current authoritative Graph revision.
 * @param node_id Dirty target node.
 * @param intent HP or RT single-domain intent.
 * @return Valid full-HP or interactive-RT submission.
 * @throws std::bad_alloc when graph identity ownership cannot allocate.
 */
ComputeRunSubmission make_dirty_resource_submission(
    const std::string& graph_identity, std::uint64_t graph_revision,
    int node_id, ComputeIntent intent) {
  ComputeRunSubmission submission =
      make_test_submission(graph_identity, graph_revision, node_id);
  submission.intent = intent;
  submission.quality = intent == ComputeIntent::RealTimeUpdate
                           ? ComputeRunQuality::Interactive
                           : ComputeRunQuality::Full;
  return submission;
}

/**
 * @brief Verifies full planning freezes the selected implementation device.
 *
 * @return Nothing; GoogleTest reports selection or fallback failures.
 * @throws Registry, graph, planning, or callback exceptions unchanged.
 * @note The test calls the frozen operation snapshots directly and inspects
 * ready metadata; it does not require a native GPU backend or service worker.
 */
TEST(Issue75DeviceRouting, FullPlanSelectsGpuAndFallsBackToCpu) {
  ensure_issue75_device_operations_registered();
  g_issue75_cpu_operation_calls.store(0, std::memory_order_relaxed);
  g_issue75_gpu_operation_calls.store(0, std::memory_order_relaxed);
  GraphTraversalService traversal;

  GraphModel gpu_graph("cache/issue75-full-gpu");
  Node gpu_node = make_plan_node(501);
  gpu_node.type = "issue75_device_route";
  gpu_node.subtype = "source";
  gpu_graph.add_node(std::move(gpu_node));
  gpu_graph.validate_topology();
  ComputeRun gpu_run(make_test_submission("issue75-full-gpu",
                                          gpu_graph.revision().value(), 501));
  ASSERT_TRUE(gpu_run.advance_to(ComputeRunPhase::Admitted));
  TaskSubmissionPlan& gpu_plan = gpu_run.emplace_submission_plan(
      gpu_graph, traversal, 501,
      std::vector<Device>{Device::GPU_METAL, Device::CPU});
  std::vector<ReadyTaskSubmission> gpu_ready =
      gpu_plan.make_initial_ready_submissions(gpu_run.acquire_lease());
  ASSERT_EQ(gpu_ready.size(), 1U);
  EXPECT_EQ(gpu_ready.front().metadata().device(), Device::GPU_METAL);
  ASSERT_EQ(gpu_plan.resolved_ops().size(), 1U);
  ASSERT_TRUE(gpu_plan.resolved_ops().front().has_value());
  ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(
      *gpu_plan.resolved_ops().front()));
  const NodeOutput gpu_output = std::get<MonolithicOpFunc>(
      *gpu_plan.resolved_ops().front())(gpu_graph.node(501), {});
  EXPECT_EQ(gpu_output.data.at("value").as_int64(), 76);

  GraphModel cpu_graph("cache/issue75-full-cpu");
  Node cpu_node = make_plan_node(502);
  cpu_node.type = "issue75_device_route";
  cpu_node.subtype = "source";
  cpu_graph.add_node(std::move(cpu_node));
  cpu_graph.validate_topology();
  ComputeRun cpu_run(make_test_submission("issue75-full-cpu",
                                          cpu_graph.revision().value(), 502));
  ASSERT_TRUE(cpu_run.advance_to(ComputeRunPhase::Admitted));
  TaskSubmissionPlan& cpu_plan = cpu_run.emplace_submission_plan(
      cpu_graph, traversal, 502, std::vector<Device>{Device::CPU});
  std::vector<ReadyTaskSubmission> cpu_ready =
      cpu_plan.make_initial_ready_submissions(cpu_run.acquire_lease());
  ASSERT_EQ(cpu_ready.size(), 1U);
  EXPECT_EQ(cpu_ready.front().metadata().device(), Device::CPU);
  ASSERT_TRUE(cpu_plan.resolved_ops().front().has_value());
  const NodeOutput cpu_output = std::get<MonolithicOpFunc>(
      *cpu_plan.resolved_ops().front())(cpu_graph.node(502), {});
  EXPECT_EQ(cpu_output.data.at("value").as_int64(), 75);
  EXPECT_EQ(g_issue75_gpu_operation_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_issue75_cpu_operation_calls.load(std::memory_order_relaxed), 1);
}

/**
 * @brief Verifies dirty HP and RT execute frozen GPU operations on the GPU
 * lane.
 *
 * @return Nothing; GoogleTest reports output, worker, or ledger failures.
 * @throws Filesystem, graph, planning, registry, or service exceptions
 * unchanged.
 * @note Each domain uses a fresh fake-Metal GraphRuntime and real source-first
 * service dispatch, so HP cache state cannot seed RT selection.
 */
TEST(Issue75DeviceRouting, DirtyHpAndRtUseFrozenGpuLane) {
  ensure_issue75_device_operations_registered();
  g_issue75_cpu_operation_calls.store(0, std::memory_order_relaxed);
  g_issue75_gpu_operation_calls.store(0, std::memory_order_relaxed);
  g_issue75_gpu_worker_id.store(-1, std::memory_order_relaxed);

  for (const ComputeIntent intent :
       {ComputeIntent::GlobalHighPrecision, ComputeIntent::RealTimeUpdate}) {
    SCOPED_TRACE(intent == ComputeIntent::RealTimeUpdate ? "rt" : "hp");
    const std::string suffix = intent == ComputeIntent::RealTimeUpdate
                                   ? "issue75-dirty-rt"
                                   : "issue75-dirty-hp";
    ScopedResourceRuntimeDirectory directory(suffix);
    GraphRuntime::Info info;
    info.name = suffix;
    info.root = directory.path();
    info.cache_root = directory.path() / "cache";
    info.hp_execution_type = "gpu_pipeline";
    info.rt_execution_type = "gpu_pipeline";
    FakeMetalGraphRuntime runtime(info, true);
    GraphModel& graph = runtime.model();
    Node node = make_resource_dirty_product_node(503, "source");
    node.type = "issue75_device_route";
    graph.add_node(std::move(node));
    graph.validate_topology();

    DirtyUpdateRequest request;
    request.node_id = 503;
    request.cache_precision = "float32";
    request.disable_disk_cache = true;
    request.dirty_roi = PixelRect{0, 0, 8, 8};
    request.suppress_graph_downsample = true;
    GraphTraversalService traversal;
    GraphEventService events;
    ExecutionService service(1U);
    ComputeRun run(make_dirty_resource_submission(
        suffix, graph.revision().value(), 503, intent));
    ASSERT_TRUE(run.advance_to(ComputeRunPhase::Admitted));

    if (intent == ComputeIntent::RealTimeUpdate) {
      RealTimeDirtyExecutor executor(traversal, events);
      const NodeOutput& output =
          executor.execute(graph, runtime.realtime_proxy_graph(), &runtime,
                           request, &run, &service);
      EXPECT_EQ(output.data.at("value").as_int64(), 76);
    } else {
      HighPrecisionDirtyExecutor executor(traversal, events);
      const NodeOutput& output =
          executor.execute(graph, runtime.realtime_proxy_graph(), &runtime,
                           request, &run, &service);
      EXPECT_EQ(output.data.at("value").as_int64(), 76);
    }
    EXPECT_EQ(g_issue75_gpu_worker_id.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
  }

  EXPECT_EQ(g_issue75_gpu_operation_calls.load(std::memory_order_relaxed), 2);
  EXPECT_EQ(g_issue75_cpu_operation_calls.load(std::memory_order_relaxed), 0);
}

/**
 * @brief Verifies connected-parameter preflight uses route-aware GPU selection.
 *
 * @return Nothing; GoogleTest reports snapshot, worker, or ledger failures.
 * @throws Graph, registry, service, or callback exceptions unchanged.
 * @note A fake-Metal Host drives the real preflight service path without a
 * native backend; the selected callback remains a normal host function.
 */
TEST(Issue75DeviceRouting, ConnectedParameterPreflightUsesGpuLane) {
  ensure_issue75_device_operations_registered();
  g_issue75_cpu_operation_calls.store(0, std::memory_order_relaxed);
  g_issue75_gpu_operation_calls.store(0, std::memory_order_relaxed);

  GraphModel graph("cache/issue75-preflight-gpu");
  Node producer = make_plan_node(504);
  producer.type = "issue75_device_route";
  producer.subtype = "source";
  Node target = make_plan_node(505);
  target.parameter_inputs.push_back({504, "value", "radius"});
  graph.add_node(std::move(producer));
  graph.add_node(std::move(target));
  graph.validate_topology();
  GraphTraversalService traversal;
  ExecutionService service(1U);
  ExecutionServiceHost host(true);
  ComputeRun run(make_test_submission("issue75-preflight-gpu",
                                      graph.revision().value(), 505));
  ASSERT_TRUE(run.advance_to(ComputeRunPhase::Admitted));

  std::shared_ptr<const StabilizedDirtyParameters> stabilized =
      stabilize_connected_dirty_parameters(
          graph, traversal, 505, 1U, graph.topology_generation(), nullptr,
          &service, &host, &run, nullptr, "gpu_pipeline");
  ASSERT_TRUE(stabilized);
  const NodeOutput* output = stabilized->find_parameter_output(504);
  ASSERT_NE(output, nullptr);
  EXPECT_EQ(output->data.at("value").as_int64(), 76);
  EXPECT_EQ(g_issue75_gpu_operation_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_issue75_cpu_operation_calls.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(host.last_worker_id(), 1);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Non-CPU resource dimension reduced below one calculated Run vector.
 */
enum class RejectedResourceDimension {
  /** @brief Host-owned retained-memory capacity. */
  RetainedMemory,
  /** @brief Host-declared execution scratch capacity. */
  Scratch,
  /** @brief Bounded ready-store entry capacity. */
  ReadyEntries,
  /** @brief Bounded ready-store byte capacity. */
  ReadyBytes,
};

/**
 * @brief Verifies one non-CPU Run dimension rejects atomically and recovers.
 *
 * @param label Stable graph identity prefix for rejected/recovery Runs.
 * @param rejected_dimension Calculated vector field reduced by one byte/entry.
 * @param rejected_demand Uniform demand that exceeds exactly one tested
 * dimension after whole-Run aggregation.
 * @param rejected_task_count Positive logical count used for aggregation.
 * @return Nothing.
 * @throws std::bad_alloc or std::system_error from test/service setup.
 * @note GoogleTest failures retain unexpected exception or accounting
 * evidence. The recovery Run uses a one-unit demand in every task dimension.
 */
void expect_resource_dimension_rejection_and_recovery(
    const std::string& label, RejectedResourceDimension rejected_dimension,
    ReadyTaskResourceDemand rejected_demand, int rejected_task_count) {
  ExecutionServiceHost host;
  std::atomic_int entered{0};
  ComputeRun rejected_run(make_test_submission(label + "-rejected", 1U, 1));
  std::vector<ReadyTaskSubmission> rejected_ready;
  for (int task_index = 0; task_index < rejected_task_count; ++task_index) {
    rejected_ready.push_back(make_counted_ready_submission(
        rejected_run.acquire_lease(), static_cast<std::uint64_t>(task_index),
        task_index + 1, entered, ExecutionTaskPriority::Normal,
        rejected_demand));
  }
  const CpuRunResourceDemand rejected_run_demand{0U, rejected_demand};
  ExecutionService probe(1U);
  ResourceVector required = probe.estimate_cpu_run_resources(
      rejected_ready.front(), rejected_task_count, rejected_run_demand);
  switch (rejected_dimension) {
    case RejectedResourceDimension::RetainedMemory:
      ASSERT_GT(required.retained_memory_bytes, 0U);
      --required.retained_memory_bytes;
      break;
    case RejectedResourceDimension::Scratch:
      ASSERT_GT(required.scratch_bytes, 0U);
      --required.scratch_bytes;
      break;
    case RejectedResourceDimension::ReadyEntries:
      ASSERT_GT(required.ready_entries, 0U);
      --required.ready_entries;
      break;
    case RejectedResourceDimension::ReadyBytes:
      ASSERT_GT(required.ready_bytes, 0U);
      --required.ready_bytes;
      break;
  }
  ExecutionService service(1U, execution_limits(required));

  bool rejected = false;
  try {
    service.execute_run(host, "cpu", std::move(rejected_ready),
                        rejected_task_count, rejected_run_demand);
  } catch (const GraphError& error) {
    rejected = true;
    EXPECT_EQ(error.code(), GraphErrc::ComputeError);
  } catch (const std::exception& error) {
    ADD_FAILURE() << "Unexpected admission exception: " << error.what();
  } catch (...) {
    ADD_FAILURE() << "Unexpected non-standard admission exception.";
  }
  EXPECT_TRUE(rejected);
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});

  constexpr ReadyTaskResourceDemand kRecoveryDemand{1U, 1U, 1U};
  ComputeRun recovery_run(make_test_submission(label + "-recovery", 2U, 2));
  std::vector<ReadyTaskSubmission> recovery_ready;
  recovery_ready.push_back(make_counted_ready_submission(
      recovery_run.acquire_lease(), 0U, 2, entered,
      ExecutionTaskPriority::Normal, kRecoveryDemand));
  EXPECT_NO_THROW(
      service.execute_run(host, "cpu", std::move(recovery_ready), 1,
                          CpuRunResourceDemand{0U, kRecoveryDemand}));
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies immutable Run request identity and explicit QoS.
 *
 * @note Two live Runs must never receive the same process-lifetime identity.
 */
TEST(ComputeRunDescriptor, CapturesIdRevisionIntentQualityAndQosWithoutReuse) {
  ComputeRun first(make_test_submission("session-a", 17, 42));
  ComputeRun second(make_test_submission("session-b", 18, 43));

  EXPECT_NE(first.descriptor().id().value(), 0U);
  EXPECT_NE(first.descriptor().id(), second.descriptor().id());
  EXPECT_EQ(first.descriptor().graph_identity(), "session-a");
  EXPECT_EQ(first.descriptor().graph_instance_id().value(), 17U);
  EXPECT_EQ(first.descriptor().revision().value(), 17U);
  EXPECT_EQ(first.descriptor().target_node_id(), 42);
  EXPECT_EQ(first.descriptor().intent(), ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(first.descriptor().quality(), ComputeRunQuality::Full);
  EXPECT_EQ(first.descriptor().qos().service_class,
            ComputeRunQosClass::Throughput);
  EXPECT_FALSE(first.descriptor().qos().deadline.has_value());
  EXPECT_EQ(first.descriptor().qos().weight, 3U);
  ASSERT_TRUE(first.descriptor().qos().maximum_parallelism.has_value());
  EXPECT_EQ(*first.descriptor().qos().maximum_parallelism, 2U);
}

/**
 * @brief Verifies strong Graph values reject zero and revisions stop at max.
 * @note Identity reservation exhaustion is covered separately through the
 * exact production CAS algorithm and an isolated atomic counter.
 */
TEST(GraphRevision, RejectsIllegalValuesAndChecksMaximumSuccessor) {
  EXPECT_THROW((void)GraphInstanceId{0U}, std::invalid_argument);
  EXPECT_THROW((void)GraphRevision{0U}, std::invalid_argument);
  EXPECT_EQ(GraphRevision::initial().value(), 1U);

  const GraphInstanceId maximum_identity{std::numeric_limits<uint64_t>::max()};
  EXPECT_EQ(maximum_identity.value(), std::numeric_limits<uint64_t>::max());

  const GraphRevision penultimate{std::numeric_limits<uint64_t>::max() - 1U};
  EXPECT_EQ(penultimate.next().value(), std::numeric_limits<uint64_t>::max());
  const GraphRevision maximum_revision{std::numeric_limits<uint64_t>::max()};
  EXPECT_THROW((void)maximum_revision.next(), std::overflow_error);
}

/**
 * @brief Exercises the production identity CAS loop at its terminal value.
 * @return Nothing; GoogleTest assertions report reuse, wrap, or race failures.
 * @throws std::bad_alloc or std::system_error if async fixture setup fails.
 * @note Eight workers start from one shared gate and one penultimate isolated
 * counter. Exactly one may reserve UINT64_MAX; every peer must observe terminal
 * exhaustion. The production process counter remains inaccessible and
 * untouched.
 */
TEST(GraphRevision, ConcurrentIdentityMintIssuesMaximumOnceThenExhausts) {
  constexpr std::size_t kWorkerCount = 8;
  const uint64_t maximum = std::numeric_limits<uint64_t>::max();
  std::atomic<uint64_t> last_issued{maximum - 1U};
  std::promise<void> start;
  const std::shared_future<void> start_signal = start.get_future().share();
  std::vector<std::future<std::optional<GraphInstanceId>>> workers;
  workers.reserve(kWorkerCount);
  ScopedIdentityMintWorkerRecovery recovery(start, workers);

  for (std::size_t worker = 0; worker < kWorkerCount; ++worker) {
    workers.emplace_back(std::async(std::launch::async, [&] {
      start_signal.wait();
      try {
        return std::optional<GraphInstanceId>{
            testing::GraphInstanceIdTestAccess::mint_from(last_issued)};
      } catch (const std::overflow_error&) {
        return std::optional<GraphInstanceId>{};
      }
    }));
  }

  recovery.release();
  std::size_t maximum_issuances = 0;
  std::size_t exhaustion_count = 0;
  for (auto& worker : workers) {
    ASSERT_EQ(worker.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    const std::optional<GraphInstanceId> result = worker.get();
    if (result) {
      EXPECT_EQ(result->value(), maximum);
      ++maximum_issuances;
    } else {
      ++exhaustion_count;
    }
  }

  EXPECT_EQ(maximum_issuances, 1U);
  EXPECT_EQ(exhaustion_count, kWorkerCount - 1U);
  EXPECT_EQ(last_issued.load(std::memory_order_relaxed), maximum);
  EXPECT_THROW((void)testing::GraphInstanceIdTestAccess::mint_from(last_issued),
               std::overflow_error);
  EXPECT_EQ(last_issued.load(std::memory_order_relaxed), maximum);
}

/**
 * @brief Proves revision exhaustion preserves a prepared structural mutation.
 * @return Nothing; GoogleTest assertions report graph or generation changes.
 * @throws Graph, filesystem, or allocation exceptions from fixture setup.
 * @note The private seam places a real Graph at UINT64_MAX only after its
 * baseline node is published. `add_node()` still builds and validates a
 * candidate, then must fail before the no-throw structural swap.
 */
TEST(GraphRevision, StructuralMutationOverflowPreservesPublishedGraph) {
  GraphModel graph(std::filesystem::path{});
  graph.add_node(make_plan_node(1));
  const uint64_t topology_before = graph.topology_generation();
  testing::GraphModelTestAccess::set_revision(
      graph, GraphRevision{std::numeric_limits<uint64_t>::max()});

  EXPECT_THROW(graph.add_node(make_plan_node(2)), std::overflow_error);
  EXPECT_EQ(graph.revision().value(), std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(graph.topology_generation(), topology_before);
  EXPECT_EQ(graph.node_count(), 1U);
  EXPECT_TRUE(graph.has_node(1));
  EXPECT_FALSE(graph.has_node(2));
  EXPECT_TRUE(graph.upstream_edges(1).empty());
  EXPECT_TRUE(graph.downstream_edges(1).empty());
}

/**
 * @brief Verifies live Graph identity non-reuse and snapshot provenance.
 * @note Structural success advances revision once; failed validation and
 * compute-state publication preserve the current authoritative revision.
 */
TEST(GraphRevision, GraphMutationAndComputeSnapshotPreserveAuthority) {
  GraphModel first(std::filesystem::path{});
  GraphModel second(std::filesystem::path{});
  EXPECT_NE(first.instance_id(), second.instance_id());
  EXPECT_EQ(first.revision(), GraphRevision::initial());

  Node node = make_plan_node(1);
  first.add_node(node);
  EXPECT_EQ(first.revision().value(), 2U);
  const GraphRevision after_add = first.revision();
  EXPECT_THROW(first.add_node(node), GraphError);
  EXPECT_EQ(first.revision(), after_add);

  std::unique_ptr<GraphModel> snapshot = first.clone_for_compute();
  EXPECT_TRUE(snapshot->is_compute_snapshot());
  EXPECT_EQ(snapshot->instance_id(), first.instance_id());
  EXPECT_EQ(snapshot->revision(), first.revision());
  snapshot->mutate_node_runtime_state(
      1, [](GraphModel::NodeRuntimeState& state) { state.hp_version = 7; });
  first.publish_compute_snapshot(*snapshot);
  EXPECT_EQ(first.node(1).hp_version, 7);
  EXPECT_EQ(first.revision(), after_add);
}

/**
 * @brief Verifies RT child quality and invalid quality/QoS rejection.
 *
 * @note The interactive RT descriptor is accepted. Each mismatched or
 * nonpositive descriptor is rejected before it can become an admitted Run.
 */
TEST(ComputeRunDescriptor, AcceptsRtChildAndRejectsMismatchedQualityOrQos) {
  ComputeRunSubmission realtime = make_test_submission("rt", 1, 1);
  realtime.intent = ComputeIntent::RealTimeUpdate;
  realtime.quality = ComputeRunQuality::Interactive;
  ComputeRun realtime_run(realtime);
  EXPECT_EQ(realtime_run.descriptor().intent(), ComputeIntent::RealTimeUpdate);
  EXPECT_EQ(realtime_run.descriptor().quality(),
            ComputeRunQuality::Interactive);

  ComputeRunSubmission mismatched_realtime = std::move(realtime);
  mismatched_realtime.quality = ComputeRunQuality::Full;
  EXPECT_THROW((void)ComputeRun(std::move(mismatched_realtime)),
               std::invalid_argument);

  ComputeRunSubmission zero_weight = make_test_submission("weight", 1, 1);
  zero_weight.qos.weight = 0;
  EXPECT_THROW((void)ComputeRun(std::move(zero_weight)), std::invalid_argument);

  ComputeRunSubmission zero_parallelism =
      make_test_submission("parallelism", 1, 1);
  zero_parallelism.qos.maximum_parallelism = 0;
  EXPECT_THROW((void)ComputeRun(std::move(zero_parallelism)),
               std::invalid_argument);
}

/**
 * @brief Verifies forward skips, duplicate phases, and backward rejection.
 *
 * @note Terminal entry is accepted only through the terminal arbiter.
 */
TEST(ComputeRunLifecycle, AdvancesMonotonicallyAndSettlesSuccessOnce) {
  ComputeRun run(make_test_submission("phase", 9, 3));
  const ComputeRunCancellationSource cancellation = run.cancellation_source();

  EXPECT_EQ(run.phase(), ComputeRunPhase::Created);
  EXPECT_TRUE(run.advance_to(ComputeRunPhase::Admitted));
  EXPECT_FALSE(run.advance_to(ComputeRunPhase::Admitted));
  EXPECT_TRUE(run.advance_to(ComputeRunPhase::Running));
  EXPECT_THROW((void)run.advance_to(ComputeRunPhase::Queued), std::logic_error);
  EXPECT_THROW((void)run.advance_to(ComputeRunPhase::Terminal),
               std::invalid_argument);

  EXPECT_TRUE(run.publish_succeeded());
  EXPECT_EQ(run.phase(), ComputeRunPhase::Terminal);
  EXPECT_FALSE(run.advance_to(ComputeRunPhase::CommitPending));
  EXPECT_FALSE(run.publish_succeeded());
  EXPECT_FALSE(cancellation.request_cancellation(
      ComputeRunCancellationReason::ExplicitRequest));
  EXPECT_FALSE(
      run.publish_failed(std::make_exception_ptr(std::runtime_error("late"))));

  const auto terminal = run.terminal_outcome();
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->kind, ComputeRunTerminalKind::Succeeded);
  EXPECT_FALSE(terminal->failure);
  EXPECT_FALSE(terminal->cancellation_reason.has_value());
}

/**
 * @brief Verifies failure outcome preserves the original exception object.
 *
 * @note Rethrowing the copied exception_ptr exercises identity-preserving
 * storage instead of reconstructing an error from text.
 */
TEST(ComputeRunLifecycle, PreservesOriginalFailureException) {
  ComputeRun run(make_test_submission("failure", 4, 5));
  const std::exception_ptr failure =
      std::make_exception_ptr(std::runtime_error("compute-run-failure"));

  ASSERT_TRUE(run.publish_failed(failure));
  const auto terminal = run.terminal_outcome();
  ASSERT_TRUE(terminal.has_value());
  ASSERT_EQ(terminal->kind, ComputeRunTerminalKind::Failed);
  ASSERT_TRUE(terminal->failure);
  EXPECT_TRUE(terminal->failure == failure);
  try {
    std::rethrow_exception(terminal->failure);
    FAIL() << "Expected stored ComputeRun failure.";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "compute-run-failure");
  }
}

/**
 * @brief Verifies concurrent success/failure/cancellation have one winner.
 *
 * @note Threads share only the Run terminal arbiter and an allocation-free
 * start flag. Every contender is joined before the outcome is inspected.
 */
TEST(ComputeRunLifecycle, ConcurrentTerminalContendersSelectExactlyOne) {
  ComputeRun run(make_test_submission("terminal-race", 6, 7));
  const ComputeRunCancellationSource cancellation = run.cancellation_source();
  std::atomic_bool start{false};
  std::atomic_int accepted{0};
  std::vector<std::thread> contenders;
  contenders.reserve(12);

  for (int index = 0; index < 12; ++index) {
    contenders.emplace_back([&, index]() {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      bool won = false;
      switch (index % 3) {
        case 0:
          won = run.publish_succeeded();
          break;
        case 1:
          won = run.publish_failed(
              std::make_exception_ptr(std::runtime_error("race failure")));
          break;
        default:
          won = cancellation.request_cancellation(
              ComputeRunCancellationReason::ExplicitRequest);
          break;
      }
      if (won) {
        accepted.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (auto& contender : contenders) {
    contender.join();
  }

  EXPECT_EQ(accepted.load(std::memory_order_relaxed), 1);
  EXPECT_TRUE(run.is_terminal());
  EXPECT_TRUE(run.terminal_outcome().has_value());
}

/**
 * @brief Verifies explicit cancellation keeps one stable reason and callback.
 *
 * @return Nothing; GoogleTest assertions report arbiter, notification, or
 * quiescence failures.
 * @throws Allocation or synchronization exceptions from fixture setup.
 * @note A live lease proves terminal publication precedes physical quiescence;
 * registering after cancellation invokes immediately but retains no slot.
 */
TEST(ComputeRunCancellation,
     ExplicitRequestIsIdempotentStableAndTerminalBeforeQuiescent) {
  ComputeRun run(make_test_submission("explicit-cancellation", 31, 41));
  const ComputeRunCancellationSource source = run.cancellation_source();
  std::atomic_int notifications{0};
  std::optional<ComputeRunCancellationReason> notified_reason;

  {
    ComputeRunLease lease = run.acquire_lease();
    ComputeRunCancellationRegistration registration =
        lease.register_cancellation_notification(
            [&](ComputeRunCancellationReason reason) {
              notified_reason = reason;
              notifications.fetch_add(1, std::memory_order_relaxed);
            });
    ASSERT_TRUE(registration.active());

    EXPECT_TRUE(
        source.request_cancellation(ComputeRunCancellationReason::GraphClose));
    EXPECT_FALSE(source.request_cancellation(
        ComputeRunCancellationReason::ExplicitRequest));
    EXPECT_EQ(lease.observe_cancellation(),
              ComputeRunCancellationReason::GraphClose);
    EXPECT_TRUE(run.is_terminal());
    EXPECT_FALSE(run.is_quiescent());
    EXPECT_EQ(notifications.load(std::memory_order_relaxed), 1);
    ASSERT_TRUE(notified_reason.has_value());
    EXPECT_EQ(*notified_reason, ComputeRunCancellationReason::GraphClose);

    ComputeRunCancellationRegistration late =
        lease.register_cancellation_notification(
            [&](ComputeRunCancellationReason reason) {
              notified_reason = reason;
              notifications.fetch_add(1, std::memory_order_relaxed);
            });
    EXPECT_FALSE(late.active());
    EXPECT_EQ(notifications.load(std::memory_order_relaxed), 2);
  }

  EXPECT_TRUE(run.is_quiescent());
  const auto outcome = run.terminal_outcome();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->kind, ComputeRunTerminalKind::Cancelled);
  EXPECT_EQ(outcome->cancellation_reason,
            ComputeRunCancellationReason::GraphClose);
}

/**
 * @brief Verifies an injected monotonic deadline wins without wall-clock sleep.
 *
 * @return Nothing; GoogleTest assertions report deadline-arbiter failures.
 * @throws Allocation or synchronization exceptions from fixture setup.
 * @note Equality with the immutable deadline is expired. A later explicit
 * request cannot replace `DeadlineExceeded` or invoke the slot again.
 */
TEST(ComputeRunCancellation, InjectedDeadlineUsesTheSharedTerminalArbiter) {
  using Clock = std::chrono::steady_clock;
  Clock::time_point now = Clock::time_point{} + std::chrono::seconds(10);
  ComputeRunSubmission submission =
      make_test_submission("deadline-cancellation", 32, 42);
  submission.qos.deadline = now + std::chrono::seconds(5);
  ComputeRun run(std::move(submission), [&now]() noexcept { return now; });
  ComputeRunLease lease = run.acquire_lease();
  const ComputeRunCancellationSource source = run.cancellation_source();
  std::atomic_int notifications{0};
  ComputeRunCancellationRegistration registration =
      lease.register_cancellation_notification(
          [&](ComputeRunCancellationReason reason) {
            EXPECT_EQ(reason, ComputeRunCancellationReason::DeadlineExceeded);
            notifications.fetch_add(1, std::memory_order_relaxed);
          });

  EXPECT_FALSE(lease.observe_cancellation().has_value());
  now += std::chrono::seconds(5);
  EXPECT_EQ(lease.observe_cancellation(),
            ComputeRunCancellationReason::DeadlineExceeded);
  EXPECT_EQ(notifications.load(std::memory_order_relaxed), 1);
  EXPECT_FALSE(source.request_cancellation(
      ComputeRunCancellationReason::ExplicitRequest));
  EXPECT_EQ(lease.observe_cancellation(),
            ComputeRunCancellationReason::DeadlineExceeded);
  EXPECT_EQ(notifications.load(std::memory_order_relaxed), 1);
}

/**
 * @brief Verifies cancellation and visible-commit claims are mutually
 * exclusive.
 *
 * @return Nothing; GoogleTest assertions report commit/cancellation ordering
 * failures.
 * @throws Allocation or synchronization exceptions from fixture setup.
 * @note Cancellation before claim denies commit, while an accepted contender
 * rejects every later cancellation/failure until that contender resolves.
 */
TEST(ComputeRunCommitArbiter, LinearizesCancellationBeforeOrAfterCommitClaim) {
  ComputeRun cancelled(make_test_submission("cancel-before-commit", 33, 43));
  ComputeRunLease cancelled_lease = cancelled.acquire_lease();
  ASSERT_TRUE(cancelled.advance_to(ComputeRunPhase::CommitPending));
  EXPECT_TRUE(cancelled.cancellation_source().request_cancellation(
      ComputeRunCancellationReason::ExplicitRequest));
  EXPECT_FALSE(cancelled_lease.try_claim_commit().has_value());
  ASSERT_TRUE(cancelled.terminal_outcome().has_value());
  EXPECT_EQ(cancelled.terminal_outcome()->kind,
            ComputeRunTerminalKind::Cancelled);

  ComputeRun committed(make_test_submission("commit-before-cancel", 34, 44));
  ComputeRunLease committed_lease = committed.acquire_lease();
  ASSERT_TRUE(committed.advance_to(ComputeRunPhase::CommitPending));
  std::optional<ComputeRunCommitContender> contender =
      committed_lease.try_claim_commit();
  ASSERT_TRUE(contender.has_value());
  EXPECT_FALSE(committed.cancellation_source().request_cancellation(
      ComputeRunCancellationReason::ExplicitRequest));
  EXPECT_FALSE(committed.publish_failed(
      std::make_exception_ptr(std::runtime_error("late failure"))));
  EXPECT_FALSE(committed.is_terminal());
  EXPECT_TRUE(contender->publish_succeeded());
  ASSERT_TRUE(committed.terminal_outcome().has_value());
  EXPECT_EQ(committed.terminal_outcome()->kind,
            ComputeRunTerminalKind::Succeeded);
}

/**
 * @brief Verifies one private request source fans out to independent child
 * Runs.
 *
 * @return Nothing; GoogleTest assertions report request fanout failures.
 * @throws Allocation or synchronization exceptions from fixture setup.
 * @note Duplicate attachment is idempotent and a child attached after the first
 * request is synchronously cancelled with the same stable reason.
 */
TEST(ComputeRunCancellation, RequestSourceFansOutAndCancelsLateChildren) {
  ComputeRequestCancellationSource request_source;
  ComputeRun hp(make_test_submission("fanout-hp", 35, 45));
  ComputeRunSubmission rt_submission = make_policy_submission(
      "fanout-rt", 36, 45, ComputeRunQosClass::Interactive);
  rt_submission.intent = ComputeIntent::RealTimeUpdate;
  rt_submission.quality = ComputeRunQuality::Interactive;
  ComputeRun rt(std::move(rt_submission));
  ComputeRun late(make_test_submission("fanout-late", 37, 45));
  ComputeRunLease hp_lease = hp.acquire_lease();
  ComputeRunLease rt_lease = rt.acquire_lease();
  ComputeRunLease late_lease = late.acquire_lease();

  request_source.attach(hp);
  request_source.attach(rt);
  request_source.attach(hp);
  EXPECT_TRUE(request_source.request_cancellation());
  EXPECT_FALSE(request_source.request_cancellation());
  request_source.attach(late);

  EXPECT_EQ(request_source.accepted_reason(),
            ComputeRunCancellationReason::ExplicitRequest);
  EXPECT_EQ(hp_lease.observe_cancellation(),
            ComputeRunCancellationReason::ExplicitRequest);
  EXPECT_EQ(rt_lease.observe_cancellation(),
            ComputeRunCancellationReason::ExplicitRequest);
  EXPECT_EQ(late_lease.observe_cancellation(),
            ComputeRunCancellationReason::ExplicitRequest);
  EXPECT_NE(hp.descriptor().id(), rt.descriptor().id());
  EXPECT_NE(rt.descriptor().id(), late.descriptor().id());
}

/**
 * @brief Verifies full HP plan, dependency, identity, and temp ownership.
 *
 * @note The plan is constructed after admission and remains address-stable
 * until passive Run destruction.
 */
TEST(ComputeRunStorage, OwnsOneFullHpSubmissionPlanAndTemporarySlots) {
  GraphModel graph("cache/compute-run-plan-storage");
  graph.add_node(make_plan_node(11));
  graph.validate_topology();
  GraphTraversalService traversal;
  ComputeRun run(
      make_test_submission("plan-storage", graph.revision().value(), 11));
  ASSERT_TRUE(run.advance_to(ComputeRunPhase::Admitted));

  TaskSubmissionPlan& plan = run.emplace_submission_plan(
      graph, traversal, 11, std::vector<Device>{Device::CPU});

  EXPECT_EQ(run.submission_plan(), &plan);
  ASSERT_EQ(plan.execution_order().size(), 1U);
  EXPECT_EQ(plan.execution_order().front(), 11);
  EXPECT_EQ(plan.temp_results().size(), plan.execution_order().size());
  EXPECT_THROW((void)run.emplace_submission_plan(
                   graph, traversal, 11, std::vector<Device>{Device::CPU}),
               std::logic_error);
}

/**
 * @brief Verifies standalone dirty HP staging is installed once under the Run.
 *
 * @note Terminal Runs reject late storage creation so cleanup cannot revive
 * request-local output state.
 */
TEST(ComputeRunStorage, OwnsOneStandaloneDirtyHpWriteBuffer) {
  ComputeRun run(make_test_submission("dirty-storage", 2, 8));
  ASSERT_TRUE(run.advance_to(ComputeRunPhase::Admitted));

  HighPrecisionDirtyWriteBuffer& buffer =
      run.emplace_dirty_hp_write_buffer(false);
  EXPECT_EQ(run.dirty_hp_write_buffer(), &buffer);
  EXPECT_THROW((void)run.emplace_dirty_hp_write_buffer(true), std::logic_error);

  ComputeRun terminal_run(make_test_submission("terminal-storage", 2, 8));
  ASSERT_TRUE(terminal_run.publish_succeeded());
  EXPECT_THROW((void)terminal_run.emplace_dirty_hp_write_buffer(true),
               std::logic_error);
}

/**
 * @brief Verifies a lease retains Run state after its request observer leaves.
 *
 * @note Observer destruction is intentionally not a cancellation signal. The
 * surviving lease proves the descriptor and terminal arbiter remain alive
 * after the original stack frame returns.
 */
TEST(ComputeRunLease,
     RetainsControlAfterObserverDestructionWithoutImplicitCancellation) {
  std::optional<ComputeRunLease> retained_lease;
  std::uint64_t retained_run_id = 0;
  {
    ComputeRun run(make_test_submission("lease-retained", 12, 9));
    retained_run_id = run.descriptor().id().value();
    retained_lease.emplace(run.acquire_lease());
    EXPECT_FALSE(run.is_quiescent());
  }

  ASSERT_TRUE(retained_lease.has_value());
  EXPECT_EQ(retained_lease->descriptor().id().value(), retained_run_id);
  EXPECT_EQ(retained_lease->descriptor().graph_identity(), "lease-retained");
  EXPECT_FALSE(retained_lease->terminal_outcome().has_value());
  retained_lease.reset();
}

/**
 * @brief Verifies terminal publication does not imply physical quiescence.
 *
 * @note The failure is visible through both observer and active lease before
 * the lease releases; quiescence changes only when active lease count reaches
 * zero.
 */
TEST(ComputeRunLease, TerminalStateRemainsNonquiescentUntilLeaseRelease) {
  ComputeRun run(make_test_submission("terminal-nonquiescent", 13, 10));
  {
    ComputeRunLease lease = run.acquire_lease();
    const std::exception_ptr failure =
        std::make_exception_ptr(std::runtime_error("leased failure"));
    ASSERT_TRUE(run.publish_failed(failure));
    EXPECT_TRUE(run.is_terminal());
    EXPECT_FALSE(run.is_quiescent());
    EXPECT_THROW((void)run.acquire_lease(), std::logic_error);
    const auto leased_outcome = lease.terminal_outcome();
    ASSERT_TRUE(leased_outcome.has_value());
    EXPECT_EQ(leased_outcome->kind, ComputeRunTerminalKind::Failed);
    EXPECT_TRUE(leased_outcome->failure == failure);
  }
  EXPECT_TRUE(run.is_quiescent());
  ASSERT_TRUE(run.terminal_outcome().has_value());
  EXPECT_EQ(run.terminal_outcome()->kind, ComputeRunTerminalKind::Failed);
}

/**
 * @brief Verifies lease copy and move operations preserve exact accounting.
 *
 * @note Copies add active ownership, moves transfer it, and replacing a lease
 * releases the previous Run without making the retained Run quiescent.
 */
TEST(ComputeRunLease, CopyMoveAndAssignmentPreserveQuiescenceAccounting) {
  ComputeRun first(make_test_submission("lease-copy-first", 14, 11));
  ComputeRun second(make_test_submission("lease-copy-second", 15, 12));
  {
    ComputeRunLease first_lease = first.acquire_lease();
    ComputeRunLease first_copy = first_lease;
    ComputeRunLease moved_copy = std::move(first_copy);
    ComputeRunLease second_lease = second.acquire_lease();

    EXPECT_FALSE(first.is_quiescent());
    EXPECT_FALSE(second.is_quiescent());
    second_lease = moved_copy;
    EXPECT_TRUE(second.is_quiescent());
    EXPECT_EQ(second_lease.descriptor().id(), first.descriptor().id());

    first_copy = std::move(second_lease);
    EXPECT_EQ(first_copy.descriptor().id(), first.descriptor().id());
    EXPECT_FALSE(first.is_quiescent());
  }
  EXPECT_TRUE(first.is_quiescent());
  EXPECT_TRUE(second.is_quiescent());
}

/**
 * @brief Verifies identical local ids are isolated by Run id and lease route.
 *
 * @note A mismatched or unregistered identity cannot publish failure. The
 * matching Run retains the exact original exception pointer.
 */
TEST(ComputeRunTaskIdentity,
     RepeatedLocalIdsAndFailurePublicationRemainRunScoped) {
  GraphModel first_graph("cache/compute-run-identity-first");
  first_graph.add_node(make_plan_node(21));
  first_graph.validate_topology();
  GraphModel second_graph("cache/compute-run-identity-second");
  second_graph.add_node(make_plan_node(22));
  second_graph.validate_topology();
  GraphTraversalService traversal;
  ComputeRun first(make_test_submission("identity-first",
                                        first_graph.revision().value(), 21));
  ComputeRun second(make_test_submission("identity-second",
                                         second_graph.revision().value(), 22));
  first.advance_to(ComputeRunPhase::Admitted);
  second.advance_to(ComputeRunPhase::Admitted);
  first.emplace_submission_plan(first_graph, traversal, 21,
                                std::vector<Device>{Device::CPU});
  second.emplace_submission_plan(second_graph, traversal, 22,
                                 std::vector<Device>{Device::CPU});
  ComputeRunLease first_lease = first.acquire_lease();
  ComputeRunLease second_lease = second.acquire_lease();
  const ComputeRunTaskIdentity first_zero = first_lease.task_identity(0);
  const ComputeRunTaskIdentity second_zero = second_lease.task_identity(0);

  EXPECT_NE(first_zero, second_zero);
  EXPECT_TRUE(first_lease.accepts_task_identity(first_zero));
  EXPECT_FALSE(first_lease.accepts_task_identity(second_zero));
  EXPECT_FALSE(
      first_lease.accepts_task_identity(first_lease.task_identity(999)));

  const std::exception_ptr failure =
      std::make_exception_ptr(std::runtime_error("second run failure"));
  EXPECT_FALSE(first_lease.publish_task_failure(second_zero, failure));
  EXPECT_FALSE(first_lease.publish_task_failure(first_lease.task_identity(999),
                                                failure));
  EXPECT_FALSE(first.terminal_outcome().has_value());
  EXPECT_FALSE(second.terminal_outcome().has_value());

  EXPECT_TRUE(second_lease.publish_task_failure(second_zero, failure));
  EXPECT_FALSE(first.terminal_outcome().has_value());
  const auto second_outcome = second.terminal_outcome();
  ASSERT_TRUE(second_outcome.has_value());
  EXPECT_EQ(second_outcome->kind, ComputeRunTerminalKind::Failed);
  EXPECT_TRUE(second_outcome->failure == failure);
}

/**
 * @brief Verifies one ready value owns immutable metadata and matching lease.
 *
 * @note A local id from another Run is legal only with that Run id; pairing
 * another Run's composite identity with the first lease must fail before queue
 * admission.
 */
TEST(ReadyTaskSubmission,
     CapturesImmutableMetadataAndRejectsMismatchedRunIdentity) {
  ComputeRun first(make_test_submission("ready-first", 41, 51));
  ComputeRun second(make_test_submission("ready-second", 42, 52));
  ComputeRunLease first_lease = first.acquire_lease();
  ComputeRunLease second_lease = second.acquire_lease();
  std::atomic_int entered{0};

  ReadyTaskSubmission submission =
      make_counted_ready_submission(first_lease, 7, 71, entered);
  EXPECT_EQ(submission.metadata().run_id(), first.descriptor().id());
  EXPECT_EQ(submission.metadata().graph_identity(), "ready-first");
  EXPECT_EQ(submission.metadata().graph_instance_id().value(), 41U);
  EXPECT_EQ(submission.metadata().revision().value(), 41U);
  EXPECT_EQ(submission.metadata().target_node_id(), 51);
  EXPECT_EQ(submission.metadata().intent(), ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(submission.metadata().quality(), ComputeRunQuality::Full);
  EXPECT_EQ(submission.metadata().qos().weight, 3U);
  EXPECT_EQ(submission.metadata().trace_node_id(), 71);
  EXPECT_TRUE(submission.metadata().is_initial_ready());
  EXPECT_EQ(submission.identity().local_task_id().value(), 7U);

  const ComputeRunTaskIdentity second_identity = second_lease.task_identity(7);
  EXPECT_THROW((void)ReadyTaskSubmission(
                   first_lease, second_identity, 71, true,
                   [](ComputeRunLease&, const ComputeRunTaskIdentity&,
                      ExecutionTaskRuntime&) {}),
               std::invalid_argument);
}

/**
 * @brief Verifies one isolated service executes one Run and can be reused.
 *
 * @note Equal local task values across sequential Runs remain isolated by
 * composite Run id. Balanced host context proves the proxy is cleared only
 * after each accepted callback settles.
 */
TEST(ExecutionService, ExecutesSequentialRunsWithRunScopedIdentity) {
  ExecutionService service(1);
  ExecutionServiceHost host;
  ComputeRun first(make_test_submission("service-first", 51, 61));
  ComputeRun second(make_test_submission("service-second", 52, 62));
  std::atomic_int entered{0};

  std::vector<ReadyTaskSubmission> first_ready;
  first_ready.push_back(
      make_counted_ready_submission(first.acquire_lease(), 0, 61, entered));
  EXPECT_NO_THROW(service.execute_run(host, "cpu", std::move(first_ready), 1));

  std::vector<ReadyTaskSubmission> second_ready;
  second_ready.push_back(
      make_counted_ready_submission(second.acquire_lease(), 0, 62, entered));
  EXPECT_NO_THROW(service.execute_run(host, "cpu", std::move(second_ready), 1));

  EXPECT_EQ(entered.load(std::memory_order_relaxed), 2);
  EXPECT_EQ(host.context_entries(), 2);
  EXPECT_EQ(host.context_exits(), 2);
  EXPECT_FALSE(host.trace_recording_failed());
  EXPECT_EQ(host.trace_events().size(), 2U);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies every non-CPU Run dimension is admitted all-or-none.
 *
 * @return Nothing.
 * @throws std::bad_alloc or std::system_error from isolated service setup.
 * @note Each case exceeds exactly retained bytes, scratch bytes, ready
 * entries, or ready bytes. A smaller Run then succeeds on the same service,
 * proving rejection did not retain a partial vector.
 */
TEST(ExecutionService, RejectsEveryNonCpuRunDimensionAndRecoversExactly) {
  ASSERT_NO_FATAL_FAILURE(expect_resource_dimension_rejection_and_recovery(
      "retained-memory", RejectedResourceDimension::RetainedMemory,
      ReadyTaskResourceDemand{5U, 1U, 1U}, 1));
  ASSERT_NO_FATAL_FAILURE(expect_resource_dimension_rejection_and_recovery(
      "scratch", RejectedResourceDimension::Scratch,
      ReadyTaskResourceDemand{1U, 5U, 1U}, 1));
  ASSERT_NO_FATAL_FAILURE(expect_resource_dimension_rejection_and_recovery(
      "ready-entries", RejectedResourceDimension::ReadyEntries,
      ReadyTaskResourceDemand{1U, 1U, 1U}, 2));
  ASSERT_NO_FATAL_FAILURE(expect_resource_dimension_rejection_and_recovery(
      "ready-bytes", RejectedResourceDimension::ReadyBytes,
      ReadyTaskResourceDemand{1U, 1U, 5U}, 1));
}

/**
 * @brief Verifies checked whole-Run multiplication rejects overflow.
 *
 * @return Nothing.
 * @throws std::bad_alloc or std::system_error from isolated service setup.
 * @note No root capacity is committed when one resource product cannot be
 * represented, and the same service remains usable afterward.
 */
TEST(ExecutionService, RejectsRunResourceOverflowWithoutPartialReservation) {
  constexpr std::uint64_t kMaximum = std::numeric_limits<std::uint64_t>::max();
  const ExecutionResourceLimits limits{1U,       kMaximum, kMaximum,
                                       kMaximum, kMaximum, ResourceVector{}};
  constexpr ReadyTaskResourceDemand kOverflowDemand{1U, 1U, kMaximum};
  ExecutionService service(1U, limits);
  ExecutionServiceHost host;
  std::atomic_int entered{0};
  ComputeRun overflow_run(make_test_submission("resource-overflow", 1U, 1));
  std::vector<ReadyTaskSubmission> overflow_ready;
  overflow_ready.push_back(make_counted_ready_submission(
      overflow_run.acquire_lease(), 0U, 1, entered,
      ExecutionTaskPriority::Normal, kOverflowDemand));
  overflow_ready.push_back(make_counted_ready_submission(
      overflow_run.acquire_lease(), 1U, 2, entered,
      ExecutionTaskPriority::Normal, kOverflowDemand));
  try {
    service.execute_run(host, "cpu", std::move(overflow_ready), 2,
                        CpuRunResourceDemand{0U, kOverflowDemand});
    FAIL() << "Expected checked Run resource overflow.";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::ComputeError);
  }
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});

  constexpr ReadyTaskResourceDemand kRecoveryDemand{1U, 1U, 1U};
  ComputeRun recovery_run(
      make_test_submission("resource-overflow-recovery", 2U, 2));
  std::vector<ReadyTaskSubmission> recovery_ready;
  recovery_ready.push_back(make_counted_ready_submission(
      recovery_run.acquire_lease(), 0U, 2, entered,
      ExecutionTaskPriority::Normal, kRecoveryDemand));
  EXPECT_NO_THROW(
      service.execute_run(host, "cpu", std::move(recovery_ready), 1,
                          CpuRunResourceDemand{0U, kRecoveryDemand}));
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies shared, per-task, and mandatory service estimates differ.
 *
 * @return Nothing.
 * @throws Standard allocation exceptions from test service and Run setup.
 * @note A once-per-Run shared increment changes retained capacity exactly
 * once. Per-task retained/scratch scale by worker concurrency, ready bytes
 * scale by logical task count, and zero adapter demand still carries a
 * positive service-owned envelope.
 */
TEST(ExecutionService, SeparatesSharedConcurrentAndLogicalTaskResources) {
  constexpr ReadyTaskResourceDemand kTaskDemand{7U, 11U, 13U};
  constexpr std::uint64_t kSharedBytes = 17U;
  ExecutionService service(2U);
  ComputeRun run(make_test_submission("resource-scaling", 1U, 1));
  std::atomic_int entered{0};
  ReadyTaskSubmission representative =
      make_counted_ready_submission(run.acquire_lease(), 0U, 1, entered,
                                    ExecutionTaskPriority::Normal, kTaskDemand);

  const ResourceVector zero_adapter = service.estimate_cpu_run_resources(
      representative, 5, CpuRunResourceDemand{});
  const ResourceVector per_task = service.estimate_cpu_run_resources(
      representative, 5, CpuRunResourceDemand{0U, kTaskDemand});
  const ResourceVector with_shared = service.estimate_cpu_run_resources(
      representative, 5, CpuRunResourceDemand{kSharedBytes, kTaskDemand});

  EXPECT_GT(zero_adapter.retained_memory_bytes, 0U);
  EXPECT_GT(zero_adapter.ready_bytes, 0U);
  EXPECT_EQ(zero_adapter.cpu_slots, 2U);
  EXPECT_EQ(zero_adapter.ready_entries, 5U);
  EXPECT_EQ(per_task.retained_memory_bytes - zero_adapter.retained_memory_bytes,
            2U * kTaskDemand.retained_memory_bytes);
  EXPECT_EQ(per_task.scratch_bytes - zero_adapter.scratch_bytes,
            2U * kTaskDemand.scratch_bytes);
  EXPECT_EQ(per_task.ready_bytes - zero_adapter.ready_bytes,
            5U * kTaskDemand.ready_bytes);
  EXPECT_EQ(with_shared.retained_memory_bytes - per_task.retained_memory_bytes,
            kSharedBytes);
  EXPECT_EQ(with_shared.scratch_bytes, per_task.scratch_bytes);
  EXPECT_EQ(with_shared.ready_entries, per_task.ready_entries);
  EXPECT_EQ(with_shared.ready_bytes, per_task.ready_bytes);
  EXPECT_THROW((void)owned_callback_resource_demand(
                   std::numeric_limits<std::uint64_t>::max()),
               GraphError);
}

/**
 * @brief Proves Run maximum parallelism bounds exact root admission resources.
 *
 * @return Nothing.
 * @throws Standard Run, service, submission, or worker exceptions unchanged;
 * GoogleTest reports unexpected failures.
 * @note The capped Run executes four logical tasks on a four-worker service
 * whose retained/scratch limits admit only one callback envelope; CPU
 * capacity remains at the service's required worker-pool floor. The otherwise
 * identical uncapped Run is rejected before callback entry, proving both the
 * diagnostic estimate and production admission use the Run QoS cap while
 * ready grants still cover every logical task.
 */
TEST(ExecutionService, MaximumParallelismCapsRunAdmissionResources) {
  constexpr unsigned int kWorkerCount = 4U;
  constexpr int kLogicalTaskCount = 4;
  constexpr ReadyTaskResourceDemand kTaskDemand{17U, 19U, 23U};
  constexpr std::uint64_t kSharedBytes = 29U;
  const CpuRunResourceDemand run_demand{kSharedBytes, kTaskDemand};

  ComputeRunSubmission capped_submission =
      make_test_submission("maximum-parallelism-admission", 1U, 1);
  capped_submission.qos.maximum_parallelism = 1U;
  ComputeRun capped_run(std::move(capped_submission));
  std::atomic_int entered{0};
  ReadyTaskSubmission capped_representative =
      make_counted_ready_submission(capped_run.acquire_lease(), 0U, 1, entered,
                                    ExecutionTaskPriority::Normal, kTaskDemand);

  ComputeRunSubmission uncapped_submission =
      make_test_submission("maximum-parallelism-admission", 2U, 1);
  uncapped_submission.qos.maximum_parallelism.reset();
  ComputeRun uncapped_run(std::move(uncapped_submission));
  ReadyTaskSubmission uncapped_representative = make_counted_ready_submission(
      uncapped_run.acquire_lease(), 0U, 1, entered,
      ExecutionTaskPriority::Normal, kTaskDemand);

  ExecutionService estimate_service(kWorkerCount);
  const ResourceVector capped_resources =
      estimate_service.estimate_cpu_run_resources(
          capped_representative, kLogicalTaskCount, run_demand);
  const ResourceVector uncapped_resources =
      estimate_service.estimate_cpu_run_resources(
          uncapped_representative, kLogicalTaskCount, run_demand);

  EXPECT_EQ(capped_resources.cpu_slots, 1U);
  EXPECT_EQ(uncapped_resources.cpu_slots, kWorkerCount);
  EXPECT_EQ(capped_resources.scratch_bytes, kTaskDemand.scratch_bytes);
  EXPECT_EQ(uncapped_resources.scratch_bytes,
            kWorkerCount * kTaskDemand.scratch_bytes);
  EXPECT_LT(capped_resources.retained_memory_bytes,
            uncapped_resources.retained_memory_bytes);
  EXPECT_EQ(capped_resources.ready_entries,
            static_cast<std::uint64_t>(kLogicalTaskCount));
  EXPECT_EQ(capped_resources.ready_entries, uncapped_resources.ready_entries);
  EXPECT_EQ(capped_resources.ready_bytes, uncapped_resources.ready_bytes);

  ExecutionResourceLimits capped_limits = execution_limits(capped_resources);
  capped_limits.cpu_slots = kWorkerCount;
  ExecutionService service(kWorkerCount, capped_limits);
  ExecutionServiceHost host;
  std::vector<ReadyTaskSubmission> capped_ready;
  capped_ready.push_back(std::move(capped_representative));
  for (int task_index = 1; task_index < kLogicalTaskCount; ++task_index) {
    capped_ready.push_back(make_counted_ready_submission(
        capped_run.acquire_lease(), static_cast<std::uint64_t>(task_index),
        task_index + 1, entered, ExecutionTaskPriority::Normal, kTaskDemand));
  }
  EXPECT_NO_THROW(service.execute_run(host, "cpu", std::move(capped_ready),
                                      kLogicalTaskCount, run_demand));
  EXPECT_EQ(entered.load(std::memory_order_relaxed), kLogicalTaskCount);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});

  std::vector<ReadyTaskSubmission> uncapped_ready;
  uncapped_ready.push_back(std::move(uncapped_representative));
  for (int task_index = 1; task_index < kLogicalTaskCount; ++task_index) {
    uncapped_ready.push_back(make_counted_ready_submission(
        uncapped_run.acquire_lease(), static_cast<std::uint64_t>(task_index),
        task_index + 1, entered, ExecutionTaskPriority::Normal, kTaskDemand));
  }
  EXPECT_THROW(service.execute_run(host, "cpu", std::move(uncapped_ready),
                                   kLogicalTaskCount, run_demand),
               GraphError);
  EXPECT_EQ(entered.load(std::memory_order_relaxed), kLogicalTaskCount);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies metadata string capacity uses both admission multipliers.
 *
 * @return Nothing.
 * @throws Standard Run, metadata, service, or estimator exceptions unchanged.
 * @note The chosen short and long strings have different actual copied
 * capacities on supported standard libraries, and that capacity delta differs
 * from their size delta. Retained bytes scale it by two fixed workers plus the
 * two pre-copied Run/Graph policy keys, while ready bytes scale it by five
 * logical tasks.
 */
TEST(ExecutionService,
     ChargesActualMetadataStringCapacityForReadyAndExecutionEnvelopes) {
  constexpr int kLogicalTaskCount = 5;
  constexpr unsigned int kWorkerCount = 2U;
  const std::string short_identity(1U, 's');
  const std::string long_identity(24U, 'l');
  ExecutionService service(kWorkerCount);
  std::atomic_int entered{0};
  ComputeRun short_run(make_test_submission(short_identity, 1U, 1));
  ComputeRun long_run(make_test_submission(long_identity, 1U, 1));
  ReadyTaskSubmission short_representative =
      make_counted_ready_submission(short_run.acquire_lease(), 0U, 1, entered);
  ReadyTaskSubmission long_representative =
      make_counted_ready_submission(long_run.acquire_lease(), 0U, 1, entered);

  const std::string& copied_short =
      short_representative.metadata().graph_identity();
  const std::string& copied_long =
      long_representative.metadata().graph_identity();
  ASSERT_GT(copied_long.capacity(), copied_short.capacity());
  const std::uint64_t capacity_delta = static_cast<std::uint64_t>(
      copied_long.capacity() - copied_short.capacity());
  const std::uint64_t size_delta =
      static_cast<std::uint64_t>(copied_long.size() - copied_short.size());
  ASSERT_NE(capacity_delta, size_delta);

  const ResourceVector short_resources = service.estimate_cpu_run_resources(
      short_representative, kLogicalTaskCount, CpuRunResourceDemand{});
  const ResourceVector long_resources = service.estimate_cpu_run_resources(
      long_representative, kLogicalTaskCount, CpuRunResourceDemand{});
  EXPECT_EQ(long_resources.retained_memory_bytes -
                short_resources.retained_memory_bytes,
            (kWorkerCount + 2U) * capacity_delta);
  EXPECT_EQ(long_resources.ready_bytes - short_resources.ready_bytes,
            static_cast<std::uint64_t>(kLogicalTaskCount) * capacity_delta);
  EXPECT_EQ(long_resources.cpu_slots, short_resources.cpu_slots);
  EXPECT_EQ(long_resources.ready_entries, short_resources.ready_entries);
}

/**
 * @brief Verifies the storage-retirement helper performs an empty-vector swap.
 *
 * @return Nothing.
 * @throws Standard Run, submission, vector, or callback allocation exceptions
 * from fixture setup.
 * @note This focused helper check does not replace the production-path test
 * below, which proves execute_run invokes the boundary at the required
 * point.
 */
TEST(ExecutionService, ReleaseHelperRetiresInitialSubmissionStorage) {
  ComputeRun run(make_test_submission("released-initial-staging", 1U, 1));
  std::atomic_int entered{0};
  std::vector<ReadyTaskSubmission> submissions;
  submissions.reserve(8U);
  submissions.push_back(
      make_counted_ready_submission(run.acquire_lease(), 0U, 1, entered));
  ASSERT_FALSE(submissions.empty());
  ASSERT_GE(submissions.capacity(), 8U);

  testing::ExecutionServiceTestAccess::release_initial_submission_storage(
      submissions);

  EXPECT_TRUE(submissions.empty());
  EXPECT_EQ(submissions.capacity(), 0U);
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 0);
}

/**
 * @brief Proves a real CPU Run retires initial backing before publication.
 *
 * @return Nothing.
 * @throws Standard Run, service, future, or callback setup exceptions.
 * @note The production observer runs after QueueEntry/grant staging and the
 * empty-vector swap. The sole callback then blocks, keeping execute_run in
 * settlement while the test verifies the original nonzero backing is gone.
 * Deleting or moving the production release after this observation leaves a
 * nonzero released capacity and fails this regression.
 */
TEST(ExecutionService,
     ProductionRunRetiresInitialStorageBeforeBlockedSettlement) {
  ExecutionService service(1U);
  ExecutionServiceHost host;
  ComputeRun run(
      make_test_submission("production-released-initial-staging", 1U, 1));
  InitialSubmissionStorageProbe observation;
  testing::ExecutionServiceTestAccess::set_initial_submission_storage_observer(
      service, &InitialSubmissionStorageProbe::observe, &observation);

  std::promise<void> callback_entered;
  std::future<void> callback_entered_future = callback_entered.get_future();
  std::promise<void> release_callback;
  const std::shared_future<void> release_gate =
      release_callback.get_future().share();
  std::atomic_bool observer_visible_before_callback{false};
  ComputeRunLease lease = run.acquire_lease();
  const ComputeRunTaskIdentity identity = lease.task_identity(0U);
  auto blocking_callback = [&callback_entered, release_gate,
                            &observer_visible_before_callback, &observation](
                               ComputeRunLease& retained_lease,
                               const ComputeRunTaskIdentity& retained_identity,
                               ExecutionTaskRuntime& runtime) {
    observer_visible_before_callback.store(
        retained_lease.descriptor().id() == retained_identity.run_id() &&
            observation.observation_count.load(std::memory_order_acquire) == 1,
        std::memory_order_release);
    callback_entered.set_value();
    release_gate.wait();
    runtime.dec_tasks_to_complete();
  };
  const ReadyTaskResourceDemand task_demand = owned_callback_resource_demand(
      static_cast<std::uint64_t>(sizeof(blocking_callback)));
  std::vector<ReadyTaskSubmission> submissions;
  submissions.reserve(8U);
  submissions.emplace_back(std::move(lease), identity, 1, true,
                           std::move(blocking_callback),
                           ExecutionTaskPriority::Normal, task_demand);
  ASSERT_EQ(submissions.size(), 1U);
  ASSERT_GE(submissions.capacity(), 8U);

  auto run_future = std::async(
      std::launch::async,
      [&service, &host, ready = std::move(submissions), task_demand]() mutable {
        service.execute_run(host, "cpu", std::move(ready), 1,
                            CpuRunResourceDemand{0U, task_demand});
      });
  ScopedPromiseRelease release_guard(release_callback);
  ASSERT_EQ(callback_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_TRUE(observer_visible_before_callback.load(std::memory_order_acquire));
  ASSERT_EQ(observation.observation_count.load(std::memory_order_acquire), 1);
  EXPECT_EQ(observation.staged_size, 1U);
  EXPECT_GE(observation.staged_capacity, 8U);
  EXPECT_EQ(observation.released_size, 0U);
  EXPECT_EQ(observation.released_capacity, 0U);
  EXPECT_EQ(service.resource_snapshot().reserved,
            observation.admitted_resources);
  EXPECT_EQ(run_future.wait_for(std::chrono::seconds(0)),
            std::future_status::timeout);

  EXPECT_TRUE(release_guard.release());
  ASSERT_EQ(run_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(run_future.get());
  testing::ExecutionServiceTestAccess::
      clear_initial_submission_storage_observer(service);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Proves the product context-transfer helper clears a preserved source.
 *
 * @return Nothing.
 * @throws Standard plan, Run, callable, or context exceptions unchanged.
 * @note The adversarial holder's move leaves both source and destination
 * targets live. The factory builds a real `DirtyReadyTaskContext`; after the
 * same helper used by production returns, only that context target may remain.
 * A second factory throws before returning a destination and proves temporary
 * ownership unwinds while the outer holder remains available for normal RAII.
 * Deleting the helper's explicit null assignment makes this test fail on every
 * standard-library moved-from representation.
 */
TEST(DirtyExecutionCommon,
     ContextTransferExplicitlyClearsMovePreservingOuterCallable) {
  constexpr int kNodeId = 130;
  const std::vector<int> task_ids{0};

  ComputePlan compute_plan;
  compute_plan.intent = ComputeIntent::GlobalHighPrecision;
  compute_plan.target_node_id = kNodeId;
  compute_plan.parallel = true;
  compute_plan.execution_order = {kNodeId};
  compute_plan.planned_nodes = {kNodeId};
  compute_plan.task_graph.tasks.resize(1U);
  compute_plan.task_graph.tasks[0].task_id = 0;
  compute_plan.task_graph.tasks[0].node_id = kNodeId;
  compute_plan.task_graph.initial_task_ids = task_ids;

  ComputeRun run(
      make_test_submission("dirty-callable-transfer-boundary", 1U, kNodeId));
  ASSERT_TRUE(run.advance_to(ComputeRunPhase::Admitted));
  ASSERT_TRUE(run.advance_to(ComputeRunPhase::Queued));
  ASSERT_TRUE(run.advance_to(ComputeRunPhase::Running));

  const std::uint64_t callable_bytes = owned_callable_retained_memory_bytes(
      static_cast<std::uint64_t>(sizeof(LargeDirtyPhaseTask)));
  const std::vector<Device> task_devices(compute_plan.task_graph.tasks.size(),
                                         Device::CPU);
  LargeDirtyPhaseTaskProbe success_probe;
  MovePreservingDirtyCallableHolder outer_holder{
      LargeDirtyPhaseTask(success_probe)};
  ASSERT_TRUE(outer_holder.has_target());
  ASSERT_EQ(success_probe.live_targets.load(std::memory_order_relaxed), 1);

  bool factory_observed_preserved_source = false;
  int factory_live_targets = -1;
  auto context = make_dirty_context_and_release_outer_callable(
      outer_holder, [&](MovePreservingDirtyCallableHolder transferred_holder) {
        factory_observed_preserved_source =
            outer_holder.has_target() && transferred_holder.has_target();
        factory_live_targets =
            success_probe.live_targets.load(std::memory_order_relaxed);
        return std::make_shared<DirtyReadyTaskContext>(
            compute_plan, nullptr, task_ids, task_devices,
            std::move(transferred_holder).release_for_context(), callable_bytes,
            run.acquire_lease(), true, ExecutionTaskPriority::Normal);
      });

  EXPECT_TRUE(factory_observed_preserved_source);
  EXPECT_EQ(factory_live_targets, 2);
  EXPECT_FALSE(outer_holder.has_target());
  ASSERT_NE(context, nullptr);
  EXPECT_EQ(success_probe.live_targets.load(std::memory_order_relaxed), 1);
  context.reset();
  EXPECT_EQ(success_probe.live_targets.load(std::memory_order_relaxed), 0);

  LargeDirtyPhaseTaskProbe failure_probe;
  {
    MovePreservingDirtyCallableHolder failure_outer_holder{
        LargeDirtyPhaseTask(failure_probe)};
    bool throwing_factory_observed_preserved_source = false;
    int throwing_factory_live_targets = -1;
    EXPECT_THROW(
        (void)make_dirty_context_and_release_outer_callable(
            failure_outer_holder,
            [&](MovePreservingDirtyCallableHolder transferred_holder)
                -> std::shared_ptr<DirtyReadyTaskContext> {
              throwing_factory_observed_preserved_source =
                  failure_outer_holder.has_target() &&
                  transferred_holder.has_target();
              throwing_factory_live_targets =
                  failure_probe.live_targets.load(std::memory_order_relaxed);
              throw std::runtime_error(
                  "Synthetic dirty context construction failure.");
            }),
        std::runtime_error);
    EXPECT_TRUE(throwing_factory_observed_preserved_source);
    EXPECT_EQ(throwing_factory_live_targets, 2);
    EXPECT_TRUE(failure_outer_holder.has_target());
    EXPECT_EQ(failure_probe.live_targets.load(std::memory_order_relaxed), 1);
  }
  EXPECT_EQ(failure_probe.live_targets.load(std::memory_order_relaxed), 0);
}

/**
 * @brief Proves dirty source charges its live outer callable copy exactly once.
 *
 * @return Nothing.
 * @throws Standard plan, Run, context, estimator, or service exceptions
 * unchanged.
 * @note The expected source vector is calculated without invoking
 * `run_dirty_source_first()`: one context-owned large target is included by
 * `DirtyReadyTaskContext`, and one independently added target covers the
 * source-only outer `std::function`. A default-limit production call first
 * captures and matches that complete vector. Reducing only retained capacity
 * by the extra target must reject before callback entry with a zero ledger;
 * the exact vector must execute and settle. A downstream-only production call
 * then succeeds at the independently calculated single-target vector. A
 * combined source-to-downstream call directly observes two targets during
 * source demand, one outer target between phases, one context target before
 * downstream admission, and zero targets after settlement. This preserves
 * product-path integration coverage, while the source-preserving holder test
 * on the same production helper supplies the implementation-independent reset
 * proof. Removing the production source addition makes the reduced-cap
 * endpoint execute and fails this test.
 */
TEST(ExecutionServiceProductResources,
     DirtySourceChargesOuterCallableCopyWithoutDownstreamDoubleCount) {
  constexpr int kNodeId = 131;
  constexpr std::uint64_t kAdditionalSharedBytes = 173U;
  const std::string graph_identity = "dirty-callable-cap";
  const std::vector<int> phase_task_ids{0};

  ComputePlan compute_plan;
  compute_plan.intent = ComputeIntent::GlobalHighPrecision;
  compute_plan.target_node_id = kNodeId;
  compute_plan.parallel = true;
  compute_plan.execution_order = {kNodeId};
  compute_plan.planned_nodes = {kNodeId};
  compute_plan.task_graph.tasks.resize(1U);
  compute_plan.task_graph.tasks[0].task_id = 0;
  compute_plan.task_graph.tasks[0].node_id = kNodeId;
  compute_plan.task_graph.initial_task_ids = phase_task_ids;

  const std::uint64_t extra_callable_bytes =
      owned_callable_retained_memory_bytes(
          static_cast<std::uint64_t>(sizeof(LargeDirtyPhaseTask)));
  ASSERT_GT(extra_callable_bytes, 0U);
  const ResourceVector source_required =
      independently_estimate_large_dirty_phase_resources(
          compute_plan, phase_task_ids, graph_identity, kAdditionalSharedBytes,
          true);
  const ResourceVector downstream_required =
      independently_estimate_large_dirty_phase_resources(
          compute_plan, phase_task_ids, graph_identity, kAdditionalSharedBytes,
          false);
  ASSERT_GE(source_required.retained_memory_bytes, extra_callable_bytes);
  EXPECT_EQ(source_required.retained_memory_bytes -
                downstream_required.retained_memory_bytes,
            extra_callable_bytes);
  EXPECT_EQ(source_required.cpu_slots, downstream_required.cpu_slots);
  EXPECT_EQ(source_required.scratch_bytes, downstream_required.scratch_bytes);
  EXPECT_EQ(source_required.ready_entries, downstream_required.ready_entries);
  EXPECT_EQ(source_required.ready_bytes, downstream_required.ready_bytes);

  const DirtyCallablePhaseRunResult calibration =
      execute_large_dirty_callable_phase_case(
          compute_plan, phase_task_ids, graph_identity, kAdditionalSharedBytes,
          true, ExecutionService::default_resource_limits());
  EXPECT_FALSE(calibration.failure_code.has_value());
  ASSERT_TRUE(calibration.admitted_resources.has_value());
  EXPECT_EQ(*calibration.admitted_resources, source_required);
  EXPECT_EQ(calibration.observation_count, 1);
  EXPECT_EQ(calibration.callback_entries, 1);
  EXPECT_EQ(calibration.reserved_after, ResourceVector{});

  ResourceVector omitted_outer_callable = source_required;
  omitted_outer_callable.retained_memory_bytes -= extra_callable_bytes;
  const DirtyCallablePhaseRunResult rejected =
      execute_large_dirty_callable_phase_case(
          compute_plan, phase_task_ids, graph_identity, kAdditionalSharedBytes,
          true, execution_limits(omitted_outer_callable));
  ASSERT_TRUE(rejected.failure_code.has_value());
  EXPECT_EQ(*rejected.failure_code, GraphErrc::ComputeError);
  EXPECT_FALSE(rejected.admitted_resources.has_value());
  EXPECT_EQ(rejected.observation_count, 0);
  EXPECT_EQ(rejected.callback_entries, 0);
  EXPECT_EQ(rejected.reserved_after, ResourceVector{});

  const DirtyCallablePhaseRunResult exact_source =
      execute_large_dirty_callable_phase_case(
          compute_plan, phase_task_ids, graph_identity, kAdditionalSharedBytes,
          true, execution_limits(source_required));
  EXPECT_FALSE(exact_source.failure_code.has_value());
  ASSERT_TRUE(exact_source.admitted_resources.has_value());
  EXPECT_EQ(*exact_source.admitted_resources, source_required);
  EXPECT_EQ(exact_source.observation_count, 1);
  EXPECT_EQ(exact_source.callback_entries, 1);
  EXPECT_EQ(exact_source.reserved_after, ResourceVector{});

  const DirtyCallablePhaseRunResult exact_downstream =
      execute_large_dirty_callable_phase_case(
          compute_plan, phase_task_ids, graph_identity, kAdditionalSharedBytes,
          false, execution_limits(downstream_required));
  EXPECT_FALSE(exact_downstream.failure_code.has_value());
  ASSERT_TRUE(exact_downstream.admitted_resources.has_value());
  EXPECT_EQ(*exact_downstream.admitted_resources, downstream_required);
  EXPECT_EQ(exact_downstream.observation_count, 1);
  EXPECT_EQ(exact_downstream.callback_entries, 1);
  EXPECT_EQ(exact_downstream.reserved_after, ResourceVector{});

  ComputePlan combined_plan = compute_plan;
  combined_plan.execution_order.push_back(kNodeId + 1);
  combined_plan.planned_nodes.push_back(kNodeId + 1);
  combined_plan.task_graph.tasks.resize(2U);
  combined_plan.task_graph.tasks[1].task_id = 1;
  combined_plan.task_graph.tasks[1].node_id = kNodeId + 1;
  combined_plan.task_graph.initial_task_ids = {0, 1};
  const std::vector<int> source_task_ids{0};
  const std::vector<int> downstream_task_ids{1};
  const std::vector<Device> task_devices(combined_plan.task_graph.tasks.size(),
                                         Device::CPU);

  ExecutionService combined_service(1U);
  ExecutionServiceHost combined_host;
  ComputeRun combined_run(make_test_submission("dirty-callable-lifetime", 1U,
                                               combined_plan.target_node_id));
  ASSERT_TRUE(combined_run.advance_to(ComputeRunPhase::Admitted));
  ASSERT_TRUE(combined_run.advance_to(ComputeRunPhase::Queued));
  ASSERT_TRUE(combined_run.advance_to(ComputeRunPhase::Running));

  LargeDirtyPhaseTaskProbe combined_probe;
  std::array<int, 2U> phase_live_targets{-1, -1};
  std::size_t phase_observation_index = 0U;
  int between_phase_live_targets = -1;
  DirtySourceFirstRunRequest combined_request;
  combined_request.intent = ComputeIntent::GlobalHighPrecision;
  combined_request.execution_service = &combined_service;
  combined_request.host = &combined_host;
  combined_request.run = &combined_run;
  combined_request.compute_plan = &combined_plan;
  combined_request.task_devices = &task_devices;
  combined_request.phase_shared_retained_memory_bytes =
      [&combined_probe, &phase_live_targets,
       &phase_observation_index](const std::vector<int>&) {
        if (phase_observation_index >= phase_live_targets.size()) {
          throw std::logic_error(
              "Dirty callable lifetime observed too many phase demands.");
        }
        phase_live_targets.at(phase_observation_index++) =
            combined_probe.live_targets.load(std::memory_order_relaxed);
        return 0U;
      };
  combined_request.source_task_ids = &source_task_ids;
  combined_request.downstream_task_ids = &downstream_task_ids;
  combined_request.before_downstream = [&combined_probe,
                                        &between_phase_live_targets] {
    between_phase_live_targets =
        combined_probe.live_targets.load(std::memory_order_relaxed);
  };

  EXPECT_NO_THROW(run_dirty_source_first(combined_request,
                                         LargeDirtyPhaseTask(combined_probe)));
  EXPECT_EQ(phase_observation_index, phase_live_targets.size());
  EXPECT_EQ(phase_live_targets.at(0U), 2);
  EXPECT_EQ(between_phase_live_targets, 1);
  EXPECT_EQ(phase_live_targets.at(1U), 1);
  EXPECT_EQ(combined_probe.callback_entries.load(std::memory_order_relaxed), 2);
  EXPECT_EQ(combined_probe.live_targets.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(combined_service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies per-node synchronization demand grows with owned mutexes.
 *
 * @return Nothing.
 * @throws Standard vector/map/mutex allocation exceptions from fixture setup.
 * @throws GraphError if checked retained-memory estimation overflows.
 * @note The visible minimum covers the shared allocation, one unordered-map
 * value and linkage, its unique_ptr-owned mutex, and the synchronization
 * object. The exact bucket count remains implementation-selected.
 */
TEST(ExecutionServiceProductResources,
     DirtyNodeSynchronizationDemandTracksNodePlanGrowth) {
  DirtyNodeSynchronization one_node({1});
  std::vector<int> many_node_ids;
  many_node_ids.reserve(64U);
  for (int node_id = 1; node_id <= 64; ++node_id) {
    many_node_ids.push_back(node_id);
  }
  DirtyNodeSynchronization many_nodes(many_node_ids);

  const std::uint64_t one_node_bytes = one_node.retained_memory_bytes();
  const std::uint64_t many_node_bytes = many_nodes.retained_memory_bytes();
  const std::uint64_t visible_one_node_minimum =
      static_cast<std::uint64_t>(sizeof(DirtyNodeSynchronization)) +
      2U * static_cast<std::uint64_t>(sizeof(void*)) +
      static_cast<std::uint64_t>(sizeof(
          std::unordered_map<int, std::unique_ptr<std::mutex>>::value_type)) +
      2U * static_cast<std::uint64_t>(sizeof(void*)) +
      static_cast<std::uint64_t>(sizeof(std::mutex));

  EXPECT_GE(one_node_bytes, visible_one_node_minimum);
  EXPECT_GT(many_node_bytes, one_node_bytes);
}

/**
 * @brief Exercises full-HP product admission below and above its estimate.
 *
 * @return Nothing.
 * @throws Standard allocation, registry, codec, or graph exceptions from
 * fixture setup.
 * @note The exact service-only envelope is first proved usable, then rejects
 * the real `TaskSubmissionPlan` adapter before operation entry because it
 * omits the Run-owned plan/runner interval. A default-capacity service
 * executes the same production adapter and releases its complete root
 * reservation.
 */
TEST(ExecutionServiceProductResources,
     FullPlanRejectsSmallLimitAndExecutesWithDefaultLimit) {
  ensure_resource_product_operations_registered();
  g_resource_product_operation_calls.store(0, std::memory_order_relaxed);
  const std::string graph_identity = "resource-full-product";
  const ResourceVector service_only =
      estimate_service_only_resources(graph_identity);
  ASSERT_NO_FATAL_FAILURE(
      expect_service_only_envelope_executes(graph_identity, service_only));

  GraphModel graph("cache/resource-full-product");
  graph.add_node(make_resource_product_node(101));
  graph.validate_topology();
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;

  ExecutionService small_service(1U, execution_limits(service_only));
  ExecutionServiceHost small_host;
  ComputeRun small_run(
      make_test_submission(graph_identity, graph.revision().value(), 101));
  ASSERT_TRUE(small_run.advance_to(ComputeRunPhase::Admitted));
  TaskSubmissionPlan& small_plan = small_run.emplace_submission_plan(
      graph, traversal, 101, std::vector<Device>{Device::CPU});
  TimingCollector small_timings;
  std::mutex small_timing_mutex;
  small_plan.emplace_task_runner(NodeTaskRunnerContext{
      graph,
      cache,
      events,
      small_service,
      small_timings,
      small_timing_mutex,
      small_plan.execution_order(),
      small_plan.id_to_idx(),
      small_plan.temp_results(),
      small_plan.resolved_ops(),
      small_plan.compute_plan().task_graph,
      false,
      false,
      true,
      nullptr,
  });
  ASSERT_TRUE(small_run.advance_to(ComputeRunPhase::Queued));
  ASSERT_TRUE(small_run.advance_to(ComputeRunPhase::Running));
  ComputeRunLease small_lease = small_run.acquire_lease();
  const std::uint64_t adapter_shared_bytes =
      small_lease.retained_memory_bytes();
  ASSERT_GT(adapter_shared_bytes, 0U);
  std::atomic_int estimate_entered{0};
  ReadyTaskSubmission adapter_representative =
      make_counted_ready_submission(small_lease, 0U, 101, estimate_entered);
  const ResourceVector with_adapter = small_service.estimate_cpu_run_resources(
      adapter_representative, 1,
      CpuRunResourceDemand{adapter_shared_bytes, {}});
  EXPECT_EQ(
      with_adapter.retained_memory_bytes - service_only.retained_memory_bytes,
      adapter_shared_bytes);
  EXPECT_EQ(with_adapter.cpu_slots, service_only.cpu_slots);
  EXPECT_EQ(with_adapter.scratch_bytes, service_only.scratch_bytes);
  EXPECT_EQ(with_adapter.ready_entries, service_only.ready_entries);
  EXPECT_EQ(with_adapter.ready_bytes, service_only.ready_bytes);

  EXPECT_THROW(dispatch_planned_tasks(graph, small_service, small_host, "cpu",
                                      101, small_plan, small_lease),
               GraphError);
  EXPECT_EQ(g_resource_product_operation_calls.load(std::memory_order_relaxed),
            0);
  EXPECT_EQ(small_host.context_entries(), 0);
  EXPECT_EQ(small_service.resource_snapshot().reserved, ResourceVector{});
  ASSERT_EQ(small_plan.temp_results().size(), 1U);
  EXPECT_FALSE(small_plan.temp_results().front().has_value());

  ExecutionService large_service(1U);
  ExecutionServiceHost large_host;
  ComputeRun large_run(
      make_test_submission(graph_identity, graph.revision().value(), 101));
  ASSERT_TRUE(large_run.advance_to(ComputeRunPhase::Admitted));
  TaskSubmissionPlan& large_plan = large_run.emplace_submission_plan(
      graph, traversal, 101, std::vector<Device>{Device::CPU});
  TimingCollector large_timings;
  std::mutex large_timing_mutex;
  large_plan.emplace_task_runner(NodeTaskRunnerContext{
      graph,
      cache,
      events,
      large_service,
      large_timings,
      large_timing_mutex,
      large_plan.execution_order(),
      large_plan.id_to_idx(),
      large_plan.temp_results(),
      large_plan.resolved_ops(),
      large_plan.compute_plan().task_graph,
      false,
      false,
      true,
      nullptr,
  });
  ASSERT_TRUE(large_run.advance_to(ComputeRunPhase::Queued));
  ASSERT_TRUE(large_run.advance_to(ComputeRunPhase::Running));
  ComputeRunLease large_lease = large_run.acquire_lease();
  EXPECT_NO_THROW(dispatch_planned_tasks(graph, large_service, large_host,
                                         "cpu", 101, large_plan, large_lease));
  EXPECT_EQ(g_resource_product_operation_calls.load(std::memory_order_relaxed),
            1);
  EXPECT_EQ(large_host.context_entries(), 1);
  EXPECT_EQ(large_host.context_entries(), large_host.context_exits());
  EXPECT_EQ(large_service.resource_snapshot().reserved, ResourceVector{});
  ASSERT_EQ(large_plan.temp_results().size(), 1U);
  ASSERT_TRUE(large_plan.temp_results().front().has_value());
  EXPECT_EQ(large_plan.temp_results().front()->data.at("value").as_int64(), 7);
}

/**
 * @brief Proves HP and RT charge complete dirty synchronization ownership.
 *
 * @return Nothing.
 * @throws Standard allocation, filesystem, graph, or service exceptions from
 * fixture setup.
 * @note For each real product path, a default-capacity calibration with a
 * small owner captures the complete admitted vector. That exact vector then
 * admits an identical fresh graph/plan with the same small owner. Changing
 * only the owner to add unused ids exceeds the retained cap before callback
 * entry. Removing either production synchronization-demand injection makes
 * the corresponding large-owner endpoint execute and fail this regression.
 */
TEST(ExecutionServiceProductResources,
     DirtyHpAndRtUseExactSmallLargeSynchronizationInterval) {
  ensure_resource_product_operations_registered();
  for (const ComputeIntent intent :
       {ComputeIntent::GlobalHighPrecision, ComputeIntent::RealTimeUpdate}) {
    const bool is_rt = intent == ComputeIntent::RealTimeUpdate;
    SCOPED_TRACE(is_rt ? "RT" : "HP");
    const int node_id = is_rt ? 112 : 111;
    const std::string graph_identity =
        is_rt ? "resource-dirty-rt-product" : "resource-dirty-hp-product";
    const std::string subtype = is_rt ? "dirty_rt" : "dirty_hp";
    std::atomic_int& operation_calls =
        is_rt ? g_resource_dirty_rt_operation_calls
              : g_resource_dirty_hp_operation_calls;
    const std::vector<int> small_owner_ids{node_id};
    const std::vector<int> large_owner_ids{node_id, node_id + 1000,
                                           node_id + 2000, node_id + 3000};

    operation_calls.store(0, std::memory_order_relaxed);
    const DirtyProductResourceResult calibration =
        execute_dirty_product_resource_case(
            is_rt ? "dirty-rt-sync-calibration" : "dirty-hp-sync-calibration",
            graph_identity, node_id, subtype, intent, small_owner_ids,
            ExecutionService::default_resource_limits());
    EXPECT_FALSE(calibration.failure_code.has_value());
    ASSERT_TRUE(calibration.admitted_resources.has_value());
    EXPECT_EQ(calibration.observation_count, 1);
    EXPECT_EQ(operation_calls.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(calibration.reserved_after, ResourceVector{});
    ASSERT_GT(calibration.synchronization_bytes, 0U);
    EXPECT_GT(calibration.admitted_resources->retained_memory_bytes,
              calibration.synchronization_bytes);
    EXPECT_EQ(calibration.run_phase, ComputeRunPhase::CommitPending);
    EXPECT_EQ(calibration.high_precision_value, is_rt ? 3 : 11);
    if (is_rt) {
      ASSERT_TRUE(calibration.real_time_value.has_value());
      EXPECT_EQ(*calibration.real_time_value, 12);
    } else {
      EXPECT_FALSE(calibration.real_time_value.has_value());
    }

    const ExecutionResourceLimits exact_small_limits =
        execution_limits(*calibration.admitted_resources);
    operation_calls.store(0, std::memory_order_relaxed);
    const DirtyProductResourceResult exact_small =
        execute_dirty_product_resource_case(
            is_rt ? "dirty-rt-sync-exact-small" : "dirty-hp-sync-exact-small",
            graph_identity, node_id, subtype, intent, small_owner_ids,
            exact_small_limits);
    EXPECT_FALSE(exact_small.failure_code.has_value());
    ASSERT_TRUE(exact_small.admitted_resources.has_value());
    EXPECT_EQ(*exact_small.admitted_resources, *calibration.admitted_resources);
    EXPECT_EQ(exact_small.synchronization_bytes,
              calibration.synchronization_bytes);
    EXPECT_EQ(exact_small.observation_count, 1);
    EXPECT_EQ(operation_calls.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(exact_small.reserved_after, ResourceVector{});
    EXPECT_EQ(exact_small.run_phase, ComputeRunPhase::CommitPending);
    EXPECT_EQ(exact_small.high_precision_value, is_rt ? 3 : 11);
    if (is_rt) {
      ASSERT_TRUE(exact_small.real_time_value.has_value());
      EXPECT_EQ(*exact_small.real_time_value, 12);
    } else {
      EXPECT_FALSE(exact_small.real_time_value.has_value());
    }

    operation_calls.store(0, std::memory_order_relaxed);
    const DirtyProductResourceResult rejected_large =
        execute_dirty_product_resource_case(
            is_rt ? "dirty-rt-sync-rejected-large"
                  : "dirty-hp-sync-rejected-large",
            graph_identity, node_id, subtype, intent, large_owner_ids,
            exact_small_limits);
    ASSERT_TRUE(rejected_large.failure_code.has_value());
    EXPECT_EQ(*rejected_large.failure_code, GraphErrc::ComputeError);
    EXPECT_GT(rejected_large.synchronization_bytes,
              exact_small.synchronization_bytes);
    EXPECT_FALSE(rejected_large.admitted_resources.has_value());
    EXPECT_EQ(rejected_large.observation_count, 0);
    EXPECT_EQ(operation_calls.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(rejected_large.reserved_after, ResourceVector{});
    EXPECT_EQ(rejected_large.high_precision_value, 3);
    EXPECT_FALSE(rejected_large.real_time_value.has_value());
  }
}

/**
 * @brief Proves downstream HP admission includes source-created staging.
 *
 * @return Nothing.
 * @throws Standard graph, Run, context, service, or staging exceptions
 * unchanged.
 * @note A source service segment first inserts one Run-owned HP entry and
 * settles. The downstream phase then charges that current buffer through its
 * live Run lease while its phase-local callback adds only one still-missing
 * node. A cap omitting exactly the existing-entry growth rejects before either
 * downstream callback; the complete cap executes and releases the ledger.
 */
TEST(ExecutionServiceProductResources,
     HpDownstreamAdmissionIncludesSettledSourceStaging) {
  constexpr int kSourceNodeId = 221;
  constexpr int kMissingNodeId = 222;
  const std::string graph_identity = "resource-hp-phase-current-staging";
  GraphModel graph("cache/resource-hp-phase-current-staging");
  graph.add_node(make_resource_product_node(kSourceNodeId));
  graph.add_node(make_resource_product_node(kMissingNodeId));
  graph.validate_topology();

  ComputePlan compute_plan;
  compute_plan.intent = ComputeIntent::GlobalHighPrecision;
  compute_plan.target_node_id = kMissingNodeId;
  compute_plan.parallel = true;
  compute_plan.execution_order = {kSourceNodeId, kMissingNodeId};
  compute_plan.planned_nodes = {kSourceNodeId, kMissingNodeId};
  compute_plan.task_graph.tasks.resize(3U);
  compute_plan.task_graph.tasks[0].task_id = 0;
  compute_plan.task_graph.tasks[0].node_id = kSourceNodeId;
  compute_plan.task_graph.tasks[1].task_id = 1;
  compute_plan.task_graph.tasks[1].node_id = kSourceNodeId;
  compute_plan.task_graph.tasks[2].task_id = 2;
  compute_plan.task_graph.tasks[2].node_id = kMissingNodeId;
  const std::vector<Device> task_devices(compute_plan.task_graph.tasks.size(),
                                         Device::CPU);

  ComputeRun run(make_test_submission(graph_identity, graph.revision().value(),
                                      kMissingNodeId));
  ASSERT_TRUE(run.advance_to(ComputeRunPhase::Admitted));
  HighPrecisionDirtyWriteBuffer& hp_buffer =
      run.emplace_dirty_hp_write_buffer(false);
  ComputeRunLease baseline_lease = run.acquire_lease();
  const std::uint64_t baseline_run_bytes =
      baseline_lease.retained_memory_bytes();
  const std::uint64_t baseline_buffer_bytes = hp_buffer.retained_memory_bytes();

  std::atomic_int source_calls{0};
  auto source_context = std::make_shared<DirtyReadyTaskContext>(
      compute_plan, nullptr, std::vector<int>{0}, task_devices,
      [&graph, &hp_buffer, &source_calls](int) {
        (void)hp_buffer.ensure_output(graph.node(kSourceNodeId));
        source_calls.fetch_add(1, std::memory_order_relaxed);
      },
      owned_callable_retained_memory_bytes(static_cast<std::uint64_t>(
          sizeof(std::reference_wrapper<GraphModel>) +
          sizeof(std::reference_wrapper<HighPrecisionDirtyWriteBuffer>) +
          sizeof(std::reference_wrapper<std::atomic_int>))),
      run.acquire_lease(), false, ExecutionTaskPriority::High);
  std::vector<ReadyTaskSubmission> source_submissions =
      source_context->make_submissions({0}, true);
  const std::uint64_t source_missing_bytes =
      hp_buffer.missing_entry_retained_memory_bytes(graph, {kSourceNodeId});
  ExecutionService source_service(1U);
  ExecutionServiceHost source_host;
  EXPECT_NO_THROW(source_service.execute_run(
      source_host, "cpu", std::move(source_submissions), 1,
      source_context->run_resource_demand(source_missing_bytes)));
  EXPECT_EQ(source_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(source_service.resource_snapshot().reserved, ResourceVector{});

  const std::uint64_t current_buffer_bytes = hp_buffer.retained_memory_bytes();
  ASSERT_GT(current_buffer_bytes, baseline_buffer_bytes);
  const std::uint64_t existing_source_entry_bytes =
      current_buffer_bytes - baseline_buffer_bytes;
  ComputeRunLease current_lease = run.acquire_lease();
  EXPECT_EQ(current_lease.retained_memory_bytes() - baseline_run_bytes,
            existing_source_entry_bytes);
  EXPECT_EQ(
      hp_buffer.missing_entry_retained_memory_bytes(graph, {kSourceNodeId}),
      0U);
  const std::uint64_t missing_downstream_bytes =
      hp_buffer.missing_entry_retained_memory_bytes(graph, {kMissingNodeId});
  ASSERT_GT(missing_downstream_bytes, 0U);
  EXPECT_EQ(hp_buffer.missing_entry_retained_memory_bytes(
                graph, {kSourceNodeId, kMissingNodeId}),
            missing_downstream_bytes);

  std::atomic_int downstream_calls{0};
  const std::vector<int> downstream_task_ids{1, 2};
  auto downstream_context = std::make_shared<DirtyReadyTaskContext>(
      compute_plan, nullptr, downstream_task_ids, task_devices,
      [&downstream_calls](int) {
        downstream_calls.fetch_add(1, std::memory_order_relaxed);
      },
      owned_callable_retained_memory_bytes(static_cast<std::uint64_t>(
          sizeof(std::reference_wrapper<std::atomic_int>))),
      run.acquire_lease(), false, ExecutionTaskPriority::Normal);
  const CpuRunResourceDemand downstream_demand =
      downstream_context->run_resource_demand(missing_downstream_bytes);
  RetainedMemoryEstimator expected_downstream_shared(
      "test HP downstream shared demand");
  expected_downstream_shared.add_bytes(
      downstream_context->retained_memory_bytes());
  expected_downstream_shared.add_bytes(current_lease.retained_memory_bytes());
  expected_downstream_shared.add_bytes(missing_downstream_bytes);
  EXPECT_EQ(downstream_demand.shared_retained_memory_bytes,
            expected_downstream_shared.bytes());

  ExecutionService probe(1U);
  std::vector<ReadyTaskSubmission> representative_submissions =
      downstream_context->make_submissions(downstream_task_ids, true);
  const ResourceVector complete_required = probe.estimate_cpu_run_resources(
      representative_submissions.front(),
      static_cast<int>(downstream_task_ids.size()), downstream_demand);
  ASSERT_GT(complete_required.retained_memory_bytes,
            existing_source_entry_bytes);
  ResourceVector omitted_existing_entry = complete_required;
  omitted_existing_entry.retained_memory_bytes -= existing_source_entry_bytes;

  ExecutionService lower_service(1U, execution_limits(omitted_existing_entry));
  ExecutionServiceHost lower_host;
  EXPECT_THROW(
      lower_service.execute_run(
          lower_host, "cpu", std::move(representative_submissions),
          static_cast<int>(downstream_task_ids.size()), downstream_demand),
      GraphError);
  EXPECT_EQ(downstream_calls.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(lower_service.resource_snapshot().reserved, ResourceVector{});

  ExecutionService complete_service(1U, execution_limits(complete_required));
  ExecutionServiceHost complete_host;
  std::vector<ReadyTaskSubmission> complete_submissions =
      downstream_context->make_submissions(downstream_task_ids, true);
  EXPECT_NO_THROW(complete_service.execute_run(
      complete_host, "cpu", std::move(complete_submissions),
      static_cast<int>(downstream_task_ids.size()), downstream_demand));
  EXPECT_EQ(downstream_calls.load(std::memory_order_relaxed), 2);
  EXPECT_EQ(complete_service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Exercises connected-parameter preflight resource admission.
 *
 * @return Nothing.
 * @throws Standard allocation, registry, graph, or service exceptions from
 * fixture setup.
 * @note The exact zero-adapter envelope is proved usable, then rejects the
 * production preflight adapter before the registered producer runs because
 * its Run/snapshot/staging interval is absent. Default capacity executes and
 * retains the named output with exact root release.
 */
TEST(ExecutionServiceProductResources,
     PreflightRejectsSmallLimitAndExecutesWithDefaultLimit) {
  ensure_resource_product_operations_registered();
  g_resource_preflight_operation_calls.store(0, std::memory_order_relaxed);
  const std::string graph_identity = "resource-preflight-product";
  const ResourceVector service_only =
      estimate_service_only_resources(graph_identity);
  ASSERT_NO_FATAL_FAILURE(
      expect_service_only_envelope_executes(graph_identity, service_only));

  GraphModel graph("cache/resource-preflight-product");
  Node producer = make_resource_product_node(201);
  producer.subtype = "parameter_source";
  Node target = make_plan_node(202);
  target.parameter_inputs.push_back({201, "value", "radius"});
  graph.add_node(producer);
  graph.add_node(target);
  graph.validate_topology();
  GraphTraversalService traversal;

  ExecutionService small_service(1U, execution_limits(service_only));
  ExecutionServiceHost small_host;
  ComputeRun small_run(
      make_test_submission(graph_identity, graph.revision().value(), 202));
  ASSERT_TRUE(small_run.advance_to(ComputeRunPhase::Admitted));
  EXPECT_THROW((void)stabilize_connected_dirty_parameters(
                   graph, traversal, 202, 1U, graph.topology_generation(),
                   nullptr, &small_service, &small_host, &small_run),
               GraphError);
  EXPECT_EQ(
      g_resource_preflight_operation_calls.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(small_host.context_entries(), 0);
  EXPECT_EQ(small_service.resource_snapshot().reserved, ResourceVector{});
  ComputeRunLease retained_small_run = small_run.acquire_lease();
  const std::uint64_t retained_run_bytes =
      retained_small_run.retained_memory_bytes();
  ASSERT_GT(retained_run_bytes, 0U);
  std::atomic_int estimate_entered{0};
  ReadyTaskSubmission run_representative = make_counted_ready_submission(
      retained_small_run, 0U, 201, estimate_entered);
  const ResourceVector with_run = small_service.estimate_cpu_run_resources(
      run_representative, 1, CpuRunResourceDemand{retained_run_bytes, {}});
  EXPECT_EQ(with_run.retained_memory_bytes - service_only.retained_memory_bytes,
            retained_run_bytes);

  ExecutionService large_service(1U);
  ExecutionServiceHost large_host;
  ComputeRun large_run(
      make_test_submission(graph_identity, graph.revision().value(), 202));
  ASSERT_TRUE(large_run.advance_to(ComputeRunPhase::Admitted));
  std::shared_ptr<const StabilizedDirtyParameters> stabilized;
  EXPECT_NO_THROW(stabilized = stabilize_connected_dirty_parameters(
                      graph, traversal, 202, 2U, graph.topology_generation(),
                      nullptr, &large_service, &large_host, &large_run));
  ASSERT_TRUE(stabilized);
  EXPECT_TRUE(stabilized->has_connected_parameters());
  const NodeOutput* parameter_output = stabilized->find_parameter_output(201);
  ASSERT_NE(parameter_output, nullptr);
  EXPECT_EQ(parameter_output->data.at("value").as_int64(), 9);
  EXPECT_EQ(
      g_resource_preflight_operation_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(large_host.context_entries(), 1);
  EXPECT_EQ(large_host.context_entries(), large_host.context_exits());
  EXPECT_EQ(large_service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies HP, RT, and preflight map growth is predicted exactly once.
 *
 * @return Nothing.
 * @throws Standard staging, metadata, allocation, or service exceptions
 * unchanged.
 * @note Duplicate ids collapse to one charge, existing HP/RT keys contribute
 * zero, empty-output metadata closes the observed insertion delta exactly,
 * and seeded projections never understate cloned Host-owned metadata. An exact
 * current-state capacity rejects before callback entry while
 * current-plus-one-entry capacity succeeds.
 */
TEST(ExecutionServiceProductResources,
     PredictableStagingEntriesCloseExactAdmissionIntervals) {
  constexpr int kFirstNodeId = 301;
  constexpr int kSecondNodeId = 302;
  constexpr int kSeededNodeId = 303;
  GraphModel staging_graph("cache/resource-staging-entries");
  staging_graph.add_node(make_resource_product_node(kFirstNodeId));
  staging_graph.add_node(make_resource_product_node(kSecondNodeId));
  staging_graph.add_node(
      make_resource_dirty_product_node(kSeededNodeId, "dirty_hp"));
  staging_graph.validate_topology();

  HighPrecisionDirtyWriteBuffer hp_buffer(false);
  const std::uint64_t hp_current = hp_buffer.retained_memory_bytes();
  const std::uint64_t hp_one = hp_buffer.missing_entry_retained_memory_bytes(
      staging_graph, {kFirstNodeId});
  ASSERT_GT(hp_one, 0U);
  EXPECT_EQ(hp_buffer.missing_entry_retained_memory_bytes(
                staging_graph, {kFirstNodeId, kFirstNodeId}),
            hp_one);
  EXPECT_EQ(hp_buffer.missing_entry_retained_memory_bytes(
                staging_graph, {kFirstNodeId, kSecondNodeId}),
            2U * hp_one);
  ASSERT_NO_FATAL_FAILURE(
      expect_pending_entry_interval("resource-staging-hp", hp_current, hp_one));
  const Node& hp_node = staging_graph.node(kFirstNodeId);
  (void)hp_buffer.ensure_output(hp_node);
  EXPECT_EQ(hp_buffer.retained_memory_bytes() - hp_current, hp_one);
  EXPECT_EQ(hp_buffer.missing_entry_retained_memory_bytes(staging_graph,
                                                          {kFirstNodeId}),
            0U);
  EXPECT_EQ(hp_buffer.missing_entry_retained_memory_bytes(
                staging_graph, {kFirstNodeId, kSecondNodeId}),
            hp_one);
  HighPrecisionDirtyWriteBuffer seeded_hp_buffer(true);
  const std::uint64_t seeded_hp_current =
      seeded_hp_buffer.retained_memory_bytes();
  const std::uint64_t seeded_hp_projected =
      seeded_hp_buffer.missing_entry_retained_memory_bytes(staging_graph,
                                                           {kSeededNodeId});
  (void)seeded_hp_buffer.ensure_output(staging_graph.node(kSeededNodeId));
  EXPECT_GE(seeded_hp_projected,
            seeded_hp_buffer.retained_memory_bytes() - seeded_hp_current);

  RealtimeProxyGraph proxy_graph;
  RealtimeProxyWriteBuffer rt_buffer(proxy_graph, false);
  const std::uint64_t rt_current = rt_buffer.retained_memory_bytes();
  const std::uint64_t rt_one =
      rt_buffer.missing_entry_retained_memory_bytes({kFirstNodeId});
  ASSERT_GT(rt_one, 0U);
  EXPECT_EQ(rt_buffer.missing_entry_retained_memory_bytes(
                {kFirstNodeId, kFirstNodeId}),
            rt_one);
  EXPECT_EQ(rt_buffer.missing_entry_retained_memory_bytes(
                {kFirstNodeId, kSecondNodeId}),
            2U * rt_one);
  ASSERT_NO_FATAL_FAILURE(
      expect_pending_entry_interval("resource-staging-rt", rt_current, rt_one));
  (void)rt_buffer.ensure_output(kFirstNodeId);
  EXPECT_EQ(rt_buffer.retained_memory_bytes() - rt_current, rt_one);
  EXPECT_EQ(rt_buffer.missing_entry_retained_memory_bytes({kFirstNodeId}), 0U);
  EXPECT_EQ(rt_buffer.missing_entry_retained_memory_bytes(
                {kFirstNodeId, kSecondNodeId}),
            rt_one);
  proxy_graph.synchronize_with_graph(staging_graph);
  RealtimeProxyGraph::NodeState seeded_proxy_state;
  seeded_proxy_state.output = make_resource_dirty_output(4);
  proxy_graph.commit_node_state(kSeededNodeId, std::move(seeded_proxy_state));
  RealtimeProxyWriteBuffer seeded_rt_buffer(proxy_graph, true);
  const std::uint64_t seeded_rt_current =
      seeded_rt_buffer.retained_memory_bytes();
  const std::uint64_t seeded_rt_projected =
      seeded_rt_buffer.missing_entry_retained_memory_bytes({kSeededNodeId});
  (void)seeded_rt_buffer.ensure_output(kSeededNodeId);
  EXPECT_GE(seeded_rt_projected,
            seeded_rt_buffer.retained_memory_bytes() - seeded_rt_current);

  StabilizedDirtyParameters preflight;
  const std::uint64_t preflight_current = preflight.retained_memory_bytes();
  const std::uint64_t preflight_one =
      preflight.missing_staged_output_entry_retained_memory_bytes(
          {kFirstNodeId});
  ASSERT_GT(preflight_one, 0U);
  EXPECT_EQ(preflight.missing_staged_output_entry_retained_memory_bytes(
                {kFirstNodeId, kFirstNodeId}),
            preflight_one);
  EXPECT_EQ(preflight.missing_staged_output_entry_retained_memory_bytes(
                {kFirstNodeId, kSecondNodeId}),
            2U * preflight_one);
  ASSERT_NO_FATAL_FAILURE(expect_pending_entry_interval(
      "resource-staging-preflight", preflight_current, preflight_one));
}

/**
 * @brief Verifies initial work uses bounded lanes and high precedes normal.
 *
 * @return Nothing.
 * @throws std::bad_alloc, std::future_error, or std::system_error from test
 * setup and synchronization.
 * @note The normal entry is submitted first. The later high entry blocks while
 * executing, leaving exactly one normal entry and its bytes in the bounded
 * store until release.
 */
TEST(ExecutionService, BoundsInitialReadyStoreAndPreservesPriorityOrdering) {
  constexpr ReadyTaskResourceDemand kDemand{3U, 4U, 5U};
  ExecutionServiceHost host;
  ComputeRun run(make_test_submission("bounded-initial-ready", 1U, 1));
  std::vector<int> execution_order;
  std::promise<void> high_entered;
  std::future<void> high_entered_future = high_entered.get_future();
  std::promise<void> release_high;
  const std::shared_future<void> high_release =
      release_high.get_future().share();

  ComputeRunLease normal_lease = run.acquire_lease();
  const ComputeRunTaskIdentity normal_identity = normal_lease.task_identity(0U);
  ComputeRunLease high_lease = run.acquire_lease();
  const ComputeRunTaskIdentity high_identity = high_lease.task_identity(1U);
  std::vector<ReadyTaskSubmission> ready;
  ready.emplace_back(
      std::move(normal_lease), normal_identity, 1, true,
      [&execution_order](ComputeRunLease&, const ComputeRunTaskIdentity&,
                         ExecutionTaskRuntime& runtime) {
        execution_order.push_back(1);
        runtime.dec_tasks_to_complete();
      },
      ExecutionTaskPriority::Normal, kDemand);
  ready.emplace_back(
      std::move(high_lease), high_identity, 2, true,
      [&execution_order, &high_entered, high_release](
          ComputeRunLease&, const ComputeRunTaskIdentity&,
          ExecutionTaskRuntime& runtime) {
        execution_order.push_back(2);
        high_entered.set_value();
        high_release.wait();
        runtime.dec_tasks_to_complete();
      },
      ExecutionTaskPriority::High, kDemand);

  const CpuRunResourceDemand run_demand{0U, kDemand};
  ExecutionService probe(1U);
  const ResourceVector required =
      probe.estimate_cpu_run_resources(ready.front(), 2, run_demand);
  ExecutionService service(1U, execution_limits(required));
  std::future<void> run_future;
  ScopedPromiseRelease release_guard(release_high);
  run_future = std::async(
      std::launch::async,
      [&service, &host, ready = std::move(ready), run_demand]() mutable {
        service.execute_run(host, "cpu", std::move(ready), 2, run_demand);
      });
  ASSERT_EQ(high_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  const std::string stats = service.get_stats();
  EXPECT_NE(stats.find("Ready tasks: 1"), std::string::npos);
  EXPECT_NE(
      stats.find("Ready bytes: " + std::to_string(required.ready_bytes / 2U)),
      std::string::npos);
  EXPECT_EQ(service.resource_snapshot().reserved, required);

  EXPECT_TRUE(release_guard.release());
  ASSERT_EQ(run_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(run_future.get());
  EXPECT_EQ(execution_order, (std::vector<int>{2, 1}));
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies dependency-released work re-enters the same bounded store.
 *
 * @return Nothing.
 * @throws std::bad_alloc, std::future_error, or std::system_error from test
 * setup and synchronization.
 * @note One executing initial callback publishes a matching dependent then
 * blocks. With one worker, diagnostics must expose exactly that dependent's
 * entry and bytes before both logical units complete and release the root.
 */
TEST(ExecutionService, BoundsDependentReentryAndReleasesRootExactly) {
  constexpr ReadyTaskResourceDemand kDemand{3U, 4U, 5U};
  ExecutionServiceHost host;
  ComputeRun run(make_test_submission("bounded-dependent-ready", 1U, 1));
  std::atomic_int entered{0};
  std::promise<void> dependent_queued;
  std::future<void> dependent_queued_future = dependent_queued.get_future();
  std::promise<void> release_initial;
  const std::shared_future<void> initial_release =
      release_initial.get_future().share();

  ComputeRunLease initial_lease = run.acquire_lease();
  const ComputeRunTaskIdentity initial_identity =
      initial_lease.task_identity(0U);
  std::vector<ReadyTaskSubmission> ready;
  ready.emplace_back(
      std::move(initial_lease), initial_identity, 1, true,
      [&entered, &dependent_queued, initial_release, kDemand](
          ComputeRunLease& retained_lease, const ComputeRunTaskIdentity&,
          ExecutionTaskRuntime& runtime) {
        auto& ready_runtime =
            dynamic_cast<ReadyTaskSubmissionRuntime&>(runtime);
        const ComputeRunTaskIdentity dependent_identity =
            retained_lease.task_identity(1U);
        ready_runtime.submit_ready_submission(ReadyTaskSubmission(
            retained_lease, dependent_identity, 2, false,
            [&entered](ComputeRunLease&, const ComputeRunTaskIdentity&,
                       ExecutionTaskRuntime& dependent_runtime) {
              entered.fetch_add(1, std::memory_order_relaxed);
              dependent_runtime.dec_tasks_to_complete();
            },
            ExecutionTaskPriority::Normal, kDemand));
        dependent_queued.set_value();
        initial_release.wait();
        entered.fetch_add(1, std::memory_order_relaxed);
        runtime.dec_tasks_to_complete();
      },
      ExecutionTaskPriority::Normal, kDemand);

  const CpuRunResourceDemand run_demand{0U, kDemand};
  ExecutionService probe(1U);
  const ResourceVector required =
      probe.estimate_cpu_run_resources(ready.front(), 2, run_demand);
  ExecutionService service(1U, execution_limits(required));
  std::future<void> run_future;
  ScopedPromiseRelease release_guard(release_initial);
  run_future = std::async(
      std::launch::async,
      [&service, &host, ready = std::move(ready), run_demand]() mutable {
        service.execute_run(host, "cpu", std::move(ready), 2, run_demand);
      });
  ASSERT_EQ(dependent_queued_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  const std::string stats = service.get_stats();
  EXPECT_NE(stats.find("Ready tasks: 1"), std::string::npos);
  EXPECT_NE(
      stats.find("Ready bytes: " + std::to_string(required.ready_bytes / 2U)),
      std::string::npos);
  EXPECT_EQ(service.resource_snapshot().reserved, required);

  EXPECT_TRUE(release_guard.release());
  ASSERT_EQ(run_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(run_future.get());
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 2);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies policy service charges trusted work and complete ready bytes.
 *
 * @return Nothing.
 * @throws Standard Run, service, future, or synchronization exceptions from
 * the real one-worker execution path.
 * @note Three equal-weight Runs share one Graph. The expensive-work Run is
 * published first, the multi-quantum ready-byte Run second, and the cheap Run
 * last. Charged service must override enqueue order while stable order remains
 * the final tie break.
 */
TEST(ExecutionServicePolicy, ChargesWorkAndReadyBytesBeforeStableTie) {
  constexpr ReadyTaskResourceDemand kBlockerDemand{1U, 1U, 1U, 1U};
  constexpr ReadyTaskResourceDemand kWorkDemand{1U, 1U, 1U, 20U};
  constexpr ReadyTaskResourceDemand kByteDemand{1U, 1U, 8193U, 1U};
  constexpr ReadyTaskResourceDemand kCheapDemand{1U, 1U, 1U, 1U};
  ExecutionService service(1U);
  std::promise<void> blocker_entered;
  std::future<void> blocker_entered_future = blocker_entered.get_future();
  std::promise<void> release_blocker;
  const std::shared_future<void> blocker_release =
      release_blocker.get_future().share();
  std::vector<int> execution_order;
  std::mutex execution_order_mutex;
  AsyncPolicyRun blocker;
  std::vector<AsyncPolicyRun> targets;
  targets.reserve(3U);
  ScopedPromiseRelease release_guard(release_blocker);

  blocker = launch_blocking_policy_run(
      service,
      make_policy_submission("cost-blocker", 1U, 0,
                             ComputeRunQosClass::Throughput),
      kBlockerDemand, blocker_entered, blocker_release);
  ASSERT_EQ(blocker_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);

  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission("cost-policy-graph", 2U, 20,
                             ComputeRunQosClass::Throughput),
      {3}, kWorkDemand, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 1U));
  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission("cost-policy-graph", 3U, 21,
                             ComputeRunQosClass::Throughput),
      {2}, kByteDemand, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 2U));
  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission("cost-policy-graph", 4U, 22,
                             ComputeRunQosClass::Throughput),
      {1}, kCheapDemand, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 3U));

  EXPECT_TRUE(release_guard.release());
  expect_policy_runs_settle(targets);
  ASSERT_EQ(blocker.completion.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(blocker.completion.get());
  EXPECT_EQ(execution_order, (std::vector<int>{1, 2, 3}));
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies exact 4096/8192-byte policy quantum boundaries.
 *
 * @return Nothing.
 * @throws Standard Run, estimator, service, future, or synchronization
 * exceptions from the real one-worker execution path.
 * @note Complete ready grants are constructed at 4096, 4097, 8192, and 8193
 * bytes. Adversarial publication order makes either floor division or charging
 * an extra quantum at an exact boundary change the observed selection order.
 */
TEST(ExecutionServicePolicy, ChargesExactReadyByteQuantumBoundaries) {
  constexpr ReadyTaskResourceDemand kBaseDemand{1U, 1U, 0U, 1U};
  constexpr ReadyTaskResourceDemand kBlockerDemand{1U, 1U, 1U, 1U};
  const std::string graph_identity = "quantum-graph";
  ExecutionService service(1U);

  std::atomic_int representative_entered{0};
  ComputeRun representative_run(make_policy_submission(
      graph_identity, 1U, 0, ComputeRunQosClass::Throughput));
  ReadyTaskSubmission representative = make_counted_ready_submission(
      representative_run.acquire_lease(), 0U, 0, representative_entered,
      ExecutionTaskPriority::Normal, kBaseDemand);
  const ResourceVector base_resources = service.estimate_cpu_run_resources(
      representative, 1, CpuRunResourceDemand{0U, kBaseDemand});
  ASSERT_GT(base_resources.ready_bytes, 0U);
  ASSERT_LT(base_resources.ready_bytes, 4096U);

  const auto demand_for_complete_bytes =
      [base_ready_bytes =
           base_resources.ready_bytes](std::uint64_t complete_ready_bytes) {
        return ReadyTaskResourceDemand{
            1U, 1U, complete_ready_bytes - base_ready_bytes, 1U};
      };
  const ReadyTaskResourceDemand demand_4096 = demand_for_complete_bytes(4096U);
  const ReadyTaskResourceDemand demand_4097 = demand_for_complete_bytes(4097U);
  const ReadyTaskResourceDemand demand_8192 = demand_for_complete_bytes(8192U);
  const ReadyTaskResourceDemand demand_8193 = demand_for_complete_bytes(8193U);

  std::promise<void> blocker_entered;
  std::future<void> blocker_entered_future = blocker_entered.get_future();
  std::promise<void> release_blocker;
  const std::shared_future<void> blocker_release =
      release_blocker.get_future().share();
  std::vector<int> execution_order;
  std::mutex execution_order_mutex;
  AsyncPolicyRun blocker;
  std::vector<AsyncPolicyRun> targets;
  targets.reserve(4U);
  ScopedPromiseRelease release_guard(release_blocker);

  blocker = launch_blocking_policy_run(
      service,
      make_policy_submission("quantum-blocker", 2U, 0,
                             ComputeRunQosClass::Throughput),
      kBlockerDemand, blocker_entered, blocker_release);
  ASSERT_EQ(blocker_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);

  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission(graph_identity, 3U, 8193,
                             ComputeRunQosClass::Throughput),
      {8193}, demand_8193, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 1U));
  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission(graph_identity, 4U, 8192,
                             ComputeRunQosClass::Throughput),
      {8192}, demand_8192, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 2U));
  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission(graph_identity, 5U, 4097,
                             ComputeRunQosClass::Throughput),
      {4097}, demand_4097, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 3U));
  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission(graph_identity, 6U, 4096,
                             ComputeRunQosClass::Throughput),
      {4096}, demand_4096, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 4U));

  EXPECT_TRUE(release_guard.release());
  expect_policy_runs_settle(targets);
  ASSERT_EQ(blocker.completion.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(blocker.completion.get());
  EXPECT_EQ(execution_order, (std::vector<int>{4096, 8192, 4097, 8193}));
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies repeatable Graph-first and Run-second charged fairness.
 *
 * @return Nothing.
 * @throws Standard Run, service, future, or synchronization exceptions from
 * three independent repetitions of the real one-worker path.
 * @note Graph A owns two Runs while Graph B owns one. Equal raw costs must
 * alternate Graph service rather than grant Graph A twice the share, and the
 * two Graph-A Runs must alternate within their shared Graph row.
 */
TEST(ExecutionServicePolicy, RepeatsHierarchicalGraphAndRunFairness) {
  constexpr ReadyTaskResourceDemand kDemand{1U, 1U, 1U, 1U};
  for (int repetition = 0; repetition < 3; ++repetition) {
    ExecutionService service(1U);
    std::promise<void> blocker_entered;
    std::future<void> blocker_entered_future = blocker_entered.get_future();
    std::promise<void> release_blocker;
    const std::shared_future<void> blocker_release =
        release_blocker.get_future().share();
    std::vector<int> execution_order;
    std::mutex execution_order_mutex;
    AsyncPolicyRun blocker;
    std::vector<AsyncPolicyRun> targets;
    targets.reserve(3U);
    ScopedPromiseRelease release_guard(release_blocker);

    blocker = launch_blocking_policy_run(
        service,
        make_policy_submission("fair-blocker", repetition + 1U, 0,
                               ComputeRunQosClass::Throughput),
        kDemand, blocker_entered, blocker_release);
    ASSERT_EQ(blocker_entered_future.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    targets.push_back(launch_ordered_policy_run(
        service,
        make_policy_submission("fair-graph-a", repetition + 10U, 11,
                               ComputeRunQosClass::Throughput),
        {11, 11, 11}, kDemand, execution_order, execution_order_mutex));
    ASSERT_TRUE(wait_for_ready_task_count(service, 3U));
    targets.push_back(launch_ordered_policy_run(
        service,
        make_policy_submission("fair-graph-a", repetition + 20U, 12,
                               ComputeRunQosClass::Throughput),
        {12, 12, 12}, kDemand, execution_order, execution_order_mutex));
    ASSERT_TRUE(wait_for_ready_task_count(service, 6U));
    targets.push_back(launch_ordered_policy_run(
        service,
        make_policy_submission("fair-graph-b", repetition + 30U, 21,
                               ComputeRunQosClass::Throughput),
        {21, 21, 21}, kDemand, execution_order, execution_order_mutex));
    ASSERT_TRUE(wait_for_ready_task_count(service, 9U));

    EXPECT_TRUE(release_guard.release());
    expect_policy_runs_settle(targets);
    ASSERT_EQ(blocker.completion.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    EXPECT_NO_THROW(blocker.completion.get());
    ASSERT_GE(execution_order.size(), 6U);
    EXPECT_EQ(
        std::vector<int>(execution_order.begin(), execution_order.begin() + 6),
        (std::vector<int>{11, 21, 12, 21, 11, 21}));
    EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
  }
}

/**
 * @brief Verifies Graph fairness history is local to the selected QoS class.
 *
 * @return Nothing.
 * @throws Standard Run, service, future, or synchronization exceptions from
 * two real one-worker service paths.
 * @note In the first path Graph A receives only Throughput service before
 * equal-deadline/equal-cost Interactive A and B candidates are published in
 * that order; cross-class leakage would incorrectly choose B. The second path
 * gives Graph A prior Interactive service and must choose B, proving same-class
 * Graph fairness remains effective.
 */
TEST(ExecutionServicePolicy, KeepsGraphFairnessLocalToSelectedClass) {
  constexpr ReadyTaskResourceDemand kDemand{1U, 1U, 1U, 1U};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::minutes(1);

  {
    ExecutionService service(1U);
    std::promise<void> blocker_entered;
    std::future<void> blocker_entered_future = blocker_entered.get_future();
    std::promise<void> release_blocker;
    const std::shared_future<void> blocker_release =
        release_blocker.get_future().share();
    std::vector<int> execution_order;
    std::mutex execution_order_mutex;
    AsyncPolicyRun blocker;
    std::vector<AsyncPolicyRun> targets;
    targets.reserve(2U);
    ScopedPromiseRelease release_guard(release_blocker);

    blocker = launch_blocking_policy_run(
        service,
        make_policy_submission("class-local-graph-a", 1U, 0,
                               ComputeRunQosClass::Throughput),
        kDemand, blocker_entered, blocker_release);
    ASSERT_EQ(blocker_entered_future.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    targets.push_back(launch_ordered_policy_run(
        service,
        make_policy_submission("class-local-graph-a", 2U, 11,
                               ComputeRunQosClass::Interactive, 1U, deadline),
        {11}, kDemand, execution_order, execution_order_mutex));
    ASSERT_TRUE(wait_for_ready_task_count(service, 1U));
    targets.push_back(launch_ordered_policy_run(
        service,
        make_policy_submission("class-local-graph-b", 3U, 21,
                               ComputeRunQosClass::Interactive, 1U, deadline),
        {21}, kDemand, execution_order, execution_order_mutex));
    ASSERT_TRUE(wait_for_ready_task_count(service, 2U));

    EXPECT_TRUE(release_guard.release());
    expect_policy_runs_settle(targets);
    ASSERT_EQ(blocker.completion.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    EXPECT_NO_THROW(blocker.completion.get());
    EXPECT_EQ(execution_order, (std::vector<int>{11, 21}));
    EXPECT_TRUE(wait_for_resource_reservation(service, ResourceVector{}));
    EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
  }

  {
    ExecutionService service(1U);
    std::promise<void> blocker_entered;
    std::future<void> blocker_entered_future = blocker_entered.get_future();
    std::promise<void> release_blocker;
    const std::shared_future<void> blocker_release =
        release_blocker.get_future().share();
    std::vector<int> execution_order;
    std::mutex execution_order_mutex;
    AsyncPolicyRun blocker;
    std::vector<AsyncPolicyRun> targets;
    targets.reserve(2U);
    ScopedPromiseRelease release_guard(release_blocker);

    blocker = launch_blocking_policy_run(
        service,
        make_policy_submission("same-class-graph-a", 10U, 0,
                               ComputeRunQosClass::Interactive, 1U, deadline),
        kDemand, blocker_entered, blocker_release);
    ASSERT_EQ(blocker_entered_future.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    targets.push_back(launch_ordered_policy_run(
        service,
        make_policy_submission("same-class-graph-a", 11U, 11,
                               ComputeRunQosClass::Interactive, 1U, deadline),
        {11}, kDemand, execution_order, execution_order_mutex));
    ASSERT_TRUE(wait_for_ready_task_count(service, 1U));
    targets.push_back(launch_ordered_policy_run(
        service,
        make_policy_submission("same-class-graph-b", 12U, 21,
                               ComputeRunQosClass::Interactive, 1U, deadline),
        {21}, kDemand, execution_order, execution_order_mutex));
    ASSERT_TRUE(wait_for_ready_task_count(service, 2U));

    EXPECT_TRUE(release_guard.release());
    expect_policy_runs_settle(targets);
    ASSERT_EQ(blocker.completion.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    EXPECT_NO_THROW(blocker.completion.get());
    EXPECT_EQ(execution_order, (std::vector<int>{21, 11}));
    EXPECT_TRUE(wait_for_resource_reservation(service, ResourceVector{}));
    EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
  }
}

/**
 * @brief Verifies positive Run weight changes within-Graph service share.
 *
 * @return Nothing.
 * @throws Standard Run, service, future, or synchronization exceptions from
 * the real one-worker execution path.
 * @note Equal-cost Runs share one Graph. Weight sixteen normalizes each high-
 * weight charge below the weight-one charge without changing resource
 * admission, so the deterministic sequence exposes the weighted accounting.
 */
TEST(ExecutionServicePolicy, AppliesRunWeightWithinOneGraph) {
  constexpr ReadyTaskResourceDemand kDemand{1U, 1U, 1U, 1U};
  ExecutionService service(1U);
  std::promise<void> blocker_entered;
  std::future<void> blocker_entered_future = blocker_entered.get_future();
  std::promise<void> release_blocker;
  const std::shared_future<void> blocker_release =
      release_blocker.get_future().share();
  std::vector<int> execution_order;
  std::mutex execution_order_mutex;
  AsyncPolicyRun blocker;
  std::vector<AsyncPolicyRun> targets;
  targets.reserve(2U);
  ScopedPromiseRelease release_guard(release_blocker);

  blocker = launch_blocking_policy_run(
      service,
      make_policy_submission("weight-blocker", 1U, 0,
                             ComputeRunQosClass::Throughput),
      kDemand, blocker_entered, blocker_release);
  ASSERT_EQ(blocker_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission("weighted-graph", 2U, 16,
                             ComputeRunQosClass::Throughput, 16U),
      {16, 16, 16, 16}, kDemand, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 4U));
  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission("weighted-graph", 3U, 1,
                             ComputeRunQosClass::Throughput, 1U),
      {1, 1, 1, 1}, kDemand, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 8U));

  EXPECT_TRUE(release_guard.release());
  expect_policy_runs_settle(targets);
  ASSERT_EQ(blocker.completion.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(blocker.completion.get());
  EXPECT_EQ(execution_order, (std::vector<int>{16, 16, 1, 16, 16, 1, 1, 1}));
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies interactive ordering consumes only explicit QoS deadline.
 *
 * @return Nothing.
 * @throws Standard Run, service, future, or synchronization exceptions from
 * the real one-worker execution path.
 * @note Both Runs retain full-quality HP intent. The later-published Run has
 * the earlier monotonic deadline and must run first solely because its
 * explicit service class selects the interactive policy.
 */
TEST(ExecutionServicePolicy, PrefersEarlierExplicitInteractiveDeadline) {
  constexpr ReadyTaskResourceDemand kDemand{1U, 1U, 1U, 1U};
  const auto now = std::chrono::steady_clock::now();
  ExecutionService service(1U);
  std::promise<void> blocker_entered;
  std::future<void> blocker_entered_future = blocker_entered.get_future();
  std::promise<void> release_blocker;
  const std::shared_future<void> blocker_release =
      release_blocker.get_future().share();
  std::vector<int> execution_order;
  std::mutex execution_order_mutex;
  AsyncPolicyRun blocker;
  std::vector<AsyncPolicyRun> targets;
  targets.reserve(2U);
  ScopedPromiseRelease release_guard(release_blocker);

  blocker = launch_blocking_policy_run(
      service,
      make_policy_submission("deadline-blocker", 1U, 0,
                             ComputeRunQosClass::Throughput),
      kDemand, blocker_entered, blocker_release);
  ASSERT_EQ(blocker_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission("deadline-graph", 2U, 30,
                             ComputeRunQosClass::Interactive, 1U,
                             now + std::chrono::seconds(30)),
      {30}, kDemand, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 1U));
  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission("deadline-graph", 3U, 10,
                             ComputeRunQosClass::Interactive, 1U,
                             now + std::chrono::seconds(10)),
      {10}, kDemand, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 2U));
  ASSERT_EQ(targets.size(), 2U);
  EXPECT_EQ(targets[0].run->descriptor().intent(),
            ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(targets[0].run->descriptor().quality(), ComputeRunQuality::Full);
  EXPECT_EQ(targets[1].run->descriptor().qos().service_class,
            ComputeRunQosClass::Interactive);

  EXPECT_TRUE(release_guard.release());
  expect_policy_runs_settle(targets);
  ASSERT_EQ(blocker.completion.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(blocker.completion.get());
  EXPECT_EQ(execution_order, (std::vector<int>{10, 30}));
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies a present interactive deadline precedes absent deadlines.
 *
 * @return Nothing.
 * @throws Standard Run, service, future, or synchronization exceptions from
 * the real one-worker execution path.
 * @note Two absent-deadline Runs surround a later-published present deadline.
 * The present value must win first, while absent values retain stable enqueue
 * order after the deadline candidate settles.
 */
TEST(ExecutionServicePolicy, PrefersPresentDeadlineOverAbsentDeadlines) {
  constexpr ReadyTaskResourceDemand kDemand{1U, 1U, 1U, 1U};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  ExecutionService service(1U);
  std::promise<void> blocker_entered;
  std::future<void> blocker_entered_future = blocker_entered.get_future();
  std::promise<void> release_blocker;
  const std::shared_future<void> blocker_release =
      release_blocker.get_future().share();
  std::vector<int> execution_order;
  std::mutex execution_order_mutex;
  AsyncPolicyRun blocker;
  std::vector<AsyncPolicyRun> targets;
  targets.reserve(3U);
  ScopedPromiseRelease release_guard(release_blocker);

  blocker = launch_blocking_policy_run(
      service,
      make_policy_submission("deadline-presence-blocker", 1U, 0,
                             ComputeRunQosClass::Throughput),
      kDemand, blocker_entered, blocker_release);
  ASSERT_EQ(blocker_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission("deadline-presence-graph", 2U, 30,
                             ComputeRunQosClass::Interactive),
      {30}, kDemand, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 1U));
  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission("deadline-presence-graph", 3U, 10,
                             ComputeRunQosClass::Interactive, 1U, deadline),
      {10}, kDemand, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 2U));
  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission("deadline-presence-graph", 4U, 20,
                             ComputeRunQosClass::Interactive),
      {20}, kDemand, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 3U));

  EXPECT_TRUE(release_guard.release());
  expect_policy_runs_settle(targets);
  ASSERT_EQ(blocker.completion.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(blocker.completion.get());
  EXPECT_EQ(execution_order, (std::vector<int>{10, 30, 20}));
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies dispatch-count aging advances one expensive old waiter.
 *
 * @return Nothing.
 * @throws Standard Run, service, future, or synchronization exceptions from
 * the real one-worker execution path.
 * @note A cheap dependent chain publishes each successor after its predecessor
 * starts. The expensive waiter is therefore the only candidate that reaches
 * age eight and must advance at the ninth target selection.
 */
TEST(ExecutionServicePolicy, AgesExpensiveWaiterAfterEightDispatches) {
  constexpr ReadyTaskResourceDemand kCheapDemand{1U, 1U, 1U, 1U};
  constexpr ReadyTaskResourceDemand kExpensiveDemand{1U, 1U, 1U, 1000U};
  ExecutionService service(1U);
  std::promise<void> blocker_entered;
  std::future<void> blocker_entered_future = blocker_entered.get_future();
  std::promise<void> release_blocker;
  const std::shared_future<void> blocker_release =
      release_blocker.get_future().share();
  std::vector<int> execution_order;
  std::mutex execution_order_mutex;
  AsyncPolicyRun blocker;
  std::vector<AsyncPolicyRun> targets;
  targets.reserve(2U);
  ScopedPromiseRelease release_guard(release_blocker);

  blocker = launch_blocking_policy_run(
      service,
      make_policy_submission("aging-blocker", 1U, 0,
                             ComputeRunQosClass::Throughput),
      kCheapDemand, blocker_entered, blocker_release);
  ASSERT_EQ(blocker_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission("aging-graph", 2U, 99,
                             ComputeRunQosClass::Throughput),
      {99}, kExpensiveDemand, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 1U));

  auto chain_host = std::make_unique<ExecutionServiceHost>();
  auto chain_run = std::make_unique<ComputeRun>(make_policy_submission(
      "aging-chain", 3U, 0, ComputeRunQosClass::Throughput));
  std::vector<ReadyTaskSubmission> chain_ready;
  chain_ready.push_back(make_dependent_policy_chain_submission(
      chain_run->acquire_lease(), 0U, 10U, kCheapDemand, execution_order,
      execution_order_mutex));
  ExecutionServiceHost* chain_host_pointer = chain_host.get();
  std::future<void> chain_completion =
      std::async(std::launch::async, [&service, chain_host_pointer,
                                      chain_ready = std::move(chain_ready),
                                      kCheapDemand]() mutable {
        service.execute_run(*chain_host_pointer, "cpu", std::move(chain_ready),
                            10, CpuRunResourceDemand{0U, kCheapDemand});
      });
  targets.push_back(AsyncPolicyRun{std::move(chain_host), std::move(chain_run),
                                   std::move(chain_completion)});
  ASSERT_TRUE(wait_for_ready_task_count(service, 2U));

  EXPECT_TRUE(release_guard.release());
  expect_policy_runs_settle(targets);
  ASSERT_EQ(blocker.completion.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(blocker.completion.get());
  ASSERT_EQ(execution_order.size(), 11U);
  EXPECT_EQ(execution_order[8], 99);
  EXPECT_EQ(
      std::count(execution_order.begin(), execution_order.begin() + 8, 99), 0);
  EXPECT_EQ(execution_order[9], 8);
  EXPECT_EQ(execution_order[10], 9);
  EXPECT_TRUE(wait_for_resource_reservation(service, ResourceVector{}));
}

/**
 * @brief Verifies aging overrides a same-Run stream of high-priority hints.
 *
 * @return Nothing.
 * @throws Standard Run, service, future, or synchronization exceptions from
 * the real one-worker execution path.
 * @note One normal entry is published before nine high entries in the same
 * Run. High remains the unaged hint, but after eight dispatches the older
 * normal head must advance before the ninth high entry.
 */
TEST(ExecutionServicePolicy, AgesNormalLaneThroughHighPriorityStream) {
  constexpr ReadyTaskResourceDemand kDemand{1U, 1U, 1U, 1U};
  ExecutionService service(1U);
  std::promise<void> blocker_entered;
  std::future<void> blocker_entered_future = blocker_entered.get_future();
  std::promise<void> release_blocker;
  const std::shared_future<void> blocker_release =
      release_blocker.get_future().share();
  std::vector<int> execution_order;
  std::mutex execution_order_mutex;
  AsyncPolicyRun blocker;
  std::vector<AsyncPolicyRun> targets;
  targets.reserve(1U);
  ScopedPromiseRelease release_guard(release_blocker);

  blocker = launch_blocking_policy_run(
      service,
      make_policy_submission("lane-aging-blocker", 1U, 0,
                             ComputeRunQosClass::Throughput),
      kDemand, blocker_entered, blocker_release);
  ASSERT_EQ(blocker_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);

  auto target_host = std::make_unique<ExecutionServiceHost>();
  auto target_run = std::make_unique<ComputeRun>(make_policy_submission(
      "lane-aging-graph", 2U, 99, ComputeRunQosClass::Throughput));
  std::vector<ReadyTaskSubmission> ready;
  ready.reserve(10U);
  for (std::uint64_t index = 0U; index < 10U; ++index) {
    ComputeRunLease lease = target_run->acquire_lease();
    const ComputeRunTaskIdentity identity = lease.task_identity(index);
    const bool normal_entry = index == 0U;
    const int marker = normal_entry ? 99 : 1;
    ready.emplace_back(
        std::move(lease), identity, marker, true,
        [&execution_order, &execution_order_mutex, marker](
            ComputeRunLease&, const ComputeRunTaskIdentity&,
            ExecutionTaskRuntime& runtime) {
          {
            std::lock_guard<std::mutex> lock(execution_order_mutex);
            execution_order.push_back(marker);
          }
          runtime.dec_tasks_to_complete();
        },
        normal_entry ? ExecutionTaskPriority::Normal
                     : ExecutionTaskPriority::High,
        kDemand);
  }
  ExecutionServiceHost* target_host_pointer = target_host.get();
  std::future<void> target_completion = std::async(
      std::launch::async, [&service, target_host_pointer,
                           ready = std::move(ready), kDemand]() mutable {
        service.execute_run(*target_host_pointer, "cpu", std::move(ready), 10,
                            CpuRunResourceDemand{0U, kDemand});
      });
  targets.push_back(AsyncPolicyRun{std::move(target_host),
                                   std::move(target_run),
                                   std::move(target_completion)});
  ASSERT_TRUE(wait_for_ready_task_count(service, 10U));

  EXPECT_TRUE(release_guard.release());
  expect_policy_runs_settle(targets);
  ASSERT_EQ(blocker.completion.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(blocker.completion.get());
  ASSERT_EQ(execution_order.size(), 10U);
  EXPECT_EQ(
      std::count(execution_order.begin(), execution_order.begin() + 8, 99), 0);
  EXPECT_EQ(execution_order[8], 99);
  EXPECT_EQ(execution_order[9], 1);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies throughput receives one selection after each burst of three.
 *
 * @return Nothing.
 * @throws Standard Run, service, future, or synchronization exceptions from
 * the real one-worker execution path.
 * @note Ten interactive and three throughput Runs remain queued together long
 * enough to cross the eight-dispatch aging boundary. Three complete
 * arbitration quanta prove that aged interactive work cannot bypass the
 * throughput progress slot; the final interactive then drains alone.
 */
TEST(ExecutionServicePolicy, BoundsInteractiveFloodAtThreeToOne) {
  constexpr ReadyTaskResourceDemand kDemand{1U, 1U, 1U, 1U};
  ExecutionService service(1U);
  std::promise<void> blocker_entered;
  std::future<void> blocker_entered_future = blocker_entered.get_future();
  std::promise<void> release_blocker;
  const std::shared_future<void> blocker_release =
      release_blocker.get_future().share();
  std::vector<int> execution_order;
  std::mutex execution_order_mutex;
  AsyncPolicyRun blocker;
  std::vector<AsyncPolicyRun> targets;
  targets.reserve(13U);
  ScopedPromiseRelease release_guard(release_blocker);

  blocker = launch_blocking_policy_run(
      service,
      make_policy_submission("flood-blocker", 1U, 0,
                             ComputeRunQosClass::Throughput),
      kDemand, blocker_entered, blocker_release);
  ASSERT_EQ(blocker_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  for (int index = 0; index < 10; ++index) {
    targets.push_back(launch_ordered_policy_run(
        service,
        make_policy_submission("flood-interactive",
                               static_cast<std::uint64_t>(index + 2), index,
                               ComputeRunQosClass::Interactive),
        {1}, kDemand, execution_order, execution_order_mutex));
    ASSERT_TRUE(wait_for_ready_task_count(
        service, static_cast<std::uint64_t>(index + 1)));
  }
  for (int index = 0; index < 3; ++index) {
    targets.push_back(launch_ordered_policy_run(
        service,
        make_policy_submission("flood-throughput",
                               static_cast<std::uint64_t>(index + 20), index,
                               ComputeRunQosClass::Throughput),
        {0}, kDemand, execution_order, execution_order_mutex));
    ASSERT_TRUE(wait_for_ready_task_count(
        service, static_cast<std::uint64_t>(index + 11)));
  }

  EXPECT_TRUE(release_guard.release());
  expect_policy_runs_settle(targets);
  ASSERT_EQ(blocker.completion.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(blocker.completion.get());
  EXPECT_EQ(execution_order,
            (std::vector<int>{1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1}));
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies old throughput backlog cannot steal interactive share.
 *
 * @return Nothing.
 * @throws Standard Run, service, future, or synchronization exceptions from
 * the real one-worker execution path.
 * @note Ten throughput entries are published before ten interactive entries.
 * The sequence crosses the eight-dispatch aging threshold while both classes
 * remain ready, proving class-local aging cannot override three-to-one class
 * arbitration in the reverse flood direction.
 */
TEST(ExecutionServicePolicy, BoundsOlderThroughputFloodAtThreeToOne) {
  constexpr ReadyTaskResourceDemand kDemand{1U, 1U, 1U, 1U};
  ExecutionService service(1U);
  std::promise<void> blocker_entered;
  std::future<void> blocker_entered_future = blocker_entered.get_future();
  std::promise<void> release_blocker;
  const std::shared_future<void> blocker_release =
      release_blocker.get_future().share();
  std::vector<int> execution_order;
  std::mutex execution_order_mutex;
  AsyncPolicyRun blocker;
  std::vector<AsyncPolicyRun> targets;
  targets.reserve(20U);
  ScopedPromiseRelease release_guard(release_blocker);

  blocker = launch_blocking_policy_run(
      service,
      make_policy_submission("reverse-flood-blocker", 1U, 0,
                             ComputeRunQosClass::Throughput),
      kDemand, blocker_entered, blocker_release);
  ASSERT_EQ(blocker_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  for (int index = 0; index < 10; ++index) {
    targets.push_back(launch_ordered_policy_run(
        service,
        make_policy_submission("reverse-flood-throughput",
                               static_cast<std::uint64_t>(index + 2), index,
                               ComputeRunQosClass::Throughput),
        {0}, kDemand, execution_order, execution_order_mutex));
    ASSERT_TRUE(wait_for_ready_task_count(
        service, static_cast<std::uint64_t>(index + 1)));
  }
  for (int index = 0; index < 10; ++index) {
    targets.push_back(launch_ordered_policy_run(
        service,
        make_policy_submission("reverse-flood-interactive",
                               static_cast<std::uint64_t>(index + 20), index,
                               ComputeRunQosClass::Interactive),
        {1}, kDemand, execution_order, execution_order_mutex));
    ASSERT_TRUE(wait_for_ready_task_count(
        service, static_cast<std::uint64_t>(index + 11)));
  }

  EXPECT_TRUE(release_guard.release());
  expect_policy_runs_settle(targets);
  ASSERT_EQ(blocker.completion.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(blocker.completion.get());
  EXPECT_EQ(execution_order, (std::vector<int>{1, 1, 1, 0, 1, 1, 1, 0, 1, 1,
                                               1, 0, 1, 0, 0, 0, 0, 0, 0, 0}));
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies headroom charges only active built-in Throughput Runs.
 *
 * @return Nothing.
 * @throws Standard Run, estimator, service, future, or synchronization
 * exceptions from the real two-worker execution path.
 * @note Limits are exactly two identical Run vectors and headroom is exactly
 * one. A blocked Interactive Run does not consume general quota, so one blocked
 * Throughput Run is admitted and the ledger reaches its exact full vector.
 * After Interactive releases, another Throughput Run is still rejected by the
 * class quota even though physical capacity is available; a legacy owner may
 * use that full-ledger capacity. Final release clears ledger and class charges.
 */
TEST(ExecutionServicePolicy, ProtectsInteractiveAdmissionHeadroom) {
  constexpr ReadyTaskResourceDemand kDemand{1U, 1U, 1U, 1U};
  const std::string graph_identity = "headroom-graph";
  ExecutionService probe(1U);
  std::atomic_int probe_entered{0};
  ComputeRun probe_run(make_policy_submission(graph_identity, 1U, 1,
                                              ComputeRunQosClass::Throughput));
  ReadyTaskSubmission representative = make_counted_ready_submission(
      probe_run.acquire_lease(), 0U, 1, probe_entered,
      ExecutionTaskPriority::Normal, kDemand);
  const ResourceVector required = probe.estimate_cpu_run_resources(
      representative, 1, CpuRunResourceDemand{0U, kDemand});
  const std::optional<ResourceVector> doubled =
      checked_multiply_resources(required, 2U);
  ASSERT_TRUE(doubled.has_value());
  ExecutionResourceLimits limits = execution_limits(*doubled);
  limits.interactive_headroom = required;
  ExecutionService service(2U, limits);

  std::promise<void> interactive_entered;
  std::future<void> interactive_entered_future =
      interactive_entered.get_future();
  std::promise<void> release_interactive;
  const std::shared_future<void> interactive_release =
      release_interactive.get_future().share();
  std::promise<void> throughput_entered;
  std::future<void> throughput_entered_future = throughput_entered.get_future();
  std::promise<void> release_throughput;
  const std::shared_future<void> throughput_release =
      release_throughput.get_future().share();
  std::vector<int> execution_order;
  std::mutex execution_order_mutex;
  AsyncPolicyRun interactive;
  AsyncPolicyRun throughput;
  std::vector<AsyncPolicyRun> targets;
  targets.reserve(1U);
  ScopedPromiseRelease interactive_release_guard(release_interactive);
  ScopedPromiseRelease throughput_release_guard(release_throughput);
  interactive = launch_blocking_policy_run(
      service,
      make_policy_submission(graph_identity, 2U, 2,
                             ComputeRunQosClass::Interactive),
      kDemand, interactive_entered, interactive_release);
  ASSERT_EQ(interactive_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(service.resource_snapshot().reserved, required);
  EXPECT_EQ(
      testing::ExecutionServiceTestAccess::throughput_reservation_snapshot(
          service),
      ResourceVector{});

  throughput = launch_blocking_policy_run(
      service,
      make_policy_submission(graph_identity, 3U, 3,
                             ComputeRunQosClass::Throughput),
      kDemand, throughput_entered, throughput_release);
  ASSERT_EQ(throughput_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(service.resource_snapshot().reserved, *doubled);
  EXPECT_EQ(
      testing::ExecutionServiceTestAccess::throughput_reservation_snapshot(
          service),
      required);

  EXPECT_TRUE(interactive_release_guard.release());
  ASSERT_EQ(interactive.completion.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(interactive.completion.get());
  EXPECT_TRUE(wait_for_resource_reservation(service, required));
  EXPECT_EQ(service.resource_snapshot().reserved, required);
  EXPECT_EQ(
      testing::ExecutionServiceTestAccess::throughput_reservation_snapshot(
          service),
      required);

  std::atomic_int rejected_entered{0};
  ComputeRun rejected_run(make_policy_submission(
      graph_identity, 4U, 4, ComputeRunQosClass::Throughput));
  std::vector<ReadyTaskSubmission> rejected_ready;
  rejected_ready.push_back(make_counted_ready_submission(
      rejected_run.acquire_lease(), 0U, 4, rejected_entered,
      ExecutionTaskPriority::Normal, kDemand));
  try {
    ExecutionServiceHost rejected_host;
    service.execute_run(rejected_host, "cpu", std::move(rejected_ready), 1,
                        CpuRunResourceDemand{0U, kDemand});
    FAIL() << "Expected throughput headroom rejection.";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::ComputeError);
  }
  EXPECT_EQ(rejected_entered.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(service.resource_snapshot().reserved, required);
  EXPECT_EQ(
      testing::ExecutionServiceTestAccess::throughput_reservation_snapshot(
          service),
      required);

  EXPECT_TRUE(throughput_release_guard.release());
  ASSERT_EQ(throughput.completion.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(throughput.completion.get());
  EXPECT_TRUE(wait_for_resource_reservation(service, ResourceVector{}));
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
  EXPECT_EQ(
      testing::ExecutionServiceTestAccess::throughput_reservation_snapshot(
          service),
      ResourceVector{});

  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission(graph_identity, 5U, 5,
                             ComputeRunQosClass::Throughput),
      {5}, kDemand, execution_order, execution_order_mutex));
  expect_policy_runs_settle(targets);
  EXPECT_EQ(execution_order, (std::vector<int>{5}));
  EXPECT_TRUE(wait_for_resource_reservation(service, ResourceVector{}));
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
  EXPECT_EQ(
      testing::ExecutionServiceTestAccess::throughput_reservation_snapshot(
          service),
      ResourceVector{});
}

/**
 * @brief Verifies concurrent Throughput admissions share one atomic quota.
 *
 * @return Nothing.
 * @throws Standard Run, estimator, service, future, or synchronization
 * exceptions from two admission contenders and two real workers.
 * @note Physical limits hold two identical Runs while general quota holds one.
 * Releasing a shared start gate must therefore yield exactly one callback and
 * one admission rejection; deleting or splitting check/reserve/charge admits
 * both contenders into physical headroom.
 */
TEST(ExecutionServicePolicy, SerializesConcurrentThroughputQuotaAdmission) {
  constexpr ReadyTaskResourceDemand kDemand{1U, 1U, 1U, 1U};
  const std::string graph_identity = "concurrent-throughput";
  ExecutionService probe(1U);
  std::atomic_int probe_entered{0};
  ComputeRun probe_run(make_policy_submission(graph_identity, 1U, 1,
                                              ComputeRunQosClass::Throughput));
  ReadyTaskSubmission representative = make_counted_ready_submission(
      probe_run.acquire_lease(), 0U, 1, probe_entered,
      ExecutionTaskPriority::Normal, kDemand);
  const ResourceVector required = probe.estimate_cpu_run_resources(
      representative, 1, CpuRunResourceDemand{0U, kDemand});
  const std::optional<ResourceVector> doubled =
      checked_add_resources(required, required);
  ASSERT_TRUE(doubled.has_value());
  ExecutionResourceLimits limits = execution_limits(*doubled);
  limits.interactive_headroom = required;
  ExecutionService service(2U, limits);

  std::promise<void> admission_start;
  const std::shared_future<void> admission_start_gate =
      admission_start.get_future().share();
  std::promise<void> callback_release;
  const std::shared_future<void> callback_release_gate =
      callback_release.get_future().share();
  std::atomic_int entered{0};
  std::vector<AsyncPolicyRun> contenders;
  contenders.reserve(2U);
  ScopedPromiseRelease admission_start_guard(admission_start);
  ScopedPromiseRelease callback_release_guard(callback_release);

  contenders.push_back(launch_concurrent_blocking_policy_run(
      service,
      make_policy_submission(graph_identity, 2U, 2,
                             ComputeRunQosClass::Throughput),
      kDemand, admission_start_gate, entered, callback_release_gate));
  contenders.push_back(launch_concurrent_blocking_policy_run(
      service,
      make_policy_submission(graph_identity, 3U, 3,
                             ComputeRunQosClass::Throughput),
      kDemand, admission_start_gate, entered, callback_release_gate));
  EXPECT_TRUE(admission_start_guard.release());

  std::size_t settled_while_blocked = 0U;
  const auto observation_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  do {
    settled_while_blocked = 0U;
    for (AsyncPolicyRun& contender : contenders) {
      if (contender.completion.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready) {
        ++settled_while_blocked;
      }
    }
    if (entered.load(std::memory_order_acquire) >= 2 ||
        (entered.load(std::memory_order_acquire) == 1 &&
         settled_while_blocked == 1U)) {
      break;
    }
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < observation_deadline);

  EXPECT_EQ(entered.load(std::memory_order_acquire), 1);
  EXPECT_EQ(settled_while_blocked, 1U);
  EXPECT_EQ(service.resource_snapshot().reserved, required);
  EXPECT_EQ(
      testing::ExecutionServiceTestAccess::throughput_reservation_snapshot(
          service),
      required);

  EXPECT_TRUE(callback_release_guard.release());
  int successful = 0;
  int rejected = 0;
  for (AsyncPolicyRun& contender : contenders) {
    ASSERT_EQ(contender.completion.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    try {
      contender.completion.get();
      ++successful;
    } catch (const GraphError& error) {
      EXPECT_EQ(error.code(), GraphErrc::ComputeError);
      ++rejected;
    }
  }
  EXPECT_EQ(successful, 1);
  EXPECT_EQ(rejected, 1);
  EXPECT_TRUE(wait_for_resource_reservation(service, ResourceVector{}));
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
  EXPECT_EQ(
      testing::ExecutionServiceTestAccess::throughput_reservation_snapshot(
          service),
      ResourceVector{});
}

/**
 * @brief Verifies dependent re-entry retains the Run's charged policy history.
 *
 * @return Nothing.
 * @throws Standard Run, service, callback, future, or synchronization
 * exceptions from the real one-worker execution path.
 * @note A two-entry peer is published before a chain Run. After peer-1 and the
 * chain initial task each receive equal service, the peer's older second entry
 * must win the tie over the newly published dependent. Resetting the chain's
 * empty policy row would incorrectly select the dependent first.
 */
TEST(ExecutionServicePolicy, RetainsPolicyHistoryAcrossDependentReentry) {
  constexpr ReadyTaskResourceDemand kDemand{1U, 1U, 1U, 1U};
  ExecutionService service(1U);
  std::promise<void> blocker_entered;
  std::future<void> blocker_entered_future = blocker_entered.get_future();
  std::promise<void> release_blocker;
  const std::shared_future<void> blocker_release =
      release_blocker.get_future().share();
  std::vector<int> execution_order;
  std::mutex execution_order_mutex;
  AsyncPolicyRun blocker;
  std::vector<AsyncPolicyRun> targets;
  targets.reserve(2U);
  ScopedPromiseRelease release_guard(release_blocker);

  blocker = launch_blocking_policy_run(
      service,
      make_policy_submission("reentry-blocker", 1U, 0,
                             ComputeRunQosClass::Throughput),
      kDemand, blocker_entered, blocker_release);
  ASSERT_EQ(blocker_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission("reentry-graph", 2U, 20,
                             ComputeRunQosClass::Throughput),
      {20, 21}, kDemand, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 2U));

  auto chain_host = std::make_unique<ExecutionServiceHost>();
  auto chain_run = std::make_unique<ComputeRun>(make_policy_submission(
      "reentry-graph", 3U, 10, ComputeRunQosClass::Throughput));
  ComputeRunLease chain_lease = chain_run->acquire_lease();
  const ComputeRunTaskIdentity chain_identity = chain_lease.task_identity(0U);
  std::vector<ReadyTaskSubmission> chain_ready;
  chain_ready.emplace_back(
      std::move(chain_lease), chain_identity, 10, true,
      [&execution_order, &execution_order_mutex, kDemand](
          ComputeRunLease& retained_lease, const ComputeRunTaskIdentity&,
          ExecutionTaskRuntime& runtime) {
        {
          std::lock_guard<std::mutex> lock(execution_order_mutex);
          execution_order.push_back(10);
        }
        auto& ready_runtime =
            dynamic_cast<ReadyTaskSubmissionRuntime&>(runtime);
        const ComputeRunTaskIdentity dependent_identity =
            retained_lease.task_identity(1U);
        ready_runtime.submit_ready_submission(ReadyTaskSubmission(
            retained_lease, dependent_identity, 11, false,
            [&execution_order, &execution_order_mutex](
                ComputeRunLease&, const ComputeRunTaskIdentity&,
                ExecutionTaskRuntime& dependent_runtime) {
              {
                std::lock_guard<std::mutex> lock(execution_order_mutex);
                execution_order.push_back(11);
              }
              dependent_runtime.dec_tasks_to_complete();
            },
            ExecutionTaskPriority::Normal, kDemand));
        runtime.dec_tasks_to_complete();
      },
      ExecutionTaskPriority::Normal, kDemand);
  ExecutionServiceHost* chain_host_pointer = chain_host.get();
  std::future<void> chain_completion =
      std::async(std::launch::async, [&service, chain_host_pointer,
                                      chain_ready = std::move(chain_ready),
                                      kDemand]() mutable {
        service.execute_run(*chain_host_pointer, "cpu", std::move(chain_ready),
                            2, CpuRunResourceDemand{0U, kDemand});
      });
  targets.push_back(AsyncPolicyRun{std::move(chain_host), std::move(chain_run),
                                   std::move(chain_completion)});
  ASSERT_TRUE(wait_for_ready_task_count(service, 3U));

  EXPECT_TRUE(release_guard.release());
  expect_policy_runs_settle(targets);
  ASSERT_EQ(blocker.completion.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(blocker.completion.get());
  EXPECT_EQ(execution_order, (std::vector<int>{20, 10, 21, 11}));
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies the Graph policy counter saturates at UINT64_MAX.
 *
 * @return Nothing.
 * @throws Standard Run, estimator, service, future, or synchronization
 * exceptions from the real one-worker execution path.
 * @note A maximum-cost Graph is charged while its sole worker callback
 * blocks. A cheap sibling Run keeps that Graph row alive beside a fresh peer
 * Graph. Graph-score saturation selects the peer before Run scores can decide;
 * Graph-score wrapping would make the maximum-cost Graph appear cheapest.
 */
TEST(ExecutionServicePolicy, SaturatesGraphCounterWithoutWrapping) {
  constexpr ReadyTaskResourceDemand kBaseDemand{1U, 1U, 0U, 1U};
  constexpr ReadyTaskResourceDemand kCheapDemand{1U, 1U, 0U, 1U};
  constexpr ReadyTaskResourceDemand kPeerDemand{1U, 1U, 0U, 2U};
  const std::string saturated_graph = "saturated-a";
  ExecutionService service(1U);

  std::atomic_int representative_entered{0};
  ComputeRun representative_run(make_policy_submission(
      saturated_graph, 1U, 0, ComputeRunQosClass::Throughput));
  ReadyTaskSubmission representative = make_counted_ready_submission(
      representative_run.acquire_lease(), 0U, 0, representative_entered,
      ExecutionTaskPriority::Normal, kBaseDemand);
  const ResourceVector base_resources = service.estimate_cpu_run_resources(
      representative, 1, CpuRunResourceDemand{0U, kBaseDemand});
  ASSERT_GT(base_resources.ready_bytes, 0U);
  const std::uint64_t byte_quanta =
      base_resources.ready_bytes / 4096U +
      (base_resources.ready_bytes % 4096U == 0U ? 0U : 1U);
  ASSERT_GT(byte_quanta, 0U);
  ASSERT_LT(byte_quanta, std::numeric_limits<std::uint64_t>::max());
  const ReadyTaskResourceDemand maximum_cost_demand{
      1U, 1U, 0U, std::numeric_limits<std::uint64_t>::max() - byte_quanta};

  std::promise<void> blocker_entered;
  std::future<void> blocker_entered_future = blocker_entered.get_future();
  std::promise<void> release_blocker;
  const std::shared_future<void> blocker_release =
      release_blocker.get_future().share();
  std::vector<int> execution_order;
  std::mutex execution_order_mutex;
  AsyncPolicyRun blocker;
  std::vector<AsyncPolicyRun> targets;
  targets.reserve(2U);
  ScopedPromiseRelease release_guard(release_blocker);

  blocker = launch_blocking_policy_run(
      service,
      make_policy_submission(saturated_graph, 2U, 0,
                             ComputeRunQosClass::Throughput),
      maximum_cost_demand, blocker_entered, blocker_release);
  ASSERT_EQ(blocker_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission(saturated_graph, 3U, 1,
                             ComputeRunQosClass::Throughput),
      {1}, kCheapDemand, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 1U));
  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission("saturated-b", 4U, 2,
                             ComputeRunQosClass::Throughput),
      {2}, kPeerDemand, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 2U));

  EXPECT_TRUE(release_guard.release());
  expect_policy_runs_settle(targets);
  ASSERT_EQ(blocker.completion.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(blocker.completion.get());
  EXPECT_EQ(execution_order, (std::vector<int>{2, 1}));
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies Run policy saturation across dependent ready re-entry.
 *
 * @return Nothing.
 * @throws Standard Run, estimator, service, callback, future, or
 * synchronization exceptions from the real one-worker execution path.
 * @note One Run first consumes the maximum representable policy cost. While
 * that callback occupies the worker, an equal-cost sibling in the same Graph
 * is published before the original Run's equal-cost dependent. Their next
 * Graph scores are therefore identical. Correct Run saturation leaves equal
 * Run scores and selects the older sibling; Run-score wrapping makes the
 * re-entering Run appear cheaper and reverses the deterministic order.
 */
TEST(ExecutionServicePolicy, SaturatesRunCounterAcrossDependentReentry) {
  constexpr ReadyTaskResourceDemand kBaseDemand{1U, 1U, 0U, 1U};
  const std::string graph_identity = "run-saturated-graph";
  ExecutionService service(1U);

  std::atomic_int representative_entered{0};
  ComputeRun representative_run(make_policy_submission(
      graph_identity, 1U, 0, ComputeRunQosClass::Throughput));
  ReadyTaskSubmission representative = make_counted_ready_submission(
      representative_run.acquire_lease(), 0U, 0, representative_entered,
      ExecutionTaskPriority::Normal, kBaseDemand);
  const ResourceVector base_resources = service.estimate_cpu_run_resources(
      representative, 1, CpuRunResourceDemand{0U, kBaseDemand});
  ASSERT_GT(base_resources.ready_bytes, 0U);
  const std::uint64_t byte_quanta =
      base_resources.ready_bytes / 4096U +
      (base_resources.ready_bytes % 4096U == 0U ? 0U : 1U);
  ASSERT_GT(byte_quanta, 0U);
  ASSERT_LT(byte_quanta, std::numeric_limits<std::uint64_t>::max());
  const ReadyTaskResourceDemand maximum_cost_demand{
      1U, 1U, 0U, std::numeric_limits<std::uint64_t>::max() - byte_quanta};

  std::promise<void> saturated_entered;
  std::future<void> saturated_entered_future = saturated_entered.get_future();
  std::promise<void> publish_dependent;
  const std::shared_future<void> publish_dependent_gate =
      publish_dependent.get_future().share();
  std::promise<void> dependent_published;
  std::future<void> dependent_published_future =
      dependent_published.get_future();
  std::promise<void> release_saturated_initial;
  const std::shared_future<void> saturated_initial_release =
      release_saturated_initial.get_future().share();
  std::vector<int> execution_order;
  std::mutex execution_order_mutex;
  AsyncPolicyRun saturated;
  std::vector<AsyncPolicyRun> targets;
  targets.reserve(1U);
  ScopedPromiseRelease publish_dependent_guard(publish_dependent);
  ScopedPromiseRelease release_saturated_guard(release_saturated_initial);

  auto saturated_host = std::make_unique<ExecutionServiceHost>();
  auto saturated_run = std::make_unique<ComputeRun>(make_policy_submission(
      graph_identity, 2U, 1, ComputeRunQosClass::Throughput));
  ComputeRunLease initial_lease = saturated_run->acquire_lease();
  const ComputeRunTaskIdentity initial_identity =
      initial_lease.task_identity(0U);
  std::vector<ReadyTaskSubmission> saturated_ready;
  saturated_ready.emplace_back(
      std::move(initial_lease), initial_identity, 1, true,
      [&saturated_entered, publish_dependent_gate, &dependent_published,
       saturated_initial_release, &execution_order, &execution_order_mutex,
       maximum_cost_demand](ComputeRunLease& retained_lease,
                            const ComputeRunTaskIdentity&,
                            ExecutionTaskRuntime& runtime) {
        saturated_entered.set_value();
        publish_dependent_gate.wait();
        auto& ready_runtime =
            dynamic_cast<ReadyTaskSubmissionRuntime&>(runtime);
        const ComputeRunTaskIdentity dependent_identity =
            retained_lease.task_identity(1U);
        ready_runtime.submit_ready_submission(ReadyTaskSubmission(
            retained_lease, dependent_identity, 1, false,
            [&execution_order, &execution_order_mutex](
                ComputeRunLease&, const ComputeRunTaskIdentity&,
                ExecutionTaskRuntime& dependent_runtime) {
              {
                std::lock_guard<std::mutex> lock(execution_order_mutex);
                execution_order.push_back(1);
              }
              dependent_runtime.dec_tasks_to_complete();
            },
            ExecutionTaskPriority::Normal, maximum_cost_demand));
        dependent_published.set_value();
        saturated_initial_release.wait();
        runtime.dec_tasks_to_complete();
      },
      ExecutionTaskPriority::Normal, maximum_cost_demand);
  ExecutionServiceHost* saturated_host_pointer = saturated_host.get();
  std::future<void> saturated_completion = std::async(
      std::launch::async, [&service, saturated_host_pointer,
                           saturated_ready = std::move(saturated_ready),
                           maximum_cost_demand]() mutable {
        service.execute_run(*saturated_host_pointer, "cpu",
                            std::move(saturated_ready), 2,
                            CpuRunResourceDemand{0U, maximum_cost_demand});
      });
  saturated =
      AsyncPolicyRun{std::move(saturated_host), std::move(saturated_run),
                     std::move(saturated_completion)};
  ASSERT_EQ(saturated_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);

  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission(graph_identity, 3U, 2,
                             ComputeRunQosClass::Throughput),
      {2}, maximum_cost_demand, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 1U));
  EXPECT_TRUE(publish_dependent_guard.release());
  ASSERT_EQ(dependent_published_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  ASSERT_TRUE(wait_for_ready_task_count(service, 2U));

  EXPECT_TRUE(release_saturated_guard.release());
  expect_policy_runs_settle(targets);
  ASSERT_EQ(saturated.completion.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(saturated.completion.get());
  EXPECT_EQ(execution_order, (std::vector<int>{2, 1}));
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies checked policy-cost overflow unwinds beside an active peer.
 *
 * @return Nothing.
 * @throws Standard Run, service, future, or synchronization exceptions from
 * the real one-worker execution path.
 * @note Maximum work plus the mandatory positive ready-byte quantum cannot be
 * represented. The staged grant and root reservation must unwind without
 * changing the blocked peer, after which an ordinary recovery Run executes.
 */
TEST(ExecutionServicePolicy, UnwindsPolicyCostOverflowWithoutAffectingPeer) {
  constexpr ReadyTaskResourceDemand kDemand{1U, 1U, 1U, 1U};
  constexpr ReadyTaskResourceDemand kOverflowDemand{
      1U, 1U, 1U, std::numeric_limits<std::uint64_t>::max()};
  ExecutionService service(1U);
  std::promise<void> blocker_entered;
  std::future<void> blocker_entered_future = blocker_entered.get_future();
  std::promise<void> release_blocker;
  const std::shared_future<void> blocker_release =
      release_blocker.get_future().share();
  std::vector<int> execution_order;
  std::mutex execution_order_mutex;
  AsyncPolicyRun blocker;
  std::vector<AsyncPolicyRun> targets;
  targets.reserve(1U);
  ScopedPromiseRelease release_guard(release_blocker);

  blocker = launch_blocking_policy_run(
      service,
      make_policy_submission("overflow-peer", 1U, 1,
                             ComputeRunQosClass::Throughput),
      kDemand, blocker_entered, blocker_release);
  ASSERT_EQ(blocker_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  const ResourceVector peer_resources = service.resource_snapshot().reserved;
  ASSERT_NE(peer_resources, ResourceVector{});

  std::atomic_int overflow_entered{0};
  ComputeRun overflow_run(make_policy_submission(
      "overflow-policy", 2U, 2, ComputeRunQosClass::Throughput));
  std::vector<ReadyTaskSubmission> overflow_ready;
  overflow_ready.push_back(make_counted_ready_submission(
      overflow_run.acquire_lease(), 0U, 2, overflow_entered,
      ExecutionTaskPriority::Normal, kOverflowDemand));
  try {
    ExecutionServiceHost overflow_host;
    service.execute_run(overflow_host, "cpu", std::move(overflow_ready), 1,
                        CpuRunResourceDemand{0U, kOverflowDemand});
    FAIL() << "Expected checked policy service-cost overflow.";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::ComputeError);
  }
  EXPECT_EQ(overflow_entered.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(service.resource_snapshot().reserved, peer_resources);
  EXPECT_NE(service.get_stats().find(", Ready tasks: 0,"), std::string::npos);

  targets.push_back(launch_ordered_policy_run(
      service,
      make_policy_submission("overflow-recovery", 3U, 3,
                             ComputeRunQosClass::Throughput),
      {3}, kDemand, execution_order, execution_order_mutex));
  ASSERT_TRUE(wait_for_ready_task_count(service, 1U));
  EXPECT_TRUE(release_guard.release());
  expect_policy_runs_settle(targets);
  ASSERT_EQ(blocker.completion.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(blocker.completion.get());
  EXPECT_EQ(execution_order, (std::vector<int>{3}));
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies independent Run batches overlap on one fixed service pool.
 *
 * @return Nothing.
 * @throws std::bad_alloc when Run, callback, future, or trace storage cannot
 * allocate.
 * @throws std::future_error when test synchronization state is invalid.
 * @throws std::system_error when async launch or synchronization fails.
 * @note The first callback blocks after entering its worker. The second caller
 * signals immediately before calling `execute_run()`. With two fixed
 * service workers, the second Run must enter and settle while the first Run
 * remains blocked, proving there is no whole-service Run gate. Both Runs reuse
 * local task id zero; each Host must observe only its matching Run/node/epoch.
 * A scope guard releases the first callback before async-future destruction on
 * every assertion path, so a serialization regression fails instead of
 * hanging this test process.
 */
TEST(ExecutionService, OverlapsIndependentConcurrentRunIntervals) {
  ExecutionService service(2);
  ExecutionServiceHost first_host;
  ExecutionServiceHost second_host;
  ComputeRun first(make_test_submission("service-gate-first", 53, 63));
  ComputeRun second(make_test_submission("service-gate-second", 54, 64));
  std::promise<void> release_first;
  const std::shared_future<void> first_release =
      release_first.get_future().share();
  std::promise<void> first_entered;
  std::atomic_bool second_entered{false};

  std::vector<ReadyTaskSubmission> first_ready;
  ComputeRunLease first_lease = first.acquire_lease();
  const ComputeRunTaskIdentity first_identity = first_lease.task_identity(0);
  first_ready.emplace_back(std::move(first_lease), first_identity, 63, true,
                           [&first_entered, first_release](
                               ComputeRunLease&, const ComputeRunTaskIdentity&,
                               ExecutionTaskRuntime& runtime) {
                             first_entered.set_value();
                             first_release.wait();
                             runtime.dec_tasks_to_complete();
                           });

  std::vector<ReadyTaskSubmission> second_ready;
  ComputeRunLease second_lease = second.acquire_lease();
  const ComputeRunTaskIdentity second_identity = second_lease.task_identity(0);
  EXPECT_EQ(first_identity.local_task_id().value(),
            second_identity.local_task_id().value());
  EXPECT_NE(first_identity.run_id(), second_identity.run_id());
  second_ready.emplace_back(
      std::move(second_lease), second_identity, 64, true,
      [&second_entered](ComputeRunLease&, const ComputeRunTaskIdentity&,
                        ExecutionTaskRuntime& runtime) {
        second_entered.store(true, std::memory_order_release);
        runtime.dec_tasks_to_complete();
      });

  std::future<void> first_future;
  std::future<void> second_future;
  ScopedPromiseRelease release_guard(release_first);
  first_future = std::async(
      std::launch::async,
      [&service, &first_host, ready = std::move(first_ready)]() mutable {
        service.execute_run(first_host, "cpu", std::move(ready), 1);
      });
  ASSERT_EQ(first_entered.get_future().wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  std::promise<void> second_call_started;
  std::future<void> second_call_started_future =
      second_call_started.get_future();
  second_future = std::async(
      std::launch::async, [&service, &second_host, &second_call_started,
                           ready = std::move(second_ready)]() mutable {
        second_call_started.set_value();
        service.execute_run(second_host, "cpu", std::move(ready), 1);
      });

  const std::future_status second_call_status =
      second_call_started_future.wait_for(std::chrono::seconds(2));
  EXPECT_EQ(second_call_status, std::future_status::ready);
  const std::future_status overlapping_completion_status =
      second_future.wait_for(std::chrono::seconds(2));
  EXPECT_EQ(overlapping_completion_status, std::future_status::ready);
  if (overlapping_completion_status == std::future_status::ready) {
    EXPECT_NO_THROW(second_future.get());
  }
  EXPECT_TRUE(second_entered.load(std::memory_order_acquire));

  EXPECT_TRUE(release_guard.release());
  if (second_future.valid()) {
    const std::future_status cleanup_status =
        second_future.wait_for(std::chrono::seconds(2));
    EXPECT_EQ(cleanup_status, std::future_status::ready);
    if (cleanup_status == std::future_status::ready) {
      EXPECT_NO_THROW(second_future.get());
    }
  }
  const std::future_status first_cleanup_status =
      first_future.wait_for(std::chrono::seconds(2));
  EXPECT_EQ(first_cleanup_status, std::future_status::ready);
  if (first_cleanup_status == std::future_status::ready) {
    EXPECT_NO_THROW(first_future.get());
  }
  EXPECT_EQ(first_host.context_entries(), first_host.context_exits());
  EXPECT_EQ(second_host.context_entries(), second_host.context_exits());
  EXPECT_FALSE(first_host.trace_recording_failed());
  EXPECT_FALSE(second_host.trace_recording_failed());
  const std::vector<ExecutionServiceHost::TraceEvent> first_traces =
      first_host.trace_events();
  const std::vector<ExecutionServiceHost::TraceEvent> second_traces =
      second_host.trace_events();
  ASSERT_EQ(first_traces.size(), 1U);
  ASSERT_EQ(second_traces.size(), 1U);
  EXPECT_EQ(first_traces.front().action, ExecutionTraceAction::AssignInitial);
  EXPECT_EQ(first_traces.front().node_id, 63);
  EXPECT_GE(first_traces.front().worker_id, 0);
  EXPECT_LT(first_traces.front().worker_id, 2);
  EXPECT_EQ(first_traces.front().epoch, first.descriptor().id().value());
  EXPECT_NE(first_traces.front().epoch, second.descriptor().id().value());
  EXPECT_EQ(second_traces.front().action, ExecutionTraceAction::AssignInitial);
  EXPECT_EQ(second_traces.front().node_id, 64);
  EXPECT_GE(second_traces.front().worker_id, 0);
  EXPECT_LT(second_traces.front().worker_id, 2);
  EXPECT_EQ(second_traces.front().epoch, second.descriptor().id().value());
  EXPECT_NE(second_traces.front().epoch, first.descriptor().id().value());
}

/**
 * @brief Maps realtime HP and RT child identity to distinct epochs on one Host.
 *
 * @return Nothing.
 * @throws std::bad_alloc when Run, callback, future, observation, or trace
 * storage cannot allocate.
 * @throws std::future_error when test synchronization state is invalid.
 * @throws std::system_error when async launch or synchronization fails.
 * @note Both children share graph identity, revision, target, Host, and local
 * task id zero. Distinct trace-node markers correlate each Host event with the
 * callback's retained lease and complete task identity, so swapping HP/RT
 * epochs cannot pass as an unordered multiset. This direct service seam
 * observes the RunState host/epoch selected by the worker loop; the
 * GraphRuntime realtime coordinator does not expose retained callback identity
 * without adding a test-only product hook.
 */
TEST(ExecutionService, DistinguishesRealtimeHpAndRtChildEpochsOnOneHost) {
  constexpr int kHpTraceNodeId = 65;
  constexpr int kRtTraceNodeId = 66;
  ExecutionService service(2);
  ExecutionServiceHost host;
  ComputeRunSubmission hp_submission =
      make_test_submission("realtime-siblings", 55, kHpTraceNodeId);
  ComputeRunSubmission rt_submission = hp_submission;
  rt_submission.intent = ComputeIntent::RealTimeUpdate;
  rt_submission.quality = ComputeRunQuality::Interactive;
  ComputeRun hp_child(std::move(hp_submission));
  ComputeRun rt_child(std::move(rt_submission));
  EXPECT_EQ(hp_child.descriptor().graph_identity(),
            rt_child.descriptor().graph_identity());
  EXPECT_EQ(hp_child.descriptor().graph_instance_id(),
            rt_child.descriptor().graph_instance_id());
  EXPECT_EQ(hp_child.descriptor().revision(), rt_child.descriptor().revision());
  EXPECT_EQ(hp_child.descriptor().target_node_id(),
            rt_child.descriptor().target_node_id());
  EXPECT_EQ(hp_child.descriptor().intent(), ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(hp_child.descriptor().quality(), ComputeRunQuality::Full);
  EXPECT_EQ(rt_child.descriptor().intent(), ComputeIntent::RealTimeUpdate);
  EXPECT_EQ(rt_child.descriptor().quality(), ComputeRunQuality::Interactive);
  EXPECT_NE(hp_child.descriptor().id(), rt_child.descriptor().id());

  std::promise<void> release_children;
  const std::shared_future<void> release =
      release_children.get_future().share();
  std::promise<void> both_entered;
  std::future<void> both_entered_future = both_entered.get_future();
  std::atomic_int active_children{0};
  std::promise<RetainedSubmissionObservation> hp_observation_promise;
  std::future<RetainedSubmissionObservation> hp_observation_future =
      hp_observation_promise.get_future();
  std::promise<RetainedSubmissionObservation> rt_observation_promise;
  std::future<RetainedSubmissionObservation> rt_observation_future =
      rt_observation_promise.get_future();

  ComputeRunLease hp_lease = hp_child.acquire_lease();
  const ComputeRunTaskIdentity hp_identity = hp_lease.task_identity(0);
  std::vector<ReadyTaskSubmission> hp_ready;
  hp_ready.emplace_back(
      std::move(hp_lease), hp_identity, kHpTraceNodeId, true,
      [&active_children, &both_entered, release, &hp_observation_promise](
          ComputeRunLease& retained_lease,
          const ComputeRunTaskIdentity& retained_identity,
          ExecutionTaskRuntime& runtime) {
        hp_observation_promise.set_value(observe_retained_submission(
            kHpTraceNodeId, retained_lease, retained_identity));
        const int active =
            active_children.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (active == 2) {
          both_entered.set_value();
        }
        release.wait();
        active_children.fetch_sub(1, std::memory_order_acq_rel);
        runtime.dec_tasks_to_complete();
      });
  ComputeRunLease rt_lease = rt_child.acquire_lease();
  const ComputeRunTaskIdentity rt_identity = rt_lease.task_identity(0);
  EXPECT_EQ(hp_identity.local_task_id().value(),
            rt_identity.local_task_id().value());
  EXPECT_NE(hp_identity.run_id(), rt_identity.run_id());
  std::vector<ReadyTaskSubmission> rt_ready;
  rt_ready.emplace_back(
      std::move(rt_lease), rt_identity, kRtTraceNodeId, true,
      [&active_children, &both_entered, release, &rt_observation_promise](
          ComputeRunLease& retained_lease,
          const ComputeRunTaskIdentity& retained_identity,
          ExecutionTaskRuntime& runtime) {
        rt_observation_promise.set_value(observe_retained_submission(
            kRtTraceNodeId, retained_lease, retained_identity));
        const int active =
            active_children.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (active == 2) {
          both_entered.set_value();
        }
        release.wait();
        active_children.fetch_sub(1, std::memory_order_acq_rel);
        runtime.dec_tasks_to_complete();
      });

  std::future<void> hp_future;
  std::future<void> rt_future;
  ScopedPromiseRelease release_guard(release_children);
  hp_future =
      std::async(std::launch::async,
                 [&service, &host, ready = std::move(hp_ready)]() mutable {
                   service.execute_run(host, "cpu", std::move(ready), 1);
                 });
  rt_future =
      std::async(std::launch::async,
                 [&service, &host, ready = std::move(rt_ready)]() mutable {
                   service.execute_run(host, "cpu", std::move(ready), 1);
                 });

  const std::future_status both_entered_status =
      both_entered_future.wait_for(std::chrono::seconds(2));
  EXPECT_EQ(both_entered_status, std::future_status::ready);
  EXPECT_TRUE(release_guard.release());
  EXPECT_NO_THROW(hp_future.get());
  EXPECT_NO_THROW(rt_future.get());
  EXPECT_EQ(active_children.load(std::memory_order_acquire), 0);
  EXPECT_EQ(host.context_entries(), 2);
  EXPECT_EQ(host.context_exits(), 2);
  EXPECT_FALSE(host.trace_recording_failed());

  ASSERT_EQ(hp_observation_future.wait_for(std::chrono::seconds(0)),
            std::future_status::ready);
  ASSERT_EQ(rt_observation_future.wait_for(std::chrono::seconds(0)),
            std::future_status::ready);
  const RetainedSubmissionObservation hp_observation =
      hp_observation_future.get();
  const RetainedSubmissionObservation rt_observation =
      rt_observation_future.get();

  EXPECT_EQ(hp_observation.trace_node_id, kHpTraceNodeId);
  EXPECT_EQ(hp_observation.descriptor_run_id,
            hp_child.descriptor().id().value());
  EXPECT_EQ(hp_observation.graph_identity,
            hp_child.descriptor().graph_identity());
  EXPECT_EQ(hp_observation.graph_revision,
            hp_child.descriptor().revision().value());
  EXPECT_EQ(hp_observation.target_node_id,
            hp_child.descriptor().target_node_id());
  EXPECT_EQ(hp_observation.intent, ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(hp_observation.quality, ComputeRunQuality::Full);
  EXPECT_EQ(hp_observation.identity_run_id, hp_identity.run_id().value());
  EXPECT_EQ(hp_observation.local_task_id, hp_identity.local_task_id().value());

  EXPECT_EQ(rt_observation.trace_node_id, kRtTraceNodeId);
  EXPECT_EQ(rt_observation.descriptor_run_id,
            rt_child.descriptor().id().value());
  EXPECT_EQ(rt_observation.graph_identity,
            rt_child.descriptor().graph_identity());
  EXPECT_EQ(rt_observation.graph_revision,
            rt_child.descriptor().revision().value());
  EXPECT_EQ(rt_observation.target_node_id,
            rt_child.descriptor().target_node_id());
  EXPECT_EQ(rt_observation.intent, ComputeIntent::RealTimeUpdate);
  EXPECT_EQ(rt_observation.quality, ComputeRunQuality::Interactive);
  EXPECT_EQ(rt_observation.identity_run_id, rt_identity.run_id().value());
  EXPECT_EQ(rt_observation.local_task_id, rt_identity.local_task_id().value());

  const std::vector<ExecutionServiceHost::TraceEvent> traces =
      host.trace_events();
  ASSERT_EQ(traces.size(), 2U);
  std::size_t hp_marker_count = 0U;
  std::size_t rt_marker_count = 0U;
  for (const ExecutionServiceHost::TraceEvent& trace : traces) {
    EXPECT_EQ(trace.action, ExecutionTraceAction::AssignInitial);
    EXPECT_GE(trace.worker_id, 0);
    EXPECT_LT(trace.worker_id, 2);
    if (trace.node_id == kHpTraceNodeId) {
      ++hp_marker_count;
      EXPECT_EQ(trace.epoch, hp_child.descriptor().id().value());
      EXPECT_NE(trace.epoch, rt_child.descriptor().id().value());
    } else if (trace.node_id == kRtTraceNodeId) {
      ++rt_marker_count;
      EXPECT_EQ(trace.epoch, rt_child.descriptor().id().value());
      EXPECT_NE(trace.epoch, hp_child.descriptor().id().value());
    } else {
      ADD_FAILURE() << "Observed a trace marker outside the realtime siblings.";
    }
  }
  EXPECT_EQ(hp_marker_count, 1U);
  EXPECT_EQ(rt_marker_count, 1U);
}

/**
 * @brief Verifies one active Run failure does not fail or drain its peer.
 *
 * @return Nothing.
 * @throws std::bad_alloc when Run, graph, callback, or future storage cannot
 * allocate.
 * @throws std::future_error when test synchronization state is invalid.
 * @throws std::system_error when async launch or synchronization fails.
 * @note Both callbacks first enter the same fixed two-worker pool and block on
 * independent promise gates. Only after both entries are observed does the
 * test release the failing Run, retain the peer gate, and prove that the exact
 * failure settles without completing the peer. Both idempotent release guards
 * precede future destruction during every cleanup path; all gates are released
 * before any future is consumed. The peer then completes normally and
 * publishes its independent success outcome.
 */
TEST(ExecutionService, IsolatesConcurrentRunFailureFromActivePeer) {
  ExecutionService service(2);
  ExecutionServiceHost failing_host;
  ExecutionServiceHost peer_host;
  GraphModel failing_graph("cache/execution-service-isolated-failure");
  failing_graph.add_node(make_plan_node(65));
  failing_graph.validate_topology();
  GraphModel peer_graph("cache/execution-service-isolated-peer");
  peer_graph.add_node(make_plan_node(66));
  peer_graph.validate_topology();
  GraphTraversalService traversal;
  ComputeRun failing(make_test_submission(
      "service-isolated-failure", failing_graph.revision().value(), 65));
  ComputeRun peer(make_test_submission("service-isolated-peer",
                                       peer_graph.revision().value(), 66));
  failing.advance_to(ComputeRunPhase::Admitted);
  peer.advance_to(ComputeRunPhase::Admitted);
  failing.emplace_submission_plan(failing_graph, traversal, 65,
                                  std::vector<Device>{Device::CPU});
  peer.emplace_submission_plan(peer_graph, traversal, 66,
                               std::vector<Device>{Device::CPU});

  std::promise<void> failing_entered;
  std::future<void> failing_entered_future = failing_entered.get_future();
  std::promise<void> release_failing;
  const std::shared_future<void> failing_release =
      release_failing.get_future().share();
  std::promise<void> peer_entered;
  std::future<void> peer_entered_future = peer_entered.get_future();
  std::promise<void> release_peer;
  const std::shared_future<void> peer_release =
      release_peer.get_future().share();
  const std::exception_ptr failure =
      std::make_exception_ptr(std::runtime_error("isolated Run failure"));

  ComputeRunLease failing_lease = failing.acquire_lease();
  const ComputeRunTaskIdentity failing_identity =
      failing_lease.task_identity(0);
  std::vector<ReadyTaskSubmission> failing_ready;
  failing_ready.emplace_back(
      std::move(failing_lease), failing_identity, 65, true,
      [&failing_entered, failing_release, failure](
          ComputeRunLease&, const ComputeRunTaskIdentity&,
          ExecutionTaskRuntime&) {
        failing_entered.set_value();
        failing_release.wait();
        std::rethrow_exception(failure);
      });

  ComputeRunLease peer_lease = peer.acquire_lease();
  const ComputeRunTaskIdentity peer_identity = peer_lease.task_identity(0);
  std::vector<ReadyTaskSubmission> peer_ready;
  peer_ready.emplace_back(std::move(peer_lease), peer_identity, 66, true,
                          [&peer_entered, peer_release](
                              ComputeRunLease&, const ComputeRunTaskIdentity&,
                              ExecutionTaskRuntime& runtime) {
                            peer_entered.set_value();
                            peer_release.wait();
                            runtime.dec_tasks_to_complete();
                          });

  std::future<void> failing_future;
  std::future<void> peer_future;
  ScopedPromiseRelease peer_release_guard(release_peer);
  ScopedPromiseRelease failing_release_guard(release_failing);
  failing_future = std::async(
      std::launch::async,
      [&service, &failing_host, ready = std::move(failing_ready)]() mutable {
        service.execute_run(failing_host, "cpu", std::move(ready), 1);
      });
  peer_future = std::async(
      std::launch::async,
      [&service, &peer_host, ready = std::move(peer_ready)]() mutable {
        service.execute_run(peer_host, "cpu", std::move(ready), 1);
      });

  const std::future_status failing_entered_status =
      failing_entered_future.wait_for(std::chrono::seconds(2));
  const std::future_status peer_entered_status =
      peer_entered_future.wait_for(std::chrono::seconds(2));
  EXPECT_EQ(failing_entered_status, std::future_status::ready);
  EXPECT_EQ(peer_entered_status, std::future_status::ready);
  if (failing_entered_status == std::future_status::ready &&
      peer_entered_status == std::future_status::ready) {
    EXPECT_TRUE(failing_release_guard.release());
    const std::future_status failing_completion_status =
        failing_future.wait_for(std::chrono::seconds(2));
    EXPECT_EQ(failing_completion_status, std::future_status::ready);
    const std::future_status peer_blocked_status =
        peer_future.wait_for(std::chrono::milliseconds(100));
    EXPECT_EQ(peer_blocked_status, std::future_status::timeout);
    EXPECT_FALSE(peer.is_terminal());
  }

  EXPECT_TRUE(failing_release_guard.release());
  EXPECT_TRUE(peer_release_guard.release());
  const std::future_status failing_cleanup_status =
      failing_future.wait_for(std::chrono::seconds(2));
  const std::future_status peer_cleanup_status =
      peer_future.wait_for(std::chrono::seconds(2));
  EXPECT_EQ(failing_cleanup_status, std::future_status::ready);
  EXPECT_EQ(peer_cleanup_status, std::future_status::ready);

  std::exception_ptr observed;
  if (failing_cleanup_status == std::future_status::ready) {
    try {
      failing_future.get();
      ADD_FAILURE() << "Expected isolated Run failure.";
    } catch (...) {
      observed = std::current_exception();
    }
  }
  EXPECT_TRUE(observed == failure);

  bool peer_completed_normally = false;
  if (peer_cleanup_status == std::future_status::ready) {
    try {
      peer_future.get();
      peer_completed_normally = true;
    } catch (const std::exception& error) {
      ADD_FAILURE() << "Peer Run threw unexpectedly: " << error.what();
    } catch (...) {
      ADD_FAILURE() << "Peer Run threw an unexpected non-standard exception.";
    }
  }
  EXPECT_TRUE(peer_completed_normally);
  if (peer_completed_normally) {
    EXPECT_TRUE(peer.publish_succeeded());
  }
  const auto failing_outcome = failing.terminal_outcome();
  EXPECT_TRUE(failing_outcome.has_value());
  if (failing_outcome.has_value()) {
    EXPECT_EQ(failing_outcome->kind, ComputeRunTerminalKind::Failed);
    EXPECT_TRUE(failing_outcome->failure == failure);
  }
  const auto peer_outcome = peer.terminal_outcome();
  EXPECT_TRUE(peer_outcome.has_value());
  if (peer_outcome.has_value()) {
    EXPECT_EQ(peer_outcome->kind, ComputeRunTerminalKind::Succeeded);
  }
  EXPECT_EQ(failing_host.context_entries(), failing_host.context_exits());
  EXPECT_EQ(peer_host.context_entries(), peer_host.context_exits());
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies mixed Run ids are rejected before any callback executes.
 *
 * @note Both submissions intentionally reuse local id zero. Their Run ids,
 * rather than that local value, drive service validation.
 */
TEST(ExecutionService, RejectsMixedRunInitialBatchBeforeExecution) {
  ExecutionService service(2);
  ExecutionServiceHost host;
  ComputeRun first(make_test_submission("mixed-first", 61, 71));
  ComputeRun second(make_test_submission("mixed-second", 62, 72));
  std::atomic_int entered{0};
  std::vector<ReadyTaskSubmission> mixed;
  mixed.push_back(
      make_counted_ready_submission(first.acquire_lease(), 0, 71, entered));
  mixed.push_back(
      make_counted_ready_submission(second.acquire_lease(), 0, 72, entered));

  EXPECT_THROW(service.execute_run(host, "cpu", std::move(mixed), 2),
               std::invalid_argument);
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(host.context_entries(), 0);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies cancellation before service publication admits no callback.
 *
 * @return Nothing; GoogleTest assertions report premature admission or leaked
 * resources.
 * @throws Allocation, service, or synchronization exceptions from setup.
 * @note The submission already owns a lease when cancellation wins. The service
 * must return before reserving resources or publishing the initial ready value.
 */
TEST(ExecutionServiceCancellation,
     PrepublicationCancellationSkipsAdmissionAndExecution) {
  ExecutionService service(1);
  ExecutionServiceHost host;
  ComputeRun run(make_test_submission("cancel-before-service", 61, 71));
  const ComputeRunCancellationSource source = run.cancellation_source();
  std::atomic_int entered{0};
  std::vector<ReadyTaskSubmission> ready;
  ready.push_back(
      make_counted_ready_submission(run.acquire_lease(), 0U, 71, entered));

  ASSERT_TRUE(source.request_cancellation(
      ComputeRunCancellationReason::ExplicitRequest));
  EXPECT_NO_THROW(service.execute_run(host, "cpu", std::move(ready), 1));
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(host.context_entries(), 0);
  EXPECT_EQ(host.context_exits(), 0);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies the post-dequeue/pre-callback cancellation observation seam.
 *
 * @return Nothing; GoogleTest assertions report callback entry or resource
 * drainage failures.
 * @throws Allocation, service, or synchronization exceptions from setup.
 * @note The Host hook runs after the worker counts the task in flight. The
 * submission executable must remain unentered. Caller settlement may precede
 * destruction of the worker's final local Run owner, so the ledger is polled
 * until that owner releases the root reservation.
 */
TEST(ExecutionServiceCancellation,
     CancellationAfterDequeueSuppressesExecutableAndDrainsExactly) {
  ExecutionService service(1);
  ExecutionServiceHost host;
  ComputeRun run(make_test_submission("cancel-after-dequeue", 62, 72));
  const ComputeRunCancellationSource source = run.cancellation_source();
  host.cancel_on_next_initial_assignment(source);
  std::atomic_int entered{0};
  std::vector<ReadyTaskSubmission> ready;
  ready.push_back(
      make_counted_ready_submission(run.acquire_lease(), 0U, 72, entered));

  EXPECT_NO_THROW(service.execute_run(host, "cpu", std::move(ready), 1));
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(host.context_entries(), 1);
  EXPECT_EQ(host.context_exits(), 1);
  EXPECT_FALSE(host.trace_recording_failed());
  ASSERT_TRUE(run.terminal_outcome().has_value());
  EXPECT_EQ(run.terminal_outcome()->kind, ComputeRunTerminalKind::Cancelled);
  EXPECT_TRUE(wait_for_resource_reservation(service, ResourceVector{}));
}

/**
 * @brief Verifies exact queued-Run purge without disturbing an executing peer.
 *
 * @return Nothing; GoogleTest assertions report cross-Run purge or reservation
 * failures.
 * @throws Allocation, future, service, or synchronization exceptions from
 * setup and settlement.
 * @note A single worker remains occupied by the peer while both target values
 * enter the bounded ready store. Cancellation settles only the target and
 * releases its complete root reservation before the peer gate opens.
 */
TEST(ExecutionServiceCancellation,
     PurgesOnlyTheCancelledQueuedRunAndReleasesItsReservation) {
  ExecutionService service(1);
  ExecutionServiceHost peer_host;
  ExecutionServiceHost target_host;
  ComputeRun peer(make_test_submission("cancel-queue-peer", 63, 73));
  ComputeRun target(make_test_submission("cancel-queue-target", 64, 74));
  const ComputeRunCancellationSource target_source =
      target.cancellation_source();
  std::promise<void> peer_entered;
  std::promise<void> release_peer;
  const std::shared_future<void> peer_release =
      release_peer.get_future().share();
  std::atomic_int target_entered{0};

  std::vector<ReadyTaskSubmission> peer_ready;
  ComputeRunLease peer_lease = peer.acquire_lease();
  peer_ready.emplace_back(peer_lease, peer_lease.task_identity(0U), 73, true,
                          [&peer_entered, peer_release](
                              ComputeRunLease&, const ComputeRunTaskIdentity&,
                              ExecutionTaskRuntime& runtime) {
                            peer_entered.set_value();
                            peer_release.wait();
                            runtime.dec_tasks_to_complete();
                          });
  const CpuRunResourceDemand run_demand{};
  const ResourceVector peer_required =
      service.estimate_cpu_run_resources(peer_ready.front(), 1, run_demand);

  std::vector<ReadyTaskSubmission> target_ready;
  target_ready.push_back(make_counted_ready_submission(target.acquire_lease(),
                                                       0U, 74, target_entered));
  target_ready.push_back(make_counted_ready_submission(target.acquire_lease(),
                                                       1U, 74, target_entered));
  const ResourceVector target_required =
      service.estimate_cpu_run_resources(target_ready.front(), 2, run_demand);
  const std::optional<ResourceVector> combined_required =
      checked_add_resources(peer_required, target_required);
  ASSERT_TRUE(combined_required.has_value());

  std::future<void> peer_future;
  std::future<void> target_future;
  ScopedPromiseRelease peer_release_guard(release_peer);
  peer_future = std::async(
      std::launch::async,
      [&service, &peer_host, ready = std::move(peer_ready)]() mutable {
        service.execute_run(peer_host, "cpu", std::move(ready), 1);
      });
  ASSERT_EQ(peer_entered.get_future().wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  target_future = std::async(
      std::launch::async,
      [&service, &target_host, ready = std::move(target_ready)]() mutable {
        service.execute_run(target_host, "cpu", std::move(ready), 2);
      });
  ASSERT_TRUE(wait_for_ready_task_count(service, 2U));
  ASSERT_TRUE(wait_for_resource_reservation(service, *combined_required));

  EXPECT_TRUE(target_source.request_cancellation(
      ComputeRunCancellationReason::ExplicitRequest));
  ASSERT_EQ(target_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(target_future.get());
  EXPECT_EQ(target_entered.load(std::memory_order_relaxed), 0);
  EXPECT_TRUE(wait_for_resource_reservation(service, peer_required));
  EXPECT_EQ(peer_future.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);

  EXPECT_TRUE(peer_release_guard.release());
  ASSERT_EQ(peer_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(peer_future.get());
  EXPECT_TRUE(wait_for_resource_reservation(service, ResourceVector{}));
  EXPECT_EQ(peer_host.context_entries(), peer_host.context_exits());
  EXPECT_EQ(target_host.context_entries(), 0);
}

/**
 * @brief Verifies cancellation waits for an active non-preemptible callback.
 *
 * @return Nothing; GoogleTest assertions report premature settlement,
 * dependent re-entry, or resource leakage.
 * @throws Allocation, future, service, or synchronization exceptions from
 * setup and settlement.
 * @note The active callback attempts dependent publication after cancellation.
 * The dependent is rejected without entry, while caller settlement waits for
 * the original callback's actual exit instead of logical completion count.
 */
TEST(ExecutionServiceCancellation,
     RunningCallbackDrainsBeforeSettlementAndCannotPublishDependentWork) {
  ExecutionService service(1);
  ExecutionServiceHost host;
  ComputeRun run(make_test_submission("cancel-running", 65, 75));
  const ComputeRunCancellationSource source = run.cancellation_source();
  std::promise<void> initial_entered;
  std::promise<void> release_initial;
  const std::shared_future<void> release = release_initial.get_future().share();
  std::atomic_int dependent_entered{0};

  ComputeRunLease initial_lease = run.acquire_lease();
  std::vector<ReadyTaskSubmission> ready;
  ready.emplace_back(
      initial_lease, initial_lease.task_identity(0U), 75, true,
      [&initial_entered, release, &dependent_entered](
          ComputeRunLease& retained_lease, const ComputeRunTaskIdentity&,
          ExecutionTaskRuntime& runtime) {
        initial_entered.set_value();
        release.wait();
        auto* ready_runtime =
            dynamic_cast<ReadyTaskSubmissionRuntime*>(&runtime);
        if (ready_runtime == nullptr) {
          throw std::logic_error(
              "Cancellation test requires ready-submission runtime.");
        }
        ComputeRunLease dependent_lease = retained_lease;
        const ComputeRunTaskIdentity dependent_identity =
            dependent_lease.task_identity(1U);
        ready_runtime->submit_ready_submission(ReadyTaskSubmission(
            std::move(dependent_lease), dependent_identity, 76, false,
            [&dependent_entered](ComputeRunLease&,
                                 const ComputeRunTaskIdentity&,
                                 ExecutionTaskRuntime& dependent_runtime) {
              dependent_entered.fetch_add(1, std::memory_order_relaxed);
              dependent_runtime.dec_tasks_to_complete();
            }));
        runtime.dec_tasks_to_complete();
      });

  std::future<void> run_future;
  ScopedPromiseRelease release_guard(release_initial);
  run_future =
      std::async(std::launch::async,
                 [&service, &host, ready = std::move(ready)]() mutable {
                   service.execute_run(host, "cpu", std::move(ready), 2);
                 });
  ASSERT_EQ(initial_entered.get_future().wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_TRUE(source.request_cancellation(
      ComputeRunCancellationReason::ExplicitRequest));
  EXPECT_EQ(run_future.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);

  EXPECT_TRUE(release_guard.release());
  ASSERT_EQ(run_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_NO_THROW(run_future.get());
  EXPECT_EQ(dependent_entered.load(std::memory_order_relaxed), 0);
  EXPECT_TRUE(wait_for_resource_reservation(service, ResourceVector{}));
  EXPECT_EQ(host.context_entries(), host.context_exits());
}

/**
 * @brief Verifies registered worker failure preserves exception identity.
 *
 * @note The custom executable publishes through its retained matching lease
 * and then rethrows the same exception_ptr. The service CPU fence must settle
 * the batch before surfacing that exact pointer.
 */
TEST(ExecutionService, PreservesExactFailureForMatchingRegisteredTask) {
  ExecutionService service(2);
  ExecutionServiceHost host;
  GraphModel graph("cache/execution-service-failure");
  graph.add_node(make_plan_node(81));
  graph.validate_topology();
  GraphTraversalService traversal;
  ComputeRun run(
      make_test_submission("service-failure", graph.revision().value(), 81));
  ASSERT_TRUE(run.advance_to(ComputeRunPhase::Admitted));
  TaskSubmissionPlan& plan = run.emplace_submission_plan(
      graph, traversal, 81, std::vector<Device>{Device::CPU});
  ASSERT_EQ(plan.size(), 1U);
  ComputeRunLease lease = run.acquire_lease();
  const ComputeRunTaskIdentity identity = lease.task_identity(0);
  const std::exception_ptr failure =
      std::make_exception_ptr(std::runtime_error("service exact failure"));
  std::vector<ReadyTaskSubmission> ready;
  ready.emplace_back(lease, identity, 81, true,
                     [failure](ComputeRunLease& retained_lease,
                               const ComputeRunTaskIdentity& retained_identity,
                               ExecutionTaskRuntime&) {
                       (void)retained_lease.publish_task_failure(
                           retained_identity, failure);
                       std::rethrow_exception(failure);
                     });

  std::exception_ptr observed;
  try {
    service.execute_run(host, "cpu", std::move(ready), 1);
    FAIL() << "Expected service worker failure.";
  } catch (...) {
    observed = std::current_exception();
  }
  EXPECT_TRUE(observed == failure);
  const auto terminal = run.terminal_outcome();
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->kind, ComputeRunTerminalKind::Failed);
  EXPECT_TRUE(terminal->failure == failure);
  EXPECT_EQ(host.context_entries(), host.context_exits());
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Verifies private execution lanes cannot bypass ready-only admission.
 *
 * @note This is a private implementation contract; no public or plugin vtable
 *       is involved.
 */
TEST(ExecutionService, RejectsBorrowedHandlesAndAnonymousCallbacks) {
  ExecutionService service;
  EXPECT_THROW(
      service.submit_initial_task_handles({}, 0, ExecutionTaskPriority::Normal),
      std::logic_error);
  EXPECT_THROW(service.submit_ready_task_handles_from_worker(
                   {}, ExecutionTaskPriority::Normal),
               std::logic_error);
  EXPECT_THROW(service.submit_ready_task_any_thread(
                   [] {}, ExecutionTaskPriority::Normal, std::nullopt),
               std::logic_error);
}

/**
 * @brief Verifies legacy A completion cannot publish B after cancellation.
 *
 * @return Nothing; GoogleTest assertions report callback entry, ledger, or
 * staged-publication failures.
 * @throws Allocation, registry, execution, or synchronization exceptions from
 * fixture setup and execution.
 * @note In the return branch, an injected deadline expires at the first
 * observation after source A returns and before dependency release. In the
 * exception branch, explicit cancellation wins immediately before A throws.
 * The recursive publication gate retires B's plan-owned completion unit, A
 * retires its callback unit, and deterministic drainage reaches zero without
 * entering B or mutating committed Graph caches. The later exception cannot
 * replace Cancelled.
 */
TEST(ComputeRunCancellation,
     LegacySourceReturnAndExceptionSuppressDependentAndSettleLedger) {
  ensure_legacy_cancellation_operations_registered();
  for (const bool throw_after_cancellation : {false, true}) {
    SCOPED_TRACE(throw_after_cancellation ? "exception" : "return");
    g_legacy_cancellation_source_calls.store(0, std::memory_order_relaxed);
    g_legacy_cancellation_dependent_calls.store(0, std::memory_order_relaxed);

    GraphModel graph(throw_after_cancellation
                         ? "cache/legacy-cancellation-exception-chain"
                         : "cache/legacy-cancellation-return-chain");
    Node source = make_plan_node(91);
    source.type = "compute_run_cancel_chain";
    source.subtype =
        throw_after_cancellation ? "source_throw_after_cancel" : "source";
    Node dependent = make_plan_node(92);
    dependent.type = "compute_run_cancel_chain";
    dependent.subtype = "dependent";
    dependent.image_inputs.push_back(ImageInput{91, "image"});
    graph.add_node(std::move(source));
    graph.add_node(std::move(dependent));
    graph.validate_topology();

    GraphTraversalService traversal;
    GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                            testing::make_yaml_cache_metadata_codec()};
    GraphEventService events;
    CompletionTrackingRuntime runtime;
    TimingCollector timings;
    std::mutex timing_mutex;
    using Clock = std::chrono::steady_clock;
    const Clock::time_point deadline =
        Clock::time_point{} + std::chrono::seconds(10);
    ComputeRunSubmission submission = make_test_submission(
        "legacy-cancel-chain", graph.revision().value(), 92);
    submission.qos.deadline = deadline;
    ComputeRun run(std::move(submission), [throw_after_cancellation,
                                           deadline]() noexcept {
      if (!throw_after_cancellation && g_legacy_cancellation_source_calls.load(
                                           std::memory_order_acquire) > 0) {
        return deadline;
      }
      return deadline - std::chrono::seconds(1);
    });
    ASSERT_TRUE(run.advance_to(ComputeRunPhase::Admitted));
    TaskSubmissionPlan& plan = run.emplace_submission_plan(
        graph, traversal, 92, std::vector<Device>{Device::CPU});
    ASSERT_EQ(plan.size(), 2U);
    ComputeRunLease lease = run.acquire_lease();
    ScopedLegacyCancellationSource configured_source(run.cancellation_source());
    plan.emplace_task_runner(NodeTaskRunnerContext{
        graph,
        cache,
        events,
        runtime,
        timings,
        timing_mutex,
        plan.execution_order(),
        plan.id_to_idx(),
        plan.temp_results(),
        plan.resolved_ops(),
        plan.compute_plan().task_graph,
        false,
        false,
        true,
        nullptr,
        &lease,
    });

    runtime.submit_initial_task_handles({}, 0);
    plan.submit_initial_ready_tasks(lease, runtime);
    ASSERT_EQ(runtime.callbacks_submitted(), 1);
    std::exception_ptr observed_failure;
    try {
      runtime.wait_for_completion();
    } catch (...) {
      observed_failure = std::current_exception();
    }
    if (throw_after_cancellation) {
      ASSERT_TRUE(observed_failure);
      try {
        std::rethrow_exception(observed_failure);
      } catch (const std::exception& error) {
        EXPECT_NE(
            std::string(error.what()).find(kLegacyPostCancellationFailure),
            std::string::npos)
            << error.what();
      }
    } else {
      EXPECT_FALSE(observed_failure);
    }

    EXPECT_EQ(
        g_legacy_cancellation_source_calls.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(
        g_legacy_cancellation_dependent_calls.load(std::memory_order_relaxed),
        0);
    EXPECT_EQ(runtime.callbacks_submitted(), 1);
    EXPECT_EQ(runtime.callbacks_invoked(), 1);
    EXPECT_EQ(runtime.total_units_added(), 2);
    EXPECT_EQ(runtime.completion_releases(), 2);
    EXPECT_EQ(runtime.tasks_to_complete(), 0);
    EXPECT_EQ(runtime.planned_execution_events(), 1);
    EXPECT_FALSE(graph.node(91).cached_output_high_precision.has_value());
    EXPECT_FALSE(graph.node(92).cached_output_high_precision.has_value());
    ASSERT_TRUE(run.terminal_outcome().has_value());
    EXPECT_EQ(run.terminal_outcome()->kind, ComputeRunTerminalKind::Cancelled);
    EXPECT_EQ(run.terminal_outcome()->cancellation_reason,
              throw_after_cancellation
                  ? ComputeRunCancellationReason::ExplicitRequest
                  : ComputeRunCancellationReason::DeadlineExceeded);
  }
}

/**
 * @brief Verifies a terminal accepted task retires its counted execution unit.
 *
 * @note The owned callback is accepted before failure publication and invoked
 * by the runtime afterward. Zero execution traces and an untouched temporary
 * slot prove that terminal retirement does not enter planned node work.
 */
TEST(ComputeRunCompletion,
     TerminalAcceptedTaskSettlesUnitWithoutExecutingPlannedWork) {
  GraphModel graph("cache/compute-run-terminal-task");
  graph.add_node(make_plan_node(31));
  graph.validate_topology();
  GraphTraversalService traversal;
  ComputeRun run(
      make_test_submission("terminal-task", graph.revision().value(), 31));
  ASSERT_TRUE(run.advance_to(ComputeRunPhase::Admitted));
  TaskSubmissionPlan& plan = run.emplace_submission_plan(
      graph, traversal, 31, std::vector<Device>{Device::CPU});
  ASSERT_EQ(plan.size(), 1U);
  ASSERT_EQ(plan.temp_results().size(), 1U);

  ComputeRunLease lease = run.acquire_lease();
  CompletionTrackingRuntime runtime;
  runtime.submit_initial_task_handles({}, 0);
  plan.submit_initial_ready_tasks(lease, runtime);
  ASSERT_EQ(runtime.callbacks_submitted(), 1);
  ASSERT_EQ(runtime.total_units_added(), 1);
  ASSERT_EQ(runtime.tasks_to_complete(), 1);
  EXPECT_THROW(lease.execute_task(lease.task_identity(999), runtime),
               std::invalid_argument);
  EXPECT_EQ(runtime.completion_releases(), 0);
  EXPECT_EQ(runtime.tasks_to_complete(), 1);

  const std::exception_ptr failure =
      std::make_exception_ptr(std::runtime_error("another worker failed"));
  ASSERT_TRUE(run.publish_failed(failure));
  EXPECT_NO_THROW(runtime.wait_for_completion());

  EXPECT_EQ(runtime.callbacks_invoked(), 1);
  EXPECT_EQ(runtime.completion_releases(), 1);
  EXPECT_EQ(runtime.tasks_to_complete(), 0);
  EXPECT_EQ(runtime.planned_execution_events(), 0);
  EXPECT_FALSE(plan.temp_results().front().has_value());
  ASSERT_TRUE(run.terminal_outcome().has_value());
  EXPECT_TRUE(run.terminal_outcome()->failure == failure);
}

/**
 * @brief Verifies a terminal accepted bootstrap retires its counted unit.
 *
 * @note The bootstrap callback is queued before failure publication. Its
 * terminal path must release only the bootstrap unit and must not add planned
 * task units or publish any planned callback.
 */
TEST(ComputeRunCompletion,
     TerminalAcceptedBootstrapSettlesUnitWithoutPublishingPlannedWork) {
  GraphModel graph("cache/compute-run-terminal-bootstrap");
  graph.add_node(make_plan_node(32));
  graph.validate_topology();
  GraphTraversalService traversal;
  ComputeRun run(
      make_test_submission("terminal-bootstrap", graph.revision().value(), 32));
  ASSERT_TRUE(run.advance_to(ComputeRunPhase::Admitted));
  TaskSubmissionPlan& plan = run.emplace_submission_plan(
      graph, traversal, 32, std::vector<Device>{Device::CPU});
  ASSERT_EQ(plan.size(), 1U);

  ComputeRunLease dispatcher_lease = run.acquire_lease();
  CompletionTrackingRuntime runtime;
  runtime.submit_initial_task_handles({}, 0);
  runtime.inc_tasks_to_complete(1);
  ComputeRunLease bootstrap_lease = dispatcher_lease;
  runtime.submit_ready_task_any_thread(
      [lease = std::move(bootstrap_lease), &runtime]() mutable {
        lease.execute_bootstrap(runtime);
      });
  ASSERT_EQ(runtime.callbacks_submitted(), 1);
  ASSERT_EQ(runtime.total_units_added(), 1);
  ASSERT_EQ(runtime.tasks_to_complete(), 1);

  const std::exception_ptr failure = std::make_exception_ptr(
      std::runtime_error("run failed before bootstrap"));
  ASSERT_TRUE(run.publish_failed(failure));
  EXPECT_NO_THROW(runtime.wait_for_completion());

  EXPECT_EQ(runtime.callbacks_submitted(), 1);
  EXPECT_EQ(runtime.callbacks_invoked(), 1);
  EXPECT_EQ(runtime.total_units_added(), 1);
  EXPECT_EQ(runtime.completion_releases(), 1);
  EXPECT_EQ(runtime.tasks_to_complete(), 0);
  EXPECT_EQ(runtime.planned_execution_events(), 0);
  EXPECT_FALSE(plan.temp_results().front().has_value());
  ASSERT_TRUE(run.terminal_outcome().has_value());
  EXPECT_TRUE(run.terminal_outcome()->failure == failure);
}

}  // namespace
}  // namespace ps::compute
