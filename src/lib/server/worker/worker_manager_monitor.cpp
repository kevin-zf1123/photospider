#include <algorithm>
#include <memory>
#include <utility>

#include "server/worker/worker_manager_internal.hpp"

/**
 * @file worker_manager_monitor.cpp
 * @brief Owns the bounded worker control/data-plane supervision state machine.
 */

namespace ps::server {

using namespace worker_manager_detail;  // NOLINT(build/namespaces)

/** @copydoc WorkerManager::Impl::monitor_process */
WorkerManagerCompletion WorkerManager::Impl::monitor_process(
    const std::shared_ptr<Record>& record, ChildProcess* process) {
  if (process == nullptr) {
    throw std::invalid_argument("worker monitor process is null");
  }
  const auto started = std::chrono::steady_clock::now();
  auto heartbeat_deadline =
      checked_worker_deadline(started, options_.heartbeat_timeout);
  const auto runtime_deadline =
      checked_worker_deadline(started, options_.attempt_runtime_timeout);
  std::optional<std::chrono::steady_clock::time_point> cancel_deadline;
  std::optional<std::chrono::steady_clock::time_point> report_deadline;
  std::optional<std::chrono::steady_clock::time_point> eof_deadline;
  std::optional<std::chrono::steady_clock::time_point> post_reap_drain_deadline;
  std::optional<PreparedWorkerReport> candidate_report;
  std::optional<JobAttemptReport> materialized_report;
  std::optional<std::vector<std::byte>> output_archive;
  std::size_t output_expected_bytes = 0U;
  std::size_t output_received_bytes = 0U;
  ArtifactContentHasher output_hasher;
  std::optional<ArtifactContentDigest> output_digest;
  bool cancel_attempted = false;
  bool cancel_delivery_failed = false;
  bool cancel_channel_failed = false;
  bool channel_eof = false;
  bool output_eof = false;
  // Test-only monitor-local order for liveness-eligible Heartbeats.
  std::uint64_t external_heartbeat_ordinal_for_test = 0U;
  WorkerFrameDecoder frame_decoder;

  for (;;) {
    observe_exit(record, process);
    const auto now = std::chrono::steady_clock::now();
    if (process->reaped && candidate_report.has_value() &&
        !candidate_report->output.has_value() && !output_eof) {
      output_eof = true;
      process->data_plane.close_manager_output_descriptor();
    }
    if (process->reaped && !channel_eof &&
        !post_reap_drain_deadline.has_value()) {
      post_reap_drain_deadline =
          checked_worker_deadline(now, options_.post_report_timeout);
    }
    const bool cancel = cancellation_requested(record);
    if (cancel && !cancel_attempted && !process->reaped) {
      cancel_attempted = true;
      cancel_deadline =
          checked_worker_deadline(now, options_.cooperative_cancel_timeout);
      try {
        send_worker_identity(
            process->control.get(), WorkerMessageKind::Cancel, record->identity,
            std::min(checked_worker_deadline(now, options_.io_timeout),
                     *cancel_deadline));
      } catch (...) {
        cancel_delivery_failed = true;
      }
    }
    const auto effective_report_deadline =
        subordinate_ordinary_deadline(report_deadline, cancel_deadline);
    if (cancel_deadline.has_value() && !process->reaped &&
        std::chrono::steady_clock::now() >= *cancel_deadline) {
      if (options_.await_cancel_deadline_zero_exit_for_test) {
        await_pre_signal_zero_exit_for_test(
            process->pid,
            checked_worker_deadline(std::chrono::steady_clock::now(),
                                    options_.terminate_timeout));
        if (candidate_report.has_value() && !output_eof) {
          static_cast<void>(
              drain_output_slice(process, output_archive, output_expected_bytes,
                                 &output_received_bytes, &output_hasher,
                                 &output_eof, &output_digest));
        }
      }
      const TerminateAndReapResult termination =
          terminate_and_reap(record, process);
      if (termination == TerminateAndReapResult::EscalationMatched) {
        return forced_cancellation_completion(
            record->identity,
            (cancel_delivery_failed || cancel_channel_failed)
                ? "cancellation channel failed and manager signal "
                  "escalation reaped the worker"
                : "worker exceeded cooperative cancellation grace; "
                  "manager signal escalation reaped it");
      }
      if (termination ==
          TerminateAndReapResult::ExitedBeforeChannelRevocation) {
        cancel_deadline.reset();
        post_reap_drain_deadline = checked_worker_deadline(
            std::chrono::steady_clock::now(), options_.post_report_timeout);
        continue;
      }
      channel_eof = true;
      continue;
    }
    if (!cancel && !process->reaped && now >= runtime_deadline) {
      terminate_and_reap(record, process);
      return failure_completion(record->identity,
                                JobAttemptFailure::WorkerRuntimeTimeout,
                                "worker exceeded attempt runtime bound");
    }
    if (!cancel && !materialized_report.has_value() && !process->reaped &&
        now >= heartbeat_deadline) {
      terminate_and_reap(record, process);
      return failure_completion(record->identity,
                                JobAttemptFailure::WorkerHeartbeatTimeout,
                                "worker heartbeat deadline expired");
    }
    if (effective_report_deadline.has_value() && !process->reaped &&
        now >= *effective_report_deadline) {
      terminate_and_reap(record, process);
      return failure_completion(
          record->identity, JobAttemptFailure::WorkerProtocol,
          "worker reported but did not close and exit within its bound");
    }

    if (output_eof && candidate_report.has_value() &&
        !materialized_report.has_value() &&
        (!process->reaped || !candidate_report->output.has_value())) {
      if (candidate_report->output.has_value() && !output_digest.has_value()) {
        throw WorkerArtifactDataPlaneError(
            "worker output EOF lacks an incremental digest");
      }
      const ArtifactContentDigest materialized_digest =
          output_digest.value_or(ArtifactContentDigest{});
      materialized_report = process->data_plane.materialize_report(
          std::move(candidate_report->report), candidate_report->output,
          std::move(output_archive), output_received_bytes,
          materialized_digest);
      if (!process->reaped && !cancel_delivery_failed &&
          !cancel_channel_failed) {
        auto acknowledgement_deadline =
            std::min(checked_worker_deadline(std::chrono::steady_clock::now(),
                                             options_.io_timeout),
                     runtime_deadline);
        try {
          send_worker_identity(process->control.get(),
                               WorkerMessageKind::CompletionReady,
                               record->identity, acknowledgement_deadline);
        } catch (const WorkerProtocolTimeout&) {
          throw ManagerFailure(
              JobAttemptFailure::WorkerProtocol,
              "worker completion acknowledgement deadline expired");
        }
      }
      candidate_report.reset();
      report_deadline = checked_worker_deadline(
          std::chrono::steady_clock::now(), options_.post_report_timeout);
    }

    if (process->reaped) {
      if (WIFSIGNALED(process->status)) {
        process->control.reset();
        return wait_status_failure_completion(
            record->identity, JobAttemptFailure::WorkerSignal, process->status);
      }
      if (!WIFEXITED(process->status) || WEXITSTATUS(process->status) != 0) {
        process->control.reset();
        return wait_status_failure_completion(
            record->identity, JobAttemptFailure::WorkerExit, process->status);
      }
      if (channel_eof) {
        process->control.reset();
        if (!candidate_report.has_value() && !materialized_report.has_value()) {
          return failure_completion(
              record->identity, JobAttemptFailure::WorkerChannel,
              cancel_channel_failed
                  ? "worker cancellation channel failed before its terminal "
                    "report"
                  : "worker exited cleanly without one report");
        }
        if (!materialized_report.has_value()) {
          return failure_completion(
              record->identity, JobAttemptFailure::WorkerChannel,
              "worker output stream did not commit before clean exit");
        }
        return report_completion(record->identity,
                                 std::move(*materialized_report));
      }
      if (post_reap_drain_deadline.has_value() &&
          now >= *post_reap_drain_deadline) {
        process->control.reset();
        return failure_completion(
            record->identity, JobAttemptFailure::WorkerChannel,
            "worker channel did not drain after clean exit");
      }
    }

    if (channel_eof) {
      if (!candidate_report.has_value() && !materialized_report.has_value()) {
        const auto terminal_channel_deadline =
            subordinate_ordinary_deadline(eof_deadline, cancel_deadline);
        if (terminal_channel_deadline.has_value() &&
            now >= *terminal_channel_deadline) {
          terminate_and_reap(record, process);
          return failure_completion(
              record->identity, JobAttemptFailure::WorkerChannel,
              "worker channel closed before its terminal report");
        }
        std::this_thread::sleep_for(kSupervisorPollInterval);
        continue;
      }
      std::this_thread::sleep_for(kSupervisorPollInterval);
      continue;
    }

    const bool output_pending =
        !process->reaped && candidate_report.has_value() && !output_eof;
    const bool defer_output =
        output_pending && options_.defer_output_drain_for_test != nullptr &&
        options_.defer_output_drain_for_test->load(std::memory_order_acquire);
    if (defer_output) {
      pollfd descriptor{process->data_plane.manager_output_descriptor(), POLLIN,
                        0};
      const int polled = ::poll(&descriptor, 1U, 0);
      if (polled > 0 && (descriptor.revents & POLLIN) != 0 &&
          options_.output_transfer_paused_for_test != nullptr) {
        options_.output_transfer_paused_for_test->store(
            true, std::memory_order_release);
      }
    }
    const bool output_slice_ready = output_pending && !defer_output;
    auto poll_deadline = output_slice_ready ? now
                                            : checked_worker_deadline(
                                                  now, kSupervisorPollInterval);
    auto acceptance_deadline = std::chrono::steady_clock::time_point::max();
    if (!process->reaped) {
      if (!cancel && !materialized_report.has_value()) {
        acceptance_deadline = std::min(acceptance_deadline, heartbeat_deadline);
      }
      acceptance_deadline = std::min(acceptance_deadline, runtime_deadline);
      if (cancel_deadline.has_value()) {
        acceptance_deadline = std::min(acceptance_deadline, *cancel_deadline);
      }
    }
    if (!process->reaped && effective_report_deadline.has_value()) {
      acceptance_deadline =
          std::min(acceptance_deadline, *effective_report_deadline);
    }
    if (post_reap_drain_deadline.has_value()) {
      acceptance_deadline =
          std::min(acceptance_deadline, *post_reap_drain_deadline);
    }
    poll_deadline = std::min(poll_deadline, acceptance_deadline);
    try {
      if (cancel_attempted && options_.inject_cancel_channel_failure_for_test) {
        throw WorkerChannelError(
            "injected accepted-cancel worker channel failure");
      }
      const WorkerProtocolFrame& frame = frame_decoder.inspect_frame(
          process->control.get(), poll_deadline, acceptance_deadline);
      if (frame.kind == WorkerMessageKind::Heartbeat) {
        if (decode_worker_identity(frame, WorkerMessageKind::Heartbeat) !=
            record->identity) {
          throw WorkerProtocolError(
              "worker heartbeat identity does not match its exact lease");
        }
        const auto heartbeat_accepted_at =
            frame_decoder.accept_frame(acceptance_deadline).accepted_at;
        if (!materialized_report.has_value()) {
          if (!cancel && heartbeat_accepted_at >= runtime_deadline) {
            terminate_and_reap(record, process);
            return failure_completion(record->identity,
                                      JobAttemptFailure::WorkerRuntimeTimeout,
                                      "worker exceeded attempt runtime bound");
          }
          if (!cancel && heartbeat_accepted_at >= heartbeat_deadline) {
            terminate_and_reap(record, process);
            return failure_completion(record->identity,
                                      JobAttemptFailure::WorkerHeartbeatTimeout,
                                      "worker heartbeat deadline expired");
          }
          heartbeat_deadline = checked_worker_deadline(
              heartbeat_accepted_at, options_.heartbeat_timeout);
          if (options_.latest_output_pending_heartbeat_ordinal_for_test !=
              nullptr) {
            ++external_heartbeat_ordinal_for_test;
            if (!process->reaped && candidate_report.has_value() &&
                !output_eof) {
              options_.latest_output_pending_heartbeat_ordinal_for_test->store(
                  external_heartbeat_ordinal_for_test,
                  std::memory_order_release);
            }
          }
        }
        if (options_.first_external_heartbeat_observed_for_test != nullptr) {
          options_.first_external_heartbeat_observed_for_test->store(
              true, std::memory_order_release);
        }
      } else if (frame.kind == WorkerMessageKind::Report) {
        if (candidate_report.has_value() || materialized_report.has_value()) {
          throw WorkerProtocolError("worker sent an extra terminal report");
        }
        PreparedWorkerReport report = decode_worker_report(
            frame, *record->assignment.spec,
            process->data_plane.assignment_metadata().output);
        if (report.report.identity != record->identity) {
          throw WorkerProtocolError(
              "worker report identity does not match its exact lease");
        }
        const auto report_accepted_at =
            frame_decoder.accept_frame(acceptance_deadline).accepted_at;
        if (!cancel && report_accepted_at >= runtime_deadline) {
          terminate_and_reap(record, process);
          return failure_completion(record->identity,
                                    JobAttemptFailure::WorkerRuntimeTimeout,
                                    "worker exceeded attempt runtime bound");
        }
        if (!cancel && report_accepted_at >= heartbeat_deadline) {
          terminate_and_reap(record, process);
          return failure_completion(record->identity,
                                    JobAttemptFailure::WorkerHeartbeatTimeout,
                                    "worker heartbeat deadline expired");
        }
        observe_exit(record, process);
        if (!process->reaped || !report.output.has_value()) {
          output_archive = process->data_plane.prepare_output_archive(
              report.report, report.output);
        }
        output_expected_bytes = report.output.has_value()
                                    ? report.output->descriptor.archive_bytes
                                    : 0U;
        output_received_bytes = 0U;
        if (process->reaped && !report.output.has_value()) {
          output_eof = true;
          process->data_plane.close_manager_output_descriptor();
        }
        candidate_report = std::move(report);
      } else {
        throw WorkerProtocolError(
            "worker sent a message invalid for its active state");
      }
    } catch (const WorkerProtocolTimeout&) {
      if (std::chrono::steady_clock::now() >= acceptance_deadline) {
        continue;
      }
      if (output_slice_ready) {
        const WorkerDataPlaneIoStatus status =
            drain_output_slice(process, output_archive, output_expected_bytes,
                               &output_received_bytes, &output_hasher,
                               &output_eof, &output_digest);
        if (status == WorkerDataPlaneIoStatus::WouldBlock) {
          auto wake_deadline = checked_worker_deadline(
              std::chrono::steady_clock::now(), kSupervisorPollInterval);
          if (!cancel && !materialized_report.has_value()) {
            wake_deadline = std::min(wake_deadline, heartbeat_deadline);
          }
          wake_deadline = std::min(wake_deadline, runtime_deadline);
          if (cancel_deadline.has_value()) {
            wake_deadline = std::min(wake_deadline, *cancel_deadline);
          }
          std::this_thread::sleep_until(wake_deadline);
        }
      }
    } catch (const WorkerProtocolEof&) {
      channel_eof = true;
      eof_deadline = checked_worker_deadline(std::chrono::steady_clock::now(),
                                             options_.post_report_timeout);
    } catch (const WorkerChannelError&) {
      if (!cancel_attempted) {
        throw;
      }
      cancel_channel_failed = true;
      channel_eof = true;
      eof_deadline = checked_worker_deadline(std::chrono::steady_clock::now(),
                                             options_.post_report_timeout);
      if (options_.await_cancel_channel_failure_exit_for_test &&
          !process->reaped) {
        await_any_exit_for_test(
            process->pid,
            checked_worker_deadline(std::chrono::steady_clock::now(),
                                    options_.terminate_timeout));
      }
    }
  }
}

}  // namespace ps::server
