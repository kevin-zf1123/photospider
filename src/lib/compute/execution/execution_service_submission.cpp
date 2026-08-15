#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "compute/execution/execution_service_internal.hpp"

/**
 * @file execution_service_submission.cpp
 * @brief Owns ready submission and fence-continuation publication.
 */

namespace ps::compute {

using namespace execution_service_detail;  // NOLINT(build/namespaces)

/** @copydoc ExecutionService::execute_prepared_run */
void ExecutionService::execute_prepared_run(PreparedExecutionRun prepared) {
  if (!prepared.state_ || prepared.state_->owner != this) {
    throw std::invalid_argument(
        "ExecutionService requires one active local prepared Run.");
  }
  std::unique_ptr<PreparedExecutionRunState> state = std::move(prepared.state_);
  const std::shared_ptr<RunState> run = state->run;
  if (!state->completion_seed.has_value()) {
    throw std::logic_error(
        "Prepared execution Run has no completion-lineage seed.");
  }
  pool_->device_executors.observe_generation(*state->completion_seed);

  {
    std::lock_guard<std::mutex> pool_lock(pool_->mutex);
    if (pool_->configured_workers == 0U || pool_->workers.empty()) {
      throw std::logic_error(
          "ExecutionService worker count is not configured.");
    }
    if (pool_->stopping) {
      throw std::logic_error("ExecutionService is stopping.");
    }
    std::lock_guard<std::mutex> run_lock(run->mutex);
    if (run->cancelled) {
      run->reservation.reset();
      return;
    }
    if (!pool_->ready_store.try_publish_prepared_batch(state->batch)) {
      throw GraphError(
          GraphErrc::ComputeError,
          "ExecutionService bounded ready store rejected initial work.");
    }
    run->published = true;
    pool_->advance_worker_notification_epoch();
  }
  pool_->ready_cv.notify_all();

  std::exception_ptr failure;
  {
    std::unique_lock<std::mutex> lock(run->mutex);
    run->settled_cv.wait(lock, [&run]() { return run->settled(); });
    run->accepting = false;
    failure = run->first_exception;
  }

  {
    std::lock_guard<std::mutex> pool_lock(pool_->mutex);
    std::lock_guard<std::mutex> run_lock(run->mutex);
    pool_->ready_store.retire_run(run);
    run->published = false;
    run->reservation.reset();
  }

  if (failure) {
    std::rethrow_exception(failure);
  }
}

/** @copydoc ExecutionService::execute_run */
void ExecutionService::execute_run(
    ExecutionHostContext& host, const std::string& execution_type,
    std::vector<ReadyTaskSubmission> initial_submissions, int total_task_count,
    CpuRunResourceDemand run_resource_demand) {
  if (!initial_submissions.empty() &&
      initial_submissions.front().lease_.observe_cancellation().has_value()) {
    return;
  }
  execute_prepared_run(prepare_run(host, execution_type,
                                   std::move(initial_submissions),
                                   total_task_count, run_resource_demand));
}

/** @copydoc ExecutionService::release_initial_submission_storage */
void ExecutionService::release_initial_submission_storage(
    std::vector<ReadyTaskSubmission>& submissions) noexcept {
  std::vector<ReadyTaskSubmission> released_storage;
  released_storage.swap(submissions);
}

/** @copydoc ExecutionService::observe_initial_submission_storage */
void ExecutionService::observe_initial_submission_storage(
    const ResourceVector& admitted_resources, std::size_t staged_size,
    std::size_t staged_capacity,
    const std::vector<ReadyTaskSubmission>& submissions) const noexcept {
  if (initial_submission_storage_observer_ == nullptr) {
    return;
  }
  initial_submission_storage_observer_(
      initial_submission_storage_observer_context_, admitted_resources,
      staged_size, staged_capacity, submissions.size(), submissions.capacity());
}

/** @copydoc ExecutionService::make_queue_entry */
std::shared_ptr<ExecutionService::QueueEntry>
ExecutionService::make_queue_entry(const std::shared_ptr<RunState>& run,
                                   ReadyTaskSubmission submission) {
  if (submission.metadata().run_id() != run->id) {
    throw std::invalid_argument(
        "ReadyTaskSubmission does not belong to its routed Run.");
  }
  if (submission.resource_demand() != run->resource_demand) {
    throw GraphError(
        GraphErrc::ComputeError,
        "ReadyTaskSubmission resource declaration differs from Run admission.");
  }
  if (!run->exposes_device(submission.metadata().device())) {
    throw std::invalid_argument(
        "ReadyTaskSubmission device is unavailable on its routed Run.");
  }
  const ResourceVector ready_resources{0U, 0U, 0U, 1U,
                                       run->ready_bytes_per_task};
  if (!run->reservation.has_value()) {
    throw std::logic_error(
        "ReadyTaskSubmission Run reservation is already closed.");
  }
  std::optional<ResourceLedger::Grant> ready_grant =
      run->reservation->try_grant(ready_resources);
  if (!ready_grant.has_value()) {
    throw GraphError(
        GraphErrc::ComputeError,
        "Run reservation cannot grant uniformly estimated ready work.");
  }
  const std::uint64_t policy_service_cost = calculate_policy_service_cost(
      submission.resource_demand(), run->ready_bytes_per_task);
  return std::make_shared<QueueEntry>(
      run, std::move(submission), std::move(*ready_grant), policy_service_cost);
}

/** @copydoc ExecutionService::enqueue_submission */
void ExecutionService::enqueue_submission(const std::shared_ptr<RunState>& run,
                                          ReadyTaskSubmission submission) {
  const std::optional<ComputeRunCancellationReason> cancellation =
      submission.lease_.observe_cancellation();
  if (cancellation.has_value()) {
    cancel_run(run, *cancellation);
    return;
  }
  std::shared_ptr<QueueEntry> entry =
      make_queue_entry(run, std::move(submission));

  {
    std::lock_guard<std::mutex> pool_lock(pool_->mutex);
    if (pool_->stopping) {
      throw std::logic_error("ExecutionService is stopping.");
    }
    if (!pool_->ready_store.owns_run(run)) {
      throw std::logic_error(
          "ExecutionService Run no longer accepts ready work.");
    }
    std::lock_guard<std::mutex> run_lock(run->mutex);
    if (run->cancelled) {
      return;
    }
    if (!run->accepting || run->first_exception) {
      throw std::logic_error(
          "ExecutionService Run no longer accepts ready work.");
    }

    if (!pool_->ready_store.try_push(entry)) {
      throw GraphError(
          GraphErrc::ComputeError,
          "ExecutionService bounded ready store rejected dependent work.");
    }
    pool_->advance_worker_notification_epoch();
  }
  pool_->ready_cv.notify_all();
}

/** @copydoc ExecutionService::submit_ready_submission */
void ExecutionService::submit_ready_submission(ReadyTaskSubmission submission) {
  const ComputeRunId run_id = submission.metadata().run_id();
  if (tls_run_state_ == nullptr || tls_run_state_->id != run_id) {
    throw std::invalid_argument(
        "Dependent ready publication requires the matching worker Run.");
  }
  std::shared_ptr<RunState> run = tls_run_state_->shared_from_this();
  enqueue_submission(run, std::move(submission));
}

/** @copydoc ExecutionService::make_ready_fence_executor */
std::shared_ptr<ReadyFenceExecutor>
ExecutionService::make_ready_fence_executor() {
  if (tls_service_ != this || tls_run_state_ == nullptr ||
      tls_queue_entry_ == nullptr ||
      tls_queue_entry_->run.get() != tls_run_state_) {
    throw std::logic_error(
        "Fence continuation executor requires the matching service worker.");
  }
  if (!tls_fence_continuation_gate_) {
    tls_fence_continuation_gate_ = std::make_shared<FenceContinuationGate>(
        tls_queue_entry_->run, tls_queue_entry_->submission.lease_,
        tls_queue_entry_->submission.identity(),
        tls_queue_entry_->submission.metadata().trace_node_id());
  }
  const std::shared_ptr<FenceContinuationGate> gate =
      tls_fence_continuation_gate_;
  const std::shared_ptr<RunState> run = gate->run;
  ComputeRunLease lease = gate->lease;
  const ComputeRunTaskIdentity identity = gate->identity;
  const int trace_node_id = gate->trace_node_id;
  ServiceReadyFenceExecutor::Submitter submitter =
      [this, gate, run, lease = std::move(lease), identity,
       trace_node_id](ReadyFenceExecutor::Task task) mutable {
        deliver_fence_continuation(gate, run, std::move(lease), identity,
                                   trace_node_id, std::move(task));
      };
  ServiceReadyFenceExecutor::Releaser releaser = [this, run]() noexcept {
    retire_fence_continuation(run);
  };
  retain_fence_continuation(run);
  try {
    return std::make_shared<ServiceReadyFenceExecutor>(std::move(submitter),
                                                       std::move(releaser));
  } catch (...) {
    retire_fence_continuation(run);
    throw;
  }
}

/** @copydoc ExecutionService::retain_fence_continuation */
void ExecutionService::retain_fence_continuation(
    const std::shared_ptr<RunState>& run) {
  if (!run) {
    throw std::invalid_argument(
        "Fence continuation retention requires an active Run.");
  }
  std::lock_guard<std::mutex> lock(run->mutex);
  if (run->pending_fence_continuations == std::numeric_limits<int>::max()) {
    throw std::overflow_error("Pending fence continuation count is exhausted.");
  }
  ++run->pending_fence_continuations;
}

/** @copydoc ExecutionService::retire_fence_continuation */
void ExecutionService::retire_fence_continuation(
    const std::shared_ptr<RunState>& run) noexcept {
  if (!run) {
    return;
  }
  try {
    bool settled = false;
    {
      std::lock_guard<std::mutex> lock(run->mutex);
      if (run->pending_fence_continuations <= 0) {
        std::terminate();
      }
      --run->pending_fence_continuations;
      settled = run->settled();
    }
    if (settled) {
      run->settled_cv.notify_all();
    }
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc ExecutionService::deliver_fence_continuation */
void ExecutionService::deliver_fence_continuation(
    const std::shared_ptr<FenceContinuationGate>& gate,
    const std::shared_ptr<RunState>& run, ComputeRunLease lease,
    ComputeRunTaskIdentity identity, int trace_node_id,
    ReadyFenceExecutor::Task task) noexcept {
  try {
    if (!run) {
      return;
    }
    if (!gate || !task || gate->run != run || gate->identity != identity ||
        gate->trace_node_id != trace_node_id ||
        lease.descriptor().id() != identity.run_id()) {
      fail_run(run, std::make_exception_ptr(std::invalid_argument(
                        "Fence continuation identity is inconsistent.")));
      return;
    }
    bool enqueue_now = false;
    {
      std::lock_guard<std::mutex> lock(gate->mutex);
      if (gate->callback_delivered) {
        fail_run(run, std::make_exception_ptr(std::logic_error(
                          "Fence continuation was delivered more than once.")));
        return;
      }
      gate->callback_delivered = true;
      if (gate->original_retired) {
        enqueue_now = true;
      } else {
        gate->parked_task.emplace(std::move(task));
      }
    }
    if (enqueue_now) {
      enqueue_fence_continuation(gate, std::move(task));
    }
  } catch (...) {
    fail_run(run, std::current_exception());
  }
}

/** @copydoc ExecutionService::release_fence_continuation */
void ExecutionService::release_fence_continuation(
    const std::shared_ptr<FenceContinuationGate>& gate) noexcept {
  if (!gate) {
    return;
  }
  try {
    std::optional<ReadyFenceExecutor::Task> parked;
    {
      std::lock_guard<std::mutex> lock(gate->mutex);
      if (gate->original_retired) {
        fail_run(gate->run,
                 std::make_exception_ptr(std::logic_error(
                     "Fence continuation original task retired twice.")));
        return;
      }
      gate->original_retired = true;
      parked = std::move(gate->parked_task);
    }
    if (parked.has_value()) {
      enqueue_fence_continuation(gate, std::move(*parked));
    }
  } catch (...) {
    fail_run(gate->run, std::current_exception());
  }
}

/** @copydoc ExecutionService::enqueue_fence_continuation */
void ExecutionService::enqueue_fence_continuation(
    const std::shared_ptr<FenceContinuationGate>& gate,
    ReadyFenceExecutor::Task task) noexcept {
  try {
    if (!gate || !gate->run || !task) {
      throw std::invalid_argument(
          "Fence continuation requires complete routed ownership.");
    }
    ReadyTaskSubmission submission(
        gate->lease, gate->identity, gate->trace_node_id, false,
        [task = std::move(task)](ComputeRunLease&,
                                 const ComputeRunTaskIdentity&,
                                 ExecutionTaskRuntime&) mutable { task(); },
        ExecutionTaskPriority::High, gate->run->resource_demand, Device::CPU);
    enqueue_submission(gate->run, std::move(submission));
  } catch (...) {
    if (gate && gate->run) {
      fail_run(gate->run, std::current_exception());
    }
  }
}

}  // namespace ps::compute
