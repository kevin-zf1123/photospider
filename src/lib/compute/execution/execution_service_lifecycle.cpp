#include <memory>
#include <utility>
#include <vector>

#include "compute/execution/execution_service_internal.hpp"

/**
 * @file execution_service_lifecycle.cpp
 * @brief Owns graph lifecycle and service shutdown transitions.
 */

namespace ps::compute {

using namespace execution_service_detail;  // NOLINT(build/namespaces)

/** @copydoc ExecutionService::register_graph_lifecycle */
void ExecutionService::register_graph_lifecycle(
    std::shared_ptr<GraphLifetimeAnchor> anchor) {
  lifecycle_registry_->register_graph(std::move(anchor));
}

/** @copydoc ExecutionService::rollback_graph_lifecycle_registration */
void ExecutionService::rollback_graph_lifecycle_registration(
    GraphInstanceId graph_instance_id) {
  lifecycle_registry_->rollback_graph_registration(graph_instance_id);
}

/** @copydoc ExecutionService::begin_graph_admission */
RunLifecycleAdmissionCandidate ExecutionService::begin_graph_admission(
    GraphInstanceId graph_instance_id) {
  return lifecycle_registry_->begin_graph_admission(graph_instance_id);
}

/** @copydoc ExecutionService::commit_graph_admission */
RunLifecycleAdmissionHandle ExecutionService::commit_graph_admission(
    RunLifecycleAdmissionCandidate candidate, ComputeRunLease run_lease,
    std::shared_ptr<ComputeRequestCancellationSource> cancellation) {
  return lifecycle_registry_->commit_standalone(
      std::move(candidate), std::move(run_lease), std::move(cancellation));
}

/** @copydoc ExecutionService::commit_graph_admission_group */
RunLifecycleAdmissionHandle ExecutionService::commit_graph_admission_group(
    RunLifecycleAdmissionCandidate candidate, RunGroupId run_group_id,
    ComputeRunLease hp_lease, ComputeRunLease rt_lease,
    std::shared_ptr<ComputeRequestCancellationSource> cancellation,
    std::shared_ptr<DirtySiblingCommitGate> sibling_commit_gate) {
  return lifecycle_registry_->commit_realtime_group(
      std::move(candidate), run_group_id, std::move(hp_lease),
      std::move(rt_lease), std::move(cancellation),
      std::move(sibling_commit_gate));
}

/** @copydoc ExecutionService::finalize_graph_admission */
void ExecutionService::finalize_graph_admission(
    RunLifecycleAdmissionHandle& handle) noexcept {
  try {
    lifecycle_registry_->finalize_admission(handle);
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc ExecutionService::permits_visible_commit */
bool ExecutionService::permits_visible_commit(GraphInstanceId graph_instance_id,
                                              ComputeRunId run_id) const {
  return lifecycle_registry_->permits_visible_commit(graph_instance_id, run_id);
}

/** @copydoc ExecutionService::begin_graph_close_lifecycle */
std::uint64_t ExecutionService::begin_graph_close_lifecycle(
    GraphInstanceId graph_instance_id, ComputeRunCancellationReason reason) {
  return lifecycle_registry_->begin_graph_close(graph_instance_id, reason);
}

/** @copydoc ExecutionService::finish_graph_close_lifecycle */
void ExecutionService::finish_graph_close_lifecycle(
    GraphInstanceId graph_instance_id) {
  lifecycle_registry_->finish_graph_close(graph_instance_id);
}

/** @copydoc ExecutionService::retire_graph_supersession_lineages */
std::size_t ExecutionService::retire_graph_supersession_lineages(
    GraphInstanceId graph_instance_id) noexcept {
  try {
    return pool_->device_executors.retire_graph_lineages(
        graph_instance_id.value());
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc ExecutionService::close_graph_lifecycle */
void ExecutionService::close_graph_lifecycle(
    GraphInstanceId graph_instance_id, ComputeRunCancellationReason reason) {
  (void)begin_graph_close_lifecycle(graph_instance_id, reason);
  finish_graph_close_lifecycle(graph_instance_id);
  (void)retire_graph_supersession_lineages(graph_instance_id);
}

/** @copydoc ExecutionService::validate_shutdown_caller */
void ExecutionService::validate_shutdown_caller() const {
  if (tls_service_ == this ||
      policy::PolicyRegistry::callback_active_on_current_thread(this)) {
    throw std::logic_error(
        "ExecutionService shutdown cannot run from its worker or policy "
        "callback.");
  }
}

/** @copydoc ExecutionService::begin_shutdown */
std::uint64_t ExecutionService::begin_shutdown() {
  validate_shutdown_caller();
  return lifecycle_registry_->begin_service_shutdown();
}

/** @copydoc ExecutionService::shutdown */
void ExecutionService::shutdown() {
  validate_shutdown_caller();
  (void)begin_shutdown();

  {
    std::unique_lock<std::mutex> lock(pool_->mutex);
    if (pool_->shutdown_complete) {
      return;
    }
    if (pool_->shutdown_in_progress) {
      pool_->shutdown_cv.wait(
          lock, [this]() { return !pool_->shutdown_in_progress; });
      if (pool_->shutdown_complete) {
        return;
      }
    }
    pool_->shutdown_in_progress = true;
  }

  try {
    lifecycle_registry_->wait_until_empty();
    pool_->compute_io_executor.shutdown();
    const execution::ComputeIoExecutorSnapshot compute_io =
        pool_->compute_io_executor.snapshot();
    if (compute_io.active_tasks != 0U ||
        compute_io.active_planned_bytes != 0U ||
        !compute_io.shutdown_complete) {
      throw std::logic_error(
          "ExecutionService compute-I/O executor did not drain.");
    }
    const ResourceLedger::Snapshot before_stop = pool_->ledger.snapshot();
    if (before_stop.reserved != ResourceVector{}) {
      throw std::logic_error(
          "ExecutionService registry settled before resource ledger zero.");
    }

    std::vector<std::thread> workers;
    std::shared_ptr<policy::PolicyBinding> interactive_binding;
    std::shared_ptr<policy::PolicyBinding> throughput_binding;
    ShutdownWorkerJoinGuard worker_join_guard(workers, *lifecycle_registry_);
    {
      std::lock_guard<std::mutex> lock(pool_->mutex);
      pool_->stopping = true;
      pool_->physical_routes.begin_shutdown();
      if (!pool_->ready_store.empty()) {
        throw std::logic_error(
            "ExecutionService registry settled with ready entries.");
      }
      if (!pool_->operation_gate.empty()) {
        throw std::logic_error(
            "ExecutionService registry settled with active operation gates.");
      }
      pool_->ready_store.clear();
      pool_->advance_worker_notification_epoch();
      workers.swap(pool_->workers);
      interactive_binding = std::move(pool_->interactive_binding);
      throughput_binding = std::move(pool_->throughput_binding);
    }
    pool_->ready_cv.notify_all();

    worker_join_guard.complete();

    interactive_binding.reset();
    throughput_binding.reset();

    {
      std::lock_guard<std::mutex> lock(pool_->mutex);
      if (!pool_->physical_routes.drained()) {
        throw std::logic_error(
            "ExecutionService routes did not drain before worker join.");
      }
      pool_->configured_workers = 0U;
    }
    if (pool_->ledger.snapshot().reserved != ResourceVector{}) {
      throw std::logic_error(
          "ExecutionService resource ledger changed after worker join.");
    }
    if (!lifecycle_telemetry_->physical_counters_zero()) {
      throw std::logic_error(
          "ExecutionService physical lifecycle counters did not retire.");
    }

    ExecutionLifecycleCounters final_counters;
    (void)lifecycle_registry_->mark_service_stopped(final_counters);
    {
      std::lock_guard<std::mutex> lock(pool_->mutex);
      pool_->shutdown_complete = true;
      pool_->shutdown_in_progress = false;
    }
    pool_->shutdown_cv.notify_all();
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(pool_->mutex);
      pool_->shutdown_in_progress = false;
    }
    pool_->shutdown_cv.notify_all();
    throw;
  }
}

}  // namespace ps::compute
