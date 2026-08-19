#include <limits>
#include <list>
#include <memory>
#include <utility>
#include <vector>

#include "compute/execution/execution_service_internal.hpp"

/**
 * @file execution_service_worker.cpp
 * @brief Owns task handles, cancellation, retirement, and worker execution.
 */

namespace ps::compute {

using namespace execution_service_detail;  // NOLINT(build/namespaces)

/** @copydoc ExecutionService::submit_initial_task_handles */
void ExecutionService::submit_initial_task_handles(
    std::vector<ExecutionTaskHandle>&& handles, int total_task_count,
    ExecutionTaskPriority priority) {
  (void)handles;
  (void)total_task_count;
  (void)priority;
  throw std::logic_error(
      "ExecutionService rejects borrowed initial task handles.");
}

/** @copydoc ExecutionService::submit_ready_task_handles_from_worker */
void ExecutionService::submit_ready_task_handles_from_worker(
    std::vector<ExecutionTaskHandle>&& handles,
    ExecutionTaskPriority priority) {
  (void)handles;
  (void)priority;
  throw std::logic_error(
      "ExecutionService rejects borrowed ready task handles.");
}

/** @copydoc ExecutionService::submit_ready_task_any_thread */
void ExecutionService::submit_ready_task_any_thread(
    Task&& task, ExecutionTaskPriority priority,
    std::optional<std::uint64_t> epoch) {
  (void)task;
  (void)priority;
  (void)epoch;
  throw std::logic_error("ExecutionService rejects anonymous ready callbacks.");
}

/** @copydoc ExecutionService::wait_for_completion */
void ExecutionService::wait_for_completion() {
  throw std::logic_error(
      "ExecutionService requires an explicit Run-scoped completion wait.");
}

/** @copydoc ExecutionService::current_worker_run */
ExecutionService::RunState& ExecutionService::current_worker_run() {
  if (tls_run_state_ == nullptr || tls_worker_id_ < 0) {
    throw std::logic_error(
        "ExecutionService runtime operation requires a worker Run.");
  }
  return *tls_run_state_;
}

/** @copydoc ExecutionService::set_exception */
void ExecutionService::set_exception(std::exception_ptr error) {
  if (!error) {
    return;
  }
  RunState& current = current_worker_run();
  fail_run(current.shared_from_this(), std::move(error));
}

/** @copydoc ExecutionService::inc_tasks_to_complete */
void ExecutionService::inc_tasks_to_complete(int delta) {
  if (delta <= 0) {
    throw std::invalid_argument(
        "ExecutionService completion increment must be positive.");
  }
  RunState& run = current_worker_run();
  std::lock_guard<std::mutex> lock(run.mutex);
  if (run.cancelled || run.first_exception) {
    return;
  }
  if (run.tasks_to_complete > std::numeric_limits<int>::max() - delta) {
    throw std::overflow_error("ExecutionService completion count overflow.");
  }
  run.tasks_to_complete += delta;
}

/** @copydoc ExecutionService::dec_tasks_to_complete */
void ExecutionService::dec_tasks_to_complete() {
  RunState& run = current_worker_run();
  std::lock_guard<std::mutex> lock(run.mutex);
  if (run.cancelled || run.first_exception) {
    return;
  }
  if (run.tasks_to_complete <= 0) {
    throw std::logic_error("ExecutionService completion count underflow.");
  }
  --run.tasks_to_complete;
  if (run.tasks_to_complete == 0) {
    run.settled_cv.notify_all();
  }
}

/** @copydoc ExecutionService::log_event */
void ExecutionService::log_event(ExecutionTraceAction action, int node_id) {
  RunState& run = current_worker_run();
  if (tls_queue_entry_ == nullptr) {
    throw std::logic_error(
        "ExecutionService trace requires one active ready submission.");
  }
  const ReadyTaskSubmission& submission = tls_queue_entry_->submission;
  const ComputeRunTaskIdentity identity = submission.identity();
  const ExecutionTaskAuditIdentity audit_identity{
      submission.metadata().revision().value(), identity.run_id().value(),
      identity.local_task_id().value()};
  run.host->log_event(action, node_id, tls_worker_id_, run.id.value(),
                      audit_identity);
}

/** @copydoc ExecutionService::fail_run */
void ExecutionService::fail_run(const std::shared_ptr<RunState>& run,
                                std::exception_ptr failure) noexcept {
  if (!failure) {
    return;
  }
  std::list<std::shared_ptr<QueueEntry>> retired_entries;
  try {
    {
      std::lock_guard<std::mutex> pool_lock(pool_->mutex);
      std::lock_guard<std::mutex> run_lock(run->mutex);
      if (!run->cancelled && !run->first_exception) {
        run->first_exception = std::move(failure);
      }
      if (!run->cancelled) {
        run->accepting = false;
      }
      if (run->published) {
        (void)pool_->ready_store.erase_run(run, retired_entries);
      }
      pool_->advance_worker_notification_epoch();
    }
    retired_entries.clear();
    pool_->ready_cv.notify_all();
    run->settled_cv.notify_all();
  } catch (...) {
  }
}

/** @copydoc ExecutionService::cancel_run */
void ExecutionService::cancel_run(
    const std::shared_ptr<RunState>& run,
    ComputeRunCancellationReason reason) noexcept {
  std::list<std::shared_ptr<QueueEntry>> retired_entries;
  try {
    {
      std::lock_guard<std::mutex> pool_lock(pool_->mutex);
      std::lock_guard<std::mutex> run_lock(run->mutex);
      if (!run->cancelled) {
        run->cancelled = true;
        run->cancellation_reason = reason;
      }
      run->first_exception = nullptr;
      run->accepting = false;
      if (run->published) {
        (void)pool_->ready_store.erase_run(run, retired_entries);
      }
      pool_->advance_worker_notification_epoch();
    }
    retired_entries.clear();
    pool_->ready_cv.notify_all();
    run->settled_cv.notify_all();
  } catch (...) {
  }
}

/** @copydoc ExecutionService::retire_worker_entry */
void ExecutionService::retire_worker_entry(
    std::shared_ptr<QueueEntry>& entry,
    const std::shared_ptr<RunState>& run) noexcept {
  try {
    const DeviceBackend device = entry->submission.metadata().device();
    {
      std::lock_guard<std::mutex> pool_lock(pool_->mutex);
      std::lock_guard<std::mutex> run_lock(run->mutex);
      if (run->in_flight <= 0) {
        std::terminate();
      }
      entry->execution_grant.reset();
      if (!pool_->physical_routes.finish(run->route, device)) {
        std::terminate();
      }
      if (!entry->operation_gate_started) {
        std::terminate();
      }
      pool_->operation_gate.finish(entry->submission.operation_constraints());
      entry->operation_gate_started = false;
      pool_->advance_worker_notification_epoch();
    }

    entry.reset();
    if (worker_entry_retirement_observer_ != nullptr) {
      worker_entry_retirement_observer_(
          worker_entry_retirement_observer_context_, run->id);
    }

    bool settled = false;
    {
      std::lock_guard<std::mutex> pool_lock(pool_->mutex);
      std::lock_guard<std::mutex> run_lock(run->mutex);
      --run->in_flight;
      settled = run->settled();
    }
    if (settled) {
      run->settled_cv.notify_all();
    }
    pool_->ready_cv.notify_all();
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc ExecutionService::worker_loop */
void ExecutionService::worker_loop(
    int worker_id, execution::PhysicalExecutionLane lane) noexcept {
  const execution::ComputeIoWaitProhibitionScope compute_io_wait_prohibition(
      lane == execution::PhysicalExecutionLane::Cpu);
  for (;;) {
    std::shared_ptr<QueueEntry> entry;
    std::exception_ptr selection_failure;
    std::shared_ptr<RunState> failed_run;
    unsigned int obsolete_retries = 0U;
    bool force_builtin = false;
    bool grant_blocked = false;
    std::uint64_t grant_blocked_epoch = 0U;

    while (!entry && !selection_failure) {
      BoundedReadyStore::PolicySnapshot snapshot;
      std::shared_ptr<policy::PolicyBinding> binding;
      {
        std::unique_lock<std::mutex> lock(pool_->mutex);
        if (grant_blocked && !pool_->ready_store.has_scheduler_selectable_work(
                                 worker_id, lane, pool_->physical_routes)) {
          const std::uint64_t observed_epoch = grant_blocked_epoch;
          if (!pool_->stopping &&
              pool_->worker_notification_epoch == observed_epoch) {
            (void)pool_->ready_cv.wait_for(
                lock, kGrantRetryBackoff, [this, observed_epoch]() {
                  return pool_->stopping ||
                         pool_->worker_notification_epoch != observed_epoch;
                });
          }
          if (pool_->stopping) {
            return;
          }
          pool_->ready_store.clear_grant_blocked(worker_id);
          grant_blocked = false;
          obsolete_retries = 0U;
          force_builtin = false;
          failed_run.reset();
          continue;
        }
        pool_->ready_cv.wait(lock, [this, worker_id, lane]() {
          return pool_->stopping ||
                 pool_->ready_store.has_scheduler_selectable_work(
                     worker_id, lane, pool_->physical_routes);
        });
        if (pool_->stopping) {
          return;
        }

        if (!force_builtin) {
          try {
            snapshot = pool_->ready_store.make_policy_snapshot(
                worker_id, lane, pool_->physical_routes);
          } catch (const std::bad_alloc&) {
            force_builtin = true;
          } catch (...) {
            selection_failure = std::current_exception();
          }
          if (!selection_failure && !snapshot.plugin_eligible) {
            force_builtin = true;
          }
          if (!force_builtin && !selection_failure) {
            binding = pool_->binding_for(snapshot.service_class);
          }
        }

        if (force_builtin && !selection_failure) {
          BoundedReadyStore::SelectionPin pin =
              pool_->ready_store.select_builtin_current(worker_id, lane,
                                                        pool_->physical_routes);
          if (!pin.entry) {
            force_builtin = false;
            continue;
          }
          failed_run = pin.entry->run;
          try {
            const BoundedReadyStore::StartResult result =
                pool_->ready_store.commit_start(pin, worker_id, lane,
                                                pool_->physical_routes);
            if (result == BoundedReadyStore::StartResult::Started) {
              entry = std::move(pin.entry);
            } else if (result ==
                       BoundedReadyStore::StartResult::GrantUnavailable) {
              if (pool_->ready_store.mark_grant_blocked(pin, worker_id)) {
                if (!grant_blocked) {
                  grant_blocked_epoch = pool_->worker_notification_epoch;
                }
                grant_blocked = true;
                obsolete_retries = 0U;
                failed_run.reset();
              }
            } else if (result ==
                       BoundedReadyStore::StartResult::IdentityExhausted) {
              selection_failure = std::make_exception_ptr(
                  GraphError(GraphErrc::ComputeError,
                             "ExecutionService start identity exhausted."));
            }
          } catch (...) {
            selection_failure = std::current_exception();
          }
        }
      }
      if (entry || selection_failure || force_builtin) {
        continue;
      }

      bool binding_faulted = false;
      try {
        binding_faulted = binding->fault().has_value();
      } catch (const std::bad_alloc&) {
        force_builtin = true;
        continue;
      } catch (...) {
        selection_failure = std::current_exception();
        continue;
      }
      if (binding_faulted) {
        force_builtin = true;
        continue;
      }

      policy::PolicyInvocationResult decision;
      try {
        decision =
            binding->select(snapshot.candidates, snapshot.snapshot_generation,
                            snapshot.selection_sequence);
      } catch (const std::bad_alloc&) {
        force_builtin = true;
        continue;
      } catch (...) {
        selection_failure = std::current_exception();
        continue;
      }

      if (decision.kind ==
          policy::PolicyInvocationResult::Kind::InvalidPluginDecision) {
        try {
          if (!decision.fault.has_value()) {
            throw GraphError(GraphErrc::ComputeError,
                             "Policy violation omitted its fault snapshot.");
          }
          (void)binding->publish_first_fault(std::move(*decision.fault));
          force_builtin = true;
        } catch (...) {
          selection_failure = std::current_exception();
        }
        continue;
      }
      if (decision.kind ==
          policy::PolicyInvocationResult::Kind::BuiltinViolation) {
        std::lock_guard<std::mutex> lock(pool_->mutex);
        BoundedReadyStore::SelectionPin pin;
        try {
          pin = pool_->ready_store.resolve_current(
              snapshot.candidates.front().candidate_id, snapshot.service_class,
              worker_id, lane, pool_->physical_routes);
        } catch (...) {
        }
        failed_run = pin.entry ? pin.entry->run : nullptr;
        selection_failure = std::make_exception_ptr(GraphError(
            GraphErrc::ComputeError,
            "Trusted built-in policy returned an invalid decision."));
        continue;
      }

      BoundedReadyStore::SelectionPin pin;
      bool obsolete = false;
      {
        std::lock_guard<std::mutex> lock(pool_->mutex);
        const std::shared_ptr<policy::PolicyBinding> current =
            pool_->binding_for(snapshot.service_class);
        if (current.get() != binding.get() ||
            current->generation() != binding->generation()) {
          obsolete = true;
        } else {
          try {
            pin = pool_->ready_store.resolve_current(
                decision.candidate_id, snapshot.service_class, worker_id, lane,
                pool_->physical_routes);
          } catch (const std::bad_alloc&) {
            obsolete = true;
          } catch (...) {
            selection_failure = std::current_exception();
          }
          if (!selection_failure && !pin.entry) {
            obsolete = true;
          }
        }

        if (!obsolete && !selection_failure) {
          failed_run = pin.entry->run;
          try {
            const BoundedReadyStore::StartResult result =
                pool_->ready_store.commit_start(pin, worker_id, lane,
                                                pool_->physical_routes);
            if (result == BoundedReadyStore::StartResult::Started) {
              entry = std::move(pin.entry);
            } else if (result ==
                       BoundedReadyStore::StartResult::GrantUnavailable) {
              if (pool_->ready_store.mark_grant_blocked(pin, worker_id)) {
                if (!grant_blocked) {
                  grant_blocked_epoch = pool_->worker_notification_epoch;
                }
                grant_blocked = true;
                obsolete_retries = 0U;
                failed_run.reset();
              } else {
                obsolete = true;
              }
            } else if (result ==
                       BoundedReadyStore::StartResult::IdentityExhausted) {
              selection_failure = std::make_exception_ptr(
                  GraphError(GraphErrc::ComputeError,
                             "ExecutionService start identity exhausted."));
            } else {
              obsolete = true;
            }
          } catch (...) {
            selection_failure = std::current_exception();
          }
        }
      }
      if (obsolete && !selection_failure && !entry) {
        if (obsolete_retries < 2U) {
          ++obsolete_retries;
        } else {
          force_builtin = true;
        }
      }
    }

    if (grant_blocked) {
      std::lock_guard<std::mutex> lock(pool_->mutex);
      pool_->ready_store.clear_grant_blocked(worker_id);
    }

    if (selection_failure) {
      if (failed_run) {
        fail_run(failed_run, selection_failure);
      }
      continue;
    }
    if (!entry) {
      continue;
    }

    const std::shared_ptr<RunState> run = entry->run;
    const std::shared_ptr<ComputeRunObservationSink>& observation_sink =
        entry->submission.lease_.descriptor().observation_sink();
    if (entry->service_start_coordinate.has_value()) {
      if (observation_sink == nullptr ||
          !entry->service_start_observation.has_value()) {
        std::terminate();
      }
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
      notify_service_start_arbitration_for_testing(
          testing::ServiceStartArbitrationPoint::BeforeServiceStartCallback);
#endif
      observation_sink->on_service_start(
          entry->submission.lease_.descriptor(), entry->submission.identity(),
          entry->policy_service_cost, *entry->service_start_observation,
          *entry->service_start_coordinate);
    }
    try {
      const std::optional<ComputeRunCancellationReason> cancellation =
          entry->submission.lease_.observe_cancellation();
      if (cancellation.has_value()) {
        cancel_run(run, *cancellation);
      }
    } catch (...) {
      fail_run(run, std::current_exception());
    }
    bool skip_callback = false;
    {
      std::lock_guard<std::mutex> lock(run->mutex);
      if (run->cancelled || run->first_exception) {
        skip_callback = true;
      }
    }
    if (skip_callback) {
      retire_worker_entry(entry, run);
      continue;
    }

    tls_service_ = this;
    tls_run_state_ = run.get();
    tls_queue_entry_ = entry.get();
    tls_fence_continuation_gate_.reset();
    tls_worker_id_ = worker_id;
    const ComputeRunTaskIdentity task_identity = entry->submission.identity();
    const ExecutionTaskAuditIdentity audit_identity{
        entry->submission.metadata().revision().value(),
        task_identity.run_id().value(), task_identity.local_task_id().value()};
    run->host->set_task_context(worker_id, run->id.value(), audit_identity);
    lifecycle_telemetry_->increment_physical_counter(
        ExecutionLifecyclePhysicalCounter::EnteredCallback);
    try {
      if (entry->submission.metadata().is_initial_ready()) {
        log_event(ExecutionTraceAction::AssignInitial,
                  entry->submission.metadata().trace_node_id());
      }
      if (entry->submission.metadata().device() == DeviceBackend::CPU) {
        entry->submission.execute(*this);
      } else {
        ReadySubmissionDeviceInvocation invocation(entry->submission, *this,
                                                   pool_->ledger);
        pool_->device_executors.execute(entry->submission.metadata().device(),
                                        invocation);
      }
      const std::optional<ComputeRunCancellationReason> cancellation =
          entry->submission.lease_.observe_cancellation();
      if (cancellation.has_value()) {
        cancel_run(run, *cancellation);
      }
    } catch (...) {
      fail_run(run, std::current_exception());
    }
    lifecycle_telemetry_->decrement_physical_counter(
        ExecutionLifecyclePhysicalCounter::EnteredCallback);
    run->host->clear_task_context();
    std::shared_ptr<FenceContinuationGate> fence_gate =
        std::move(tls_fence_continuation_gate_);
    tls_queue_entry_ = nullptr;
    tls_worker_id_ = -1;
    tls_run_state_ = nullptr;
    tls_service_ = nullptr;
    retire_worker_entry(entry, run);
    release_fence_continuation(fence_gate);
  }
}

}  // namespace ps::compute
