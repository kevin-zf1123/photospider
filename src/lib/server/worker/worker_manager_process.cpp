#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "server/worker/worker_manager_internal.hpp"

/**
 * @file worker_manager_process.cpp
 * @brief Owns worker spawn, checkpoint/output transport, and process execution.
 */

namespace ps::server {

using namespace worker_manager_detail;  // NOLINT(build/namespaces)

/** @copydoc WorkerManager::Impl::run_in_process */
WorkerManagerCompletion WorkerManager::Impl::run_in_process(
    const std::shared_ptr<Record>& record) {
  JobAttemptReport report;
  try {
    std::unique_ptr<JobAttemptWorker> worker =
        factory_->create(record->assignment);
    if (worker == nullptr) {
      report.identity = record->identity;
      report.outcome = JobAttemptOutcome::Failed;
      report.settled = false;
      report.failure = JobAttemptFailure::HostSetup;
      report.message = "worker factory returned null";
    } else {
      report = worker->execute(record->assignment, [this, record] {
        return cancellation_requested(record);
      });
    }
  } catch (const std::exception& error) {
    report.identity = record->identity;
    report.outcome = JobAttemptOutcome::Failed;
    report.settled = false;
    report.failure = JobAttemptFailure::Unexpected;
    report.message = std::string("worker execution raised: ") + error.what();
  } catch (...) {
    report.identity = record->identity;
    report.outcome = JobAttemptOutcome::Failed;
    report.settled = false;
    report.failure = JobAttemptFailure::Unexpected;
    report.message = "worker execution raised a non-standard exception";
  }
  return report_completion(record->identity, std::move(report));
}

/** @copydoc WorkerManager::Impl::spawn_process */
ChildProcess WorkerManager::Impl::spawn_process(
    const std::shared_ptr<Record>& record) {
  WorkerArtifactDataPlane data_plane = std::move(record->data_plane);
  int sockets[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    throw ManagerFailure(
        JobAttemptFailure::WorkerStartup,
        std::string("worker socketpair failed: ") + std::strerror(errno));
  }
  UniqueFd parent_socket(sockets[0]);
  UniqueFd child_socket(sockets[1]);
  configure_descriptor(parent_socket.get(), true);
  configure_descriptor(child_socket.get(), true);
#ifdef SO_NOSIGPIPE
  int enabled = 1;
  if (::setsockopt(parent_socket.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                   sizeof(enabled)) != 0 ||
      ::setsockopt(child_socket.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                   sizeof(enabled)) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "configure worker socket SIGPIPE suppression");
  }
#endif
  int status_pipe[2] = {-1, -1};
  if (::pipe(status_pipe) != 0) {
    throw ManagerFailure(
        JobAttemptFailure::WorkerStartup,
        std::string("worker exec-status pipe failed: ") + std::strerror(errno));
  }
  UniqueFd status_read(status_pipe[0]);
  UniqueFd status_write(status_pipe[1]);
  configure_descriptor(status_read.get(), true);
  configure_descriptor(status_write.get(), false);

  const ChildDescriptorClosurePlan descriptor_closure =
      prepare_child_descriptor_closure();
  const rlimit address_space = worker_address_space_limit(
      record->assignment.spec->resource_request().host_memory_bytes);
  const rlimit file_size = worker_file_size_limit(static_cast<std::uint64_t>(
      data_plane.assignment_metadata().output.maximum_payload_bytes));
  const std::string executable = options_.worker_executable.string();
  const WorkerProcessLaunchArguments launch_arguments =
      make_worker_process_launch_arguments(WorkerProcessLaunchOptions{
          kWorkerControlDescriptor, kWorkerCheckpointDataDescriptor,
          kWorkerOutputDataDescriptor, options_.startup_timeout,
          options_.io_timeout});
  const char* const executable_pointer = executable.c_str();
  const char* const control_pointer = launch_arguments.control_fd.c_str();
  const char* const checkpoint_pointer =
      launch_arguments.checkpoint_data_fd.c_str();
  const char* const output_pointer = launch_arguments.output_data_fd.c_str();
  const char* const startup_pointer = launch_arguments.startup_timeout.c_str();
  const char* const io_pointer = launch_arguments.io_timeout.c_str();

  require_sigchld_reaping_authority_before_fork();
  const pid_t pid = ::fork();
  if (pid < 0) {
    throw ManagerFailure(
        JobAttemptFailure::WorkerStartup,
        std::string("worker fork failed: ") + std::strerror(errno));
  }
  if (pid == 0) {
    const int control_copy = ::fcntl(child_socket.get(), F_DUPFD_CLOEXEC,
                                     kWorkerOutputDataDescriptor + 1);
    if (control_copy < 0) {
      child_setup_failed(status_write.get(), errno);
    }
    const int checkpoint_copy =
        ::fcntl(data_plane.worker_checkpoint_descriptor(), F_DUPFD_CLOEXEC,
                kWorkerOutputDataDescriptor + 1);
    if (checkpoint_copy < 0) {
      child_setup_failed(status_write.get(), errno);
    }
    const int output_copy =
        ::fcntl(data_plane.worker_output_descriptor(), F_DUPFD_CLOEXEC,
                kWorkerOutputDataDescriptor + 1);
    if (output_copy < 0) {
      child_setup_failed(status_write.get(), errno);
    }
    const int status_copy = ::fcntl(status_write.get(), F_DUPFD_CLOEXEC,
                                    kWorkerOutputDataDescriptor + 1);
    if (status_copy < 0) {
      child_setup_failed(status_write.get(), errno);
    }
    if (::dup2(control_copy, kWorkerControlDescriptor) < 0) {
      child_setup_failed(status_copy, errno);
    }
    if (::dup2(checkpoint_copy, kWorkerCheckpointDataDescriptor) < 0) {
      child_setup_failed(status_copy, errno);
    }
    if (::dup2(output_copy, kWorkerOutputDataDescriptor) < 0) {
      child_setup_failed(status_copy, errno);
    }
    if (::dup2(status_copy, kWorkerExecStatusDescriptor) < 0) {
      child_setup_failed(status_copy, errno);
    }
    if (::fcntl(kWorkerControlDescriptor, F_SETFD, 0) < 0 ||
        ::fcntl(kWorkerCheckpointDataDescriptor, F_SETFD, 0) < 0 ||
        ::fcntl(kWorkerOutputDataDescriptor, F_SETFD, 0) < 0 ||
        ::fcntl(kWorkerExecStatusDescriptor, F_SETFD, FD_CLOEXEC) < 0 ||
        ::setrlimit(RLIMIT_AS, &address_space) != 0 ||
        ::setrlimit(RLIMIT_FSIZE, &file_size) != 0) {
      child_setup_failed(kWorkerExecStatusDescriptor, errno);
    }
    const int close_error = close_child_descriptors(descriptor_closure);
    if (close_error != 0) {
      child_setup_failed(kWorkerExecStatusDescriptor, close_error);
    }
    char* const arguments[] = {const_cast<char*>(executable_pointer),
                               const_cast<char*>(control_pointer),
                               const_cast<char*>(checkpoint_pointer),
                               const_cast<char*>(output_pointer),
                               const_cast<char*>(startup_pointer),
                               const_cast<char*>(io_pointer),
                               nullptr};
    ::execv(executable_pointer, arguments);
    child_setup_failed(kWorkerExecStatusDescriptor, errno);
  }

  child_socket.reset();
  status_write.reset();
  data_plane.close_worker_descriptors();
  set_live_pid(record, pid);
  ChildProcess process;
  process.pid = pid;
  process.control = std::move(parent_socket);
  process.data_plane = std::move(data_plane);
  try {
    const auto exec_status_started =
        worker_exec_status_now(options_.exec_status_deadline_hooks_for_test);
    const std::optional<int> child_error = read_exec_status(
        status_read.get(),
        checked_worker_deadline(exec_status_started, options_.startup_timeout),
        options_.exec_status_deadline_hooks_for_test);
    status_read.reset();
    if (child_error.has_value()) {
      static_cast<void>(terminate_and_reap(record, &process));
      throw ManagerFailure(JobAttemptFailure::WorkerStartup,
                           std::string("worker setup/exec failed: ") +
                               std::strerror(*child_error));
    }
  } catch (...) {
    status_read.reset();
    if (!process.reaped) {
      terminate_and_reap(record, &process);
    }
    throw;
  }
  return process;
}

/** @copydoc WorkerManager::Impl::transfer_checkpoint */
std::optional<WorkerManagerCompletion> WorkerManager::Impl::transfer_checkpoint(
    const std::shared_ptr<Record>& record, ChildProcess* process,
    std::chrono::steady_clock::time_point deadline) {
  if (process == nullptr) {
    throw std::invalid_argument("worker checkpoint process is null");
  }
  const std::vector<std::byte> payload =
      record->assignment.checkpoint == nullptr
          ? std::vector<std::byte>{}
          : encode_named_value_artifact_set(
                record->assignment.checkpoint->values);
  std::size_t offset = 0U;
  while (offset != payload.size()) {
    if (cancellation_requested(record)) {
      const TerminateAndReapResult termination =
          terminate_and_reap(record, process);
      process->data_plane.close_manager_checkpoint_descriptor();
      if (termination == TerminateAndReapResult::EscalationMatched) {
        return forced_cancellation_completion(
            record->identity,
            "manager cancelled checkpoint transfer and reaped the worker");
      }
      process->control.reset();
      return failure_completion(
          record->identity, JobAttemptFailure::WorkerStartup,
          "worker exited during cancelled checkpoint transfer");
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      throw ManagerFailure(JobAttemptFailure::WorkerStartup,
                           "worker checkpoint transfer deadline expired");
    }
    if (options_.defer_checkpoint_transfer_for_test != nullptr &&
        options_.defer_checkpoint_transfer_for_test->load(
            std::memory_order_acquire)) {
      if (options_.checkpoint_transfer_paused_for_test != nullptr) {
        options_.checkpoint_transfer_paused_for_test->store(
            true, std::memory_order_release);
      }
      std::this_thread::sleep_until(std::min(
          deadline, checked_worker_deadline(now, kSupervisorPollInterval)));
      continue;
    }
    const WorkerDataPlaneIoStatus status =
        process->data_plane.send_checkpoint_chunk(payload, &offset);
    if (status == WorkerDataPlaneIoStatus::Progress) {
      continue;
    }
    if (status == WorkerDataPlaneIoStatus::EndOfStream) {
      throw ManagerFailure(
          JobAttemptFailure::WorkerStartup,
          "worker checkpoint stream closed before exact transfer");
    }
    const auto slice_deadline = std::min(
        deadline, checked_worker_deadline(now, kSupervisorPollInterval));
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        slice_deadline - now);
    if (remaining.count() <= 0) {
      remaining = std::chrono::milliseconds(1);
    }
    pollfd descriptor{process->data_plane.manager_checkpoint_descriptor(),
                      POLLOUT, 0};
    const int polled =
        ::poll(&descriptor, 1U, static_cast<int>(remaining.count()));
    if (polled < 0 && errno != EINTR) {
      throw ManagerFailure(JobAttemptFailure::WorkerStartup,
                           std::string("worker checkpoint poll failed: ") +
                               std::strerror(errno));
    }
  }
  process->data_plane.close_manager_checkpoint_descriptor();
  return std::nullopt;
}

/** @copydoc WorkerManager::Impl::drain_output_slice */
WorkerDataPlaneIoStatus WorkerManager::Impl::drain_output_slice(
    ChildProcess* process, std::optional<std::vector<std::byte>>& archive,
    std::size_t expected_bytes, std::size_t* received_bytes,
    ArtifactContentHasher* hasher, bool* output_eof,
    std::optional<ArtifactContentDigest>* output_digest) {
  if (process == nullptr || received_bytes == nullptr || hasher == nullptr ||
      output_eof == nullptr || output_digest == nullptr ||
      (expected_bytes != 0U && !archive.has_value())) {
    throw std::invalid_argument("worker output drain state is incomplete");
  }
  std::byte* destination = archive.has_value() ? archive->data() : nullptr;
  const std::size_t prior_size = *received_bytes;
  const WorkerDataPlaneIoStatus status =
      process->data_plane.receive_output_chunk(destination, expected_bytes,
                                               received_bytes);
  if (status == WorkerDataPlaneIoStatus::Progress) {
    hasher->update(destination + prior_size, *received_bytes - prior_size);
  } else if (status == WorkerDataPlaneIoStatus::EndOfStream) {
    *output_eof = true;
    *output_digest = hasher->finish();
    process->data_plane.close_manager_output_descriptor();
  }
  return status;
}

/** @copydoc WorkerManager::Impl::run_external_process */
WorkerManagerCompletion WorkerManager::Impl::run_external_process(
    const std::shared_ptr<Record>& record) {
  record->data_plane = WorkerArtifactDataPlane::create(record->assignment);
  ChildProcess process = spawn_process(record);
  bool accepted = false;
  try {
    PreparedWorkerAssignment prepared{
        record->assignment,
        factory_->prepared_external_graph(record->assignment),
        process.data_plane.assignment_metadata(), options_.heartbeat_interval};
    const auto startup_deadline = checked_worker_deadline(
        std::chrono::steady_clock::now(), options_.startup_timeout);
    send_worker_assignment(
        process.control.get(), prepared,
        std::min(startup_deadline,
                 checked_worker_deadline(std::chrono::steady_clock::now(),
                                         options_.io_timeout)));
    std::optional<WorkerManagerCompletion> checkpoint_completion =
        transfer_checkpoint(record, &process, startup_deadline);
    if (checkpoint_completion.has_value()) {
      return std::move(*checkpoint_completion);
    }
    WorkerFrameDecoder acceptance_decoder;
    const WorkerProtocolFrame& acceptance = acceptance_decoder.inspect_frame(
        process.control.get(), startup_deadline);
    if (decode_worker_identity(acceptance,
                               WorkerMessageKind::AssignmentAccepted) !=
        record->identity) {
      throw WorkerProtocolError(
          "worker acceptance identity does not match its exact lease");
    }
    static_cast<void>(acceptance_decoder.accept_frame(startup_deadline));
    accepted = true;
    return monitor_process(record, &process);
  } catch (const ManagerFailure&) {
    if (!process.reaped) {
      terminate_and_reap(record, &process);
    }
    throw;
  } catch (const WorkerProtocolTimeout& error) {
    terminate_and_reap(record, &process);
    return failure_completion(record->identity,
                              accepted
                                  ? JobAttemptFailure::WorkerHeartbeatTimeout
                                  : JobAttemptFailure::WorkerStartup,
                              error.what());
  } catch (const WorkerChannelError& error) {
    if (accepted && options_.await_cancel_channel_failure_exit_for_test &&
        !process.reaped) {
      await_any_exit_for_test(
          process.pid, checked_worker_deadline(std::chrono::steady_clock::now(),
                                               options_.terminate_timeout));
    }
    terminate_and_reap(record, &process);
    return failure_completion(record->identity,
                              JobAttemptFailure::WorkerChannel, error.what());
  } catch (const WorkerProtocolEof& error) {
    try {
      if (!process.reaped) {
        static_cast<void>(wait_for_exit_until(
            record, &process,
            checked_worker_deadline(std::chrono::steady_clock::now(),
                                    options_.post_report_timeout)));
      }
    } catch (...) {
      if (!process.reaped) {
        terminate_and_reap(record, &process);
      }
      throw;
    }
    if (process.reaped && WIFSIGNALED(process.status)) {
      process.control.reset();
      return wait_status_failure_completion(
          record->identity, JobAttemptFailure::WorkerSignal, process.status);
    }
    if (process.reaped &&
        (!WIFEXITED(process.status) || WEXITSTATUS(process.status) != 0)) {
      process.control.reset();
      return wait_status_failure_completion(
          record->identity, JobAttemptFailure::WorkerExit, process.status);
    }
    terminate_and_reap(record, &process);
    return failure_completion(record->identity,
                              JobAttemptFailure::WorkerChannel, error.what());
  } catch (const WorkerProtocolError& error) {
    terminate_and_reap(record, &process);
    return failure_completion(record->identity,
                              JobAttemptFailure::WorkerProtocol, error.what());
  } catch (const std::exception& error) {
    terminate_and_reap(record, &process);
    return failure_completion(record->identity,
                              accepted ? JobAttemptFailure::WorkerProtocol
                                       : JobAttemptFailure::WorkerStartup,
                              error.what());
  }
}

}  // namespace ps::server
