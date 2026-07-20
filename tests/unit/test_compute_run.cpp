#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
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
#include "graph/graph_cache_service.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_traversal_service.hpp"
#include "photospider/core/graph_error.hpp"
#include "photospider/scheduler/scheduler.hpp"
#include "photospider/scheduler/scheduler_task_runtime.hpp"
#include "providers/configured_image_artifact_codec.hpp"
#include "runtime/graph_event_service.hpp"
#include "runtime/graph_runtime.hpp"
#include "support/cache_test_dependencies.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Builds deterministic descriptor input for ComputeRun unit tests.
 *
 * @param graph_identity Graph/session label to capture.
 * @param topology_generation Topology-only submission revision.
 * @param target_node_id Requested graph-local target.
 * @return Valid full-quality HP submission with explicit throughput QoS.
 * @throws std::bad_alloc if graph identity ownership cannot allocate.
 * @note The returned deadline is absent so test equality does not depend on
 * wall-clock timing.
 */
ComputeRunSubmission make_test_submission(std::string graph_identity,
                                          uint64_t topology_generation,
                                          int target_node_id) {
  return ComputeRunSubmission{
      std::move(graph_identity),
      ComputeRunSubmissionRevision{topology_generation},
      target_node_id,
      ComputeIntent::GlobalHighPrecision,
      ComputeRunQuality::Full,
      ComputeRunQos{ComputeRunQosClass::Throughput, std::nullopt, 3, 2}};
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
 * @note This fixture models scheduler completion ownership only; it owns no
 * worker, epoch filter, graph state, or operation implementation.
 */
class CompletionTrackingRuntime final : public SchedulerTaskRuntime {
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
      std::vector<TaskHandle>&& handles, int total_task_count,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
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
      std::vector<TaskHandle>&& handles,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
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
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal,
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
   * @note Nonpositive input is a no-op, matching current scheduler behavior.
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
   * @param action Stable scheduler trace action.
   * @param node_id Backend node id, unused by this fixture.
   * @return Nothing.
   * @throws Nothing.
   * @note Assignment traces are intentionally excluded from the execution
   * count.
   */
  void log_event(SchedulerTraceAction action, int node_id) noexcept override {
    (void)node_id;
    if (action == SchedulerTraceAction::Execute ||
        action == SchedulerTraceAction::ExecuteTile) {
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
class ExecutionServiceHost final : public SchedulerHostContext {
 public:
  /**
   * @brief Complete immutable scheduler trace tuple observed by one Host.
   *
   * @throws Nothing for scalar value construction and movement.
   * @note Epoch is the opaque ComputeRunId value forwarded by ExecutionService.
   */
  struct TraceEvent final {
    /** @brief Scheduler action forwarded by the active service worker. */
    SchedulerTraceAction action = SchedulerTraceAction::AssignInitial;

    /** @brief Graph-local diagnostic node id supplied by the ready task. */
    int node_id = -1;

    /** @brief Fixed-pool worker id that emitted this event. */
    int worker_id = -1;

    /** @brief Opaque Run epoch active on that worker. */
    std::uint64_t epoch = 0;
  };

  /**
   * @brief Reports the fixture's only physical capability.
   * @param device Capability requested by the CPU scheduler.
   * @return True only for CPU.
   * @throws Nothing.
   */
  bool is_device_available(Device device) const noexcept override {
    return device == Device::CPU;
  }

  /**
   * @brief Records one worker-context entry.
   * @param worker_id CPU worker id, ignored after validation by scheduler.
   * @param epoch Active nonzero scheduler epoch.
   * @return Nothing.
   * @throws Nothing.
   */
  void set_task_context(int worker_id, std::uint64_t epoch) noexcept override {
    (void)worker_id;
    (void)epoch;
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
   * @brief Records one forwarded scheduler trace.
   * @param action Trace action copied from the service CPU runtime.
   * @param node_id Planned node id.
   * @param worker_id Active CPU worker id.
   * @param epoch Active scheduler epoch.
   * @return Nothing.
   * @throws Nothing.
   */
  void log_event(SchedulerTraceAction action, int node_id, int worker_id,
                 std::uint64_t epoch) noexcept override {
    try {
      std::lock_guard<std::mutex> lock(trace_mutex_);
      trace_events_.push_back(TraceEvent{action, node_id, worker_id, epoch});
    } catch (...) {
      trace_recording_failed_.store(true, std::memory_order_relaxed);
    }
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
  /** @brief Top-level CPU callback context entries. */
  std::atomic_int context_entries_{0};

  /** @brief Balanced top-level CPU callback context exits. */
  std::atomic_int context_exits_{0};

  /** @brief Serializes concurrent trace publication and snapshot copying. */
  mutable std::mutex trace_mutex_;

  /** @brief Complete scheduler traces forwarded to this Host proxy. */
  std::vector<TraceEvent> trace_events_;

  /** @brief Whether noexcept trace observation lost any event. */
  std::atomic_bool trace_recording_failed_{false};
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

  /** @brief Topology generation exposed by the retained descriptor. */
  uint64_t topology_generation = 0;

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
  return RetainedSubmissionObservation{
      trace_node_id,
      descriptor.id().value(),
      descriptor.graph_identity(),
      descriptor.revision().topology_generation,
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
    SchedulerTaskPriority priority = SchedulerTaskPriority::Normal,
    ReadyTaskResourceDemand resource_demand =
        ReadyTaskSubmission::default_resource_demand()) {
  const ComputeRunTaskIdentity identity = lease.task_identity(local_task_id);
  return ReadyTaskSubmission(
      std::move(lease), identity, trace_node_id, true,
      [&entered](ComputeRunLease& retained_lease,
                 const ComputeRunTaskIdentity& retained_identity,
                 SchedulerTaskRuntime& runtime) {
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
 * @brief Converts an exact vector into private service composition limits.
 * @param resources Complete capacity vector.
 * @return Private limits with identical dimensions.
 * @throws Nothing.
 */
ExecutionResourceLimits execution_limits(const ResourceVector& resources) {
  return ExecutionResourceLimits{
      resources.cpu_slots,     resources.retained_memory_bytes,
      resources.scratch_bytes, resources.ready_entries,
      resources.ready_bytes,
  };
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
  EXPECT_NO_THROW(service.execute_cpu_run(host, std::move(submissions), 1,
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
  EXPECT_NO_THROW(lower_service.execute_cpu_run(
      lower_host, std::move(lower_ready), 1,
      CpuRunResourceDemand{current_shared_bytes, {}}));
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(lower_service.resource_snapshot().reserved, ResourceVector{});

  ComputeRun rejected_run(make_test_submission(graph_identity, 3U, 1));
  std::vector<ReadyTaskSubmission> rejected_ready;
  rejected_ready.push_back(make_counted_ready_submission(
      rejected_run.acquire_lease(), 0U, 1, entered));
  EXPECT_THROW(
      lower_service.execute_cpu_run(
          lower_host, std::move(rejected_ready), 1,
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
  EXPECT_NO_THROW(complete_service.execute_cpu_run(
      complete_host, std::move(complete_ready), 1,
      CpuRunResourceDemand{current_shared_bytes + missing_entry_bytes, {}}));
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 2);
  EXPECT_EQ(complete_service.resource_snapshot().reserved, ResourceVector{});
}

/**
 * @brief Builds one HP or RT descriptor for dirty product paths.
 * @param graph_identity Stable GraphRuntime/session identity.
 * @param topology_generation Current graph topology identity.
 * @param node_id Dirty target node.
 * @param intent HP or RT single-domain intent.
 * @return Valid full-HP or interactive-RT submission.
 * @throws std::bad_alloc when graph identity ownership cannot allocate.
 */
ComputeRunSubmission make_dirty_resource_submission(
    const std::string& graph_identity, std::uint64_t topology_generation,
    int node_id, ComputeIntent intent) {
  ComputeRunSubmission submission =
      make_test_submission(graph_identity, topology_generation, node_id);
  submission.intent = intent;
  submission.quality = intent == ComputeIntent::RealTimeUpdate
                           ? ComputeRunQuality::Interactive
                           : ComputeRunQuality::Full;
  return submission;
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
        task_index + 1, entered, SchedulerTaskPriority::Normal,
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
    service.execute_cpu_run(host, std::move(rejected_ready),
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
      SchedulerTaskPriority::Normal, kRecoveryDemand));
  EXPECT_NO_THROW(
      service.execute_cpu_run(host, std::move(recovery_ready), 1,
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
  EXPECT_EQ(first.descriptor().revision().topology_generation, 17U);
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
  EXPECT_FALSE(
      run.publish_cancelled(ComputeRunCancellationReason::ExplicitRequest));
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
          won = run.publish_cancelled(
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
      make_test_submission("plan-storage", graph.topology_generation(), 11));
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
                                        first_graph.topology_generation(), 21));
  ComputeRun second(make_test_submission(
      "identity-second", second_graph.topology_generation(), 22));
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
  EXPECT_EQ(submission.metadata().revision().topology_generation, 41U);
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
                      SchedulerTaskRuntime&) {}),
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
  EXPECT_NO_THROW(service.execute_cpu_run(host, std::move(first_ready), 1));

  std::vector<ReadyTaskSubmission> second_ready;
  second_ready.push_back(
      make_counted_ready_submission(second.acquire_lease(), 0, 62, entered));
  EXPECT_NO_THROW(service.execute_cpu_run(host, std::move(second_ready), 1));

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
 * @brief Verifies scheduler-owner CPU reservations block Runs atomically.
 *
 * @return Nothing.
 * @throws std::bad_alloc or std::system_error from isolated service setup.
 * @note Releasing the sole legacy reservation restores the exact CPU slot and
 * permits an otherwise identical Run without changing fixed worker
 * infrastructure.
 */
TEST(ExecutionService, SharesCpuAdmissionWithLegacyOwnerAndRecoversExactly) {
  constexpr ReadyTaskResourceDemand kDemand{1U, 1U, 1U};
  ExecutionResourceLimits limits = ExecutionService::default_resource_limits();
  limits.cpu_slots = 1U;
  ExecutionService service(1U, limits);
  ExecutionServiceHost host;
  auto legacy = service.try_reserve_legacy_scheduler_workers(1U);
  ASSERT_TRUE(legacy.has_value());

  std::atomic_int entered{0};
  ComputeRun rejected_run(make_test_submission("cpu-owner-rejected", 1U, 1));
  std::vector<ReadyTaskSubmission> rejected_ready;
  rejected_ready.push_back(make_counted_ready_submission(
      rejected_run.acquire_lease(), 0U, 1, entered,
      SchedulerTaskPriority::Normal, kDemand));
  try {
    service.execute_cpu_run(host, std::move(rejected_ready), 1,
                            CpuRunResourceDemand{0U, kDemand});
    FAIL() << "Expected shared CPU admission rejection.";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::ComputeError);
  }
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{1U});

  legacy.reset();
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
  ComputeRun recovery_run(make_test_submission("cpu-owner-recovery", 2U, 2));
  std::vector<ReadyTaskSubmission> recovery_ready;
  recovery_ready.push_back(make_counted_ready_submission(
      recovery_run.acquire_lease(), 0U, 2, entered,
      SchedulerTaskPriority::Normal, kDemand));
  EXPECT_NO_THROW(service.execute_cpu_run(host, std::move(recovery_ready), 1,
                                          CpuRunResourceDemand{0U, kDemand}));
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
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
  const ExecutionResourceLimits limits{1U, kMaximum, kMaximum, kMaximum,
                                       kMaximum};
  constexpr ReadyTaskResourceDemand kOverflowDemand{1U, 1U, kMaximum};
  ExecutionService service(1U, limits);
  ExecutionServiceHost host;
  std::atomic_int entered{0};
  ComputeRun overflow_run(make_test_submission("resource-overflow", 1U, 1));
  std::vector<ReadyTaskSubmission> overflow_ready;
  overflow_ready.push_back(make_counted_ready_submission(
      overflow_run.acquire_lease(), 0U, 1, entered,
      SchedulerTaskPriority::Normal, kOverflowDemand));
  overflow_ready.push_back(make_counted_ready_submission(
      overflow_run.acquire_lease(), 1U, 2, entered,
      SchedulerTaskPriority::Normal, kOverflowDemand));
  try {
    service.execute_cpu_run(host, std::move(overflow_ready), 2,
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
      SchedulerTaskPriority::Normal, kRecoveryDemand));
  EXPECT_NO_THROW(
      service.execute_cpu_run(host, std::move(recovery_ready), 1,
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
                                    SchedulerTaskPriority::Normal, kTaskDemand);

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
 * @brief Verifies metadata string capacity uses both admission multipliers.
 *
 * @return Nothing.
 * @throws Standard Run, metadata, service, or estimator exceptions unchanged.
 * @note The chosen short and long strings have different actual copied
 * capacities on supported standard libraries, and that capacity delta differs
 * from their size delta. Retained bytes scale it by two fixed workers while
 * ready bytes scale it by five logical tasks.
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
            kWorkerCount * capacity_delta);
  EXPECT_EQ(long_resources.ready_bytes - short_resources.ready_bytes,
            static_cast<std::uint64_t>(kLogicalTaskCount) * capacity_delta);
  EXPECT_EQ(long_resources.cpu_slots, short_resources.cpu_slots);
  EXPECT_EQ(long_resources.ready_entries, short_resources.ready_entries);
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
      make_test_submission(graph_identity, graph.topology_generation(), 101));
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

  EXPECT_THROW(dispatch_planned_tasks(graph, small_service, small_host, 101,
                                      small_plan, small_lease),
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
      make_test_submission(graph_identity, graph.topology_generation(), 101));
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
  EXPECT_NO_THROW(dispatch_planned_tasks(graph, large_service, large_host, 101,
                                         large_plan, large_lease));
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
 * @brief Exercises the shared dirty adapter for HP and RT service Runs.
 *
 * @return Nothing.
 * @throws Standard allocation, filesystem, graph, or service exceptions from
 * fixture setup.
 * @note Each exact service-only envelope is proved usable before the real
 * HP/RT executor plans and stages production work. That lower endpoint rejects
 * before registered operation entry; default capacity then commits exactly
 * one output and returns every reserved dimension to zero.
 */
TEST(ExecutionServiceProductResources,
     DirtyHpAndRtRejectSmallLimitAndExecuteWithDefaultLimit) {
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
    operation_calls.store(0, std::memory_order_relaxed);

    const ResourceVector service_only =
        estimate_service_only_resources(graph_identity);
    ASSERT_NO_FATAL_FAILURE(
        expect_service_only_envelope_executes(graph_identity, service_only));

    ScopedResourceRuntimeDirectory directory(is_rt ? "dirty-rt-product"
                                                   : "dirty-hp-product");
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
    request.suppress_graph_downsample = !is_rt;
    HighPrecisionDirtyExecutor hp_executor(traversal, events);
    RealTimeDirtyExecutor rt_executor(traversal, events);

    ExecutionService small_service(1U, execution_limits(service_only));
    ComputeRun small_run(make_dirty_resource_submission(
        graph_identity, graph.topology_generation(), node_id, intent));
    ASSERT_TRUE(small_run.advance_to(ComputeRunPhase::Admitted));
    if (is_rt) {
      EXPECT_THROW((void)rt_executor.execute(
                       graph, runtime.realtime_proxy_graph(), &runtime, request,
                       &small_run, &small_service),
                   GraphError);
    } else {
      EXPECT_THROW((void)hp_executor.execute(
                       graph, runtime.realtime_proxy_graph(), &runtime, request,
                       &small_run, &small_service),
                   GraphError);
    }
    EXPECT_EQ(operation_calls.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(small_service.resource_snapshot().reserved, ResourceVector{});
    ASSERT_TRUE(graph.node(node_id).cached_output_high_precision.has_value());
    EXPECT_EQ(graph.node(node_id)
                  .cached_output_high_precision->data.at("value")
                  .as_int64(),
              3);
    EXPECT_EQ(runtime.realtime_proxy_graph().find_output(node_id), nullptr);

    ComputeRunLease retained_small_run = small_run.acquire_lease();
    const std::uint64_t retained_run_bytes =
        retained_small_run.retained_memory_bytes();
    ASSERT_GT(retained_run_bytes, 0U);
    std::atomic_int estimate_entered{0};
    ReadyTaskSubmission run_representative = make_counted_ready_submission(
        retained_small_run, 0U, node_id, estimate_entered);
    const ResourceVector with_run = small_service.estimate_cpu_run_resources(
        run_representative, 1, CpuRunResourceDemand{retained_run_bytes, {}});
    EXPECT_EQ(
        with_run.retained_memory_bytes - service_only.retained_memory_bytes,
        retained_run_bytes);

    ExecutionService large_service(1U);
    ComputeRun large_run(make_dirty_resource_submission(
        graph_identity, graph.topology_generation(), node_id, intent));
    ASSERT_TRUE(large_run.advance_to(ComputeRunPhase::Admitted));
    NodeOutput* output = nullptr;
    if (is_rt) {
      EXPECT_NO_THROW(output = &rt_executor.execute(
                          graph, runtime.realtime_proxy_graph(), &runtime,
                          request, &large_run, &large_service));
    } else {
      EXPECT_NO_THROW(output = &hp_executor.execute(
                          graph, runtime.realtime_proxy_graph(), &runtime,
                          request, &large_run, &large_service));
    }
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(output->data.at("value").as_int64(), is_rt ? 12 : 11);
    EXPECT_EQ(operation_calls.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(large_run.phase(), ComputeRunPhase::CommitPending);
    EXPECT_EQ(large_service.resource_snapshot().reserved, ResourceVector{});
    if (is_rt) {
      ASSERT_NE(runtime.realtime_proxy_graph().find_output(node_id), nullptr);
      EXPECT_EQ(graph.node(node_id)
                    .cached_output_high_precision->data.at("value")
                    .as_int64(),
                3);
    } else {
      ASSERT_TRUE(graph.node(node_id).cached_output_high_precision.has_value());
      EXPECT_EQ(graph.node(node_id)
                    .cached_output_high_precision->data.at("value")
                    .as_int64(),
                11);
    }
  }
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
      make_test_submission(graph_identity, graph.topology_generation(), 202));
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
      make_test_submission(graph_identity, graph.topology_generation(), 202));
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
                         SchedulerTaskRuntime& runtime) {
        execution_order.push_back(1);
        runtime.dec_tasks_to_complete();
      },
      SchedulerTaskPriority::Normal, kDemand);
  ready.emplace_back(
      std::move(high_lease), high_identity, 2, true,
      [&execution_order, &high_entered, high_release](
          ComputeRunLease&, const ComputeRunTaskIdentity&,
          SchedulerTaskRuntime& runtime) {
        execution_order.push_back(2);
        high_entered.set_value();
        high_release.wait();
        runtime.dec_tasks_to_complete();
      },
      SchedulerTaskPriority::High, kDemand);

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
        service.execute_cpu_run(host, std::move(ready), 2, run_demand);
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
          SchedulerTaskRuntime& runtime) {
        auto& ready_runtime =
            dynamic_cast<ReadyTaskSubmissionRuntime&>(runtime);
        const ComputeRunTaskIdentity dependent_identity =
            retained_lease.task_identity(1U);
        ready_runtime.submit_ready_submission(ReadyTaskSubmission(
            retained_lease, dependent_identity, 2, false,
            [&entered](ComputeRunLease&, const ComputeRunTaskIdentity&,
                       SchedulerTaskRuntime& dependent_runtime) {
              entered.fetch_add(1, std::memory_order_relaxed);
              dependent_runtime.dec_tasks_to_complete();
            },
            SchedulerTaskPriority::Normal, kDemand));
        dependent_queued.set_value();
        initial_release.wait();
        entered.fetch_add(1, std::memory_order_relaxed);
        runtime.dec_tasks_to_complete();
      },
      SchedulerTaskPriority::Normal, kDemand);

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
        service.execute_cpu_run(host, std::move(ready), 2, run_demand);
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
 * @brief Verifies independent Run batches overlap on one fixed service pool.
 *
 * @return Nothing.
 * @throws std::bad_alloc when Run, callback, future, or trace storage cannot
 * allocate.
 * @throws std::future_error when test synchronization state is invalid.
 * @throws std::system_error when async launch or synchronization fails.
 * @note The first callback blocks after entering its worker. The second caller
 * signals immediately before calling `execute_cpu_run()`. With two fixed
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
                               SchedulerTaskRuntime& runtime) {
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
                        SchedulerTaskRuntime& runtime) {
        second_entered.store(true, std::memory_order_release);
        runtime.dec_tasks_to_complete();
      });

  std::future<void> first_future;
  std::future<void> second_future;
  ScopedPromiseRelease release_guard(release_first);
  first_future = std::async(
      std::launch::async,
      [&service, &first_host, ready = std::move(first_ready)]() mutable {
        service.execute_cpu_run(first_host, std::move(ready), 1);
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
        service.execute_cpu_run(second_host, std::move(ready), 1);
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
  EXPECT_EQ(first_traces.front().action, SchedulerTraceAction::AssignInitial);
  EXPECT_EQ(first_traces.front().node_id, 63);
  EXPECT_GE(first_traces.front().worker_id, 0);
  EXPECT_LT(first_traces.front().worker_id, 2);
  EXPECT_EQ(first_traces.front().epoch, first.descriptor().id().value());
  EXPECT_NE(first_traces.front().epoch, second.descriptor().id().value());
  EXPECT_EQ(second_traces.front().action, SchedulerTraceAction::AssignInitial);
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
  EXPECT_EQ(hp_child.descriptor().revision().topology_generation,
            rt_child.descriptor().revision().topology_generation);
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
          SchedulerTaskRuntime& runtime) {
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
          SchedulerTaskRuntime& runtime) {
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
                   service.execute_cpu_run(host, std::move(ready), 1);
                 });
  rt_future =
      std::async(std::launch::async,
                 [&service, &host, ready = std::move(rt_ready)]() mutable {
                   service.execute_cpu_run(host, std::move(ready), 1);
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
  EXPECT_EQ(hp_observation.topology_generation,
            hp_child.descriptor().revision().topology_generation);
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
  EXPECT_EQ(rt_observation.topology_generation,
            rt_child.descriptor().revision().topology_generation);
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
    EXPECT_EQ(trace.action, SchedulerTraceAction::AssignInitial);
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
      "service-isolated-failure", failing_graph.topology_generation(), 65));
  ComputeRun peer(make_test_submission("service-isolated-peer",
                                       peer_graph.topology_generation(), 66));
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
          SchedulerTaskRuntime&) {
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
                              SchedulerTaskRuntime& runtime) {
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
        service.execute_cpu_run(failing_host, std::move(ready), 1);
      });
  peer_future = std::async(
      std::launch::async,
      [&service, &peer_host, ready = std::move(peer_ready)]() mutable {
        service.execute_cpu_run(peer_host, std::move(ready), 1);
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

  EXPECT_THROW(service.execute_cpu_run(host, std::move(mixed), 2),
               std::invalid_argument);
  EXPECT_EQ(entered.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(host.context_entries(), 0);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
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
      make_test_submission("service-failure", graph.topology_generation(), 81));
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
                               SchedulerTaskRuntime&) {
                       (void)retained_lease.publish_task_failure(
                           retained_identity, failure);
                       std::rethrow_exception(failure);
                     });

  std::exception_ptr observed;
  try {
    service.execute_cpu_run(host, std::move(ready), 1);
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
 * @brief Verifies generic scheduler lanes cannot bypass ready-only admission.
 *
 * @note This is a private implementation contract and changes no installed
 * SchedulerTaskRuntime vtable.
 */
TEST(ExecutionService, RejectsBorrowedHandlesAndAnonymousCallbacks) {
  ExecutionService service;
  EXPECT_THROW(
      service.submit_initial_task_handles({}, 0, SchedulerTaskPriority::Normal),
      std::logic_error);
  EXPECT_THROW(service.submit_ready_task_handles_from_worker(
                   {}, SchedulerTaskPriority::Normal),
               std::logic_error);
  EXPECT_THROW(service.submit_ready_task_any_thread(
                   [] {}, SchedulerTaskPriority::Normal, std::nullopt),
               std::logic_error);
}

/**
 * @brief Verifies a terminal accepted task retires its counted scheduler unit.
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
      make_test_submission("terminal-task", graph.topology_generation(), 31));
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
  ComputeRun run(make_test_submission("terminal-bootstrap",
                                      graph.topology_generation(), 32));
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
