#include <memory>
#include <string>
#include <utility>

#include "server/worker/worker_manager_internal.hpp"

/**
 * @file worker_manager_lifecycle.cpp
 * @brief Owns manager records, completion construction, PID authority, and
 * reaping.
 */

namespace ps::server {

using namespace worker_manager_detail;  // NOLINT(build/namespaces)

/** @copydoc WorkerManager::Impl::Impl */
WorkerManager::Impl::Impl(std::shared_ptr<JobAttemptWorkerFactory> factory,
                          WorkerManagerCallbacks callbacks,
                          WorkerManagerOptions options,
                          bool in_process_test_mode)
    : factory_(std::move(factory)),      // NOLINT(whitespace/indent_namespace)
      callbacks_(std::move(callbacks)),  // NOLINT(whitespace/indent_namespace)
      options_(std::move(options)),      // NOLINT(whitespace/indent_namespace)
      in_process_test_mode_(             // NOLINT(whitespace/indent_namespace)
          in_process_test_mode) {
  validate_configuration();
  reaper_ = std::thread(&Impl::reap_supervisors, this);
}

/** @copydoc WorkerManager::Impl::~Impl */
WorkerManager::Impl::~Impl() noexcept {
  shutdown();
}

/** @copydoc WorkerManager::Impl::start */
void WorkerManager::Impl::start(JobAssignment assignment) {
  validate_attempt_identity(assignment.identity);
  if (assignment.spec == nullptr) {
    throw std::invalid_argument("worker manager assignment has no JobSpec");
  }
  validate_job_spec(*assignment.spec);
  auto record = std::make_shared<Record>(
      std::move(assignment), options_.fail_record_construction_for_test);
  const std::string key = record->identity.attempt_id.value();
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutting_down_) {
    throw std::invalid_argument("worker manager is shutting down");
  }
  const auto inserted = records_.emplace(key, record);
  if (!inserted.second) {
    throw std::logic_error("worker manager attempt identity collided");
  }
  try {
    throw_manager_thread_start_failure_if_armed(record->identity.job_id);
    record->supervisor = std::thread(&Impl::supervise, this, record);
  } catch (...) {
    records_.erase(inserted.first);
    condition_.notify_all();
    throw;
  }
}

/** @copydoc WorkerManager::Impl::request_cancel */
bool WorkerManager::Impl::request_cancel(
    const AttemptIdentity& identity) noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = records_.find(identity.attempt_id.value());
    if (found == records_.end() || found->second->identity != identity ||
        found->second->completed) {
      return false;
    }
    found->second->cancellation_requested = true;
    condition_.notify_all();
    return true;
  } catch (...) {
    return false;
  }
}

/** @copydoc WorkerManager::Impl::owns_attempt */
bool WorkerManager::Impl::owns_attempt(
    const AttemptIdentity& identity) const noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = records_.find(identity.attempt_id.value());
    if (found != records_.end() && found->second->identity == identity) {
      return true;
    }
    return joining_record_ != nullptr && joining_record_->identity == identity;
  } catch (...) {
    return false;
  }
}

/** @copydoc WorkerManager::Impl::shutdown */
void WorkerManager::Impl::shutdown() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!shutting_down_) {
      shutting_down_ = true;
      for (auto& entry : records_) {
        entry.second->shutdown_requested = true;
        entry.second->cancellation_requested = true;
      }
    }
  }
  condition_.notify_all();
  if (reaper_.joinable()) {
    reaper_.join();
  }
}

/** @copydoc WorkerManager::Impl::ownership_snapshot */
WorkerManagerOwnershipSnapshot WorkerManager::Impl::ownership_snapshot()
    const noexcept {  // NOLINT(whitespace/indent_namespace)
  WorkerManagerOwnershipSnapshot snapshot;
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : records_) {
      if (entry.second->completed) {
        ++snapshot.completed;
      } else {
        ++snapshot.active;
      }
      if (entry.second->pid > 0) {
        ++snapshot.live_processes;
      }
    }
    snapshot.joining = joining_record_ == nullptr ? 0U : 1U;
  } catch (...) {
    return {};
  }
  return snapshot;
}

/** @copydoc WorkerManager::Impl::wait_for_owned_count_at_most */
bool WorkerManager::Impl::wait_for_owned_count_at_most(
    std::size_t maximum_count, std::chrono::milliseconds timeout) const {
  if (timeout.count() < 0) {
    throw std::invalid_argument("worker ownership wait timeout is negative");
  }
  std::unique_lock<std::mutex> lock(mutex_);
  return condition_.wait_for(lock, timeout, [&] {
    return records_.size() + (joining_record_ == nullptr ? 0U : 1U) <=
           maximum_count;
  });
}

/** @copydoc WorkerManager::Impl::validate_configuration */
void WorkerManager::Impl::validate_configuration() const {
  if (factory_ == nullptr || !callbacks_.begin_assignment ||
      !callbacks_.cancellation_requested || !callbacks_.complete_assignment) {
    throw std::invalid_argument("worker manager configuration is incomplete");
  }
  static_cast<void>(validate_and_convert_worker_duration(
      options_.startup_timeout, kMaximumWorkerDuration, "startup_timeout"));
  static_cast<void>(validate_and_convert_worker_duration(
      options_.heartbeat_interval, kMaximumWorkerHeartbeatInterval,
      "heartbeat_interval"));
  static_cast<void>(validate_and_convert_worker_duration(
      options_.heartbeat_timeout, kMaximumWorkerDuration, "heartbeat_timeout"));
  static_cast<void>(validate_and_convert_worker_duration(
      options_.attempt_runtime_timeout, kMaximumWorkerDuration,
      "attempt_runtime_timeout"));
  static_cast<void>(validate_and_convert_worker_duration(
      options_.post_report_timeout, kMaximumWorkerDuration,
      "post_report_timeout"));
  static_cast<void>(validate_and_convert_worker_duration(
      options_.cooperative_cancel_timeout, kMaximumWorkerDuration,
      "cooperative_cancel_timeout"));
  static_cast<void>(validate_and_convert_worker_duration(
      options_.terminate_timeout, kMaximumWorkerDuration, "terminate_timeout"));
  static_cast<void>(validate_and_convert_worker_duration(
      options_.kill_reap_timeout, kMaximumWorkerDuration, "kill_reap_timeout"));
  static_cast<void>(validate_and_convert_worker_duration(
      options_.io_timeout, kMaximumWorkerDuration, "io_timeout"));
  if (options_.heartbeat_interval >= options_.heartbeat_timeout) {
    throw std::invalid_argument(
        "heartbeat_interval must be less than heartbeat_timeout");
  }
  if (in_process_test_mode_) {
    return;
  }
  validate_sigchld_reaping_configuration();
  if (!factory_->has_prepared_external_graphs() ||
      options_.worker_executable.empty()) {
    throw std::invalid_argument(
        "product worker factory or executable is not externalizable");
  }
  struct stat executable_status{};
  if (::stat(options_.worker_executable.c_str(), &executable_status) != 0 ||
      !S_ISREG(executable_status.st_mode) ||
      ::access(options_.worker_executable.c_str(), X_OK) != 0) {
    throw std::invalid_argument(
        "product worker executable is absent or not executable");
  }
}

/** @copydoc WorkerManager::Impl::supervise */
void WorkerManager::Impl::supervise(
    const std::shared_ptr<Record>& record) noexcept {
  std::optional<WorkerManagerCompletion> completion;
  bool assignment_began = false;
  try {
    assignment_began = callbacks_.begin_assignment(record->identity);
    if (assignment_began) {
      completion = in_process_test_mode_ ? run_in_process(record)
                                         : run_external_process(record);
    }
  } catch (const ManagerFailure& error) {
    try {
      inject_completion_construction_failure_for_test();
      completion =
          failure_completion(record->identity, error.failure(), error.what());
    } catch (...) {
      fail_stop_completion_delivery_lost();
    }
  } catch (const std::exception& error) {
    try {
      inject_completion_construction_failure_for_test();
      completion = prefixed_failure_completion(
          record->identity, JobAttemptFailure::WorkerStartup,
          "worker supervision raised: ", error.what());
    } catch (...) {
      fail_stop_completion_delivery_lost();
    }
  } catch (...) {
    try {
      inject_completion_construction_failure_for_test();
      completion = failure_completion(
          record->identity, JobAttemptFailure::WorkerStartup,
          "worker supervision raised a non-standard exception");
    } catch (...) {
      fail_stop_completion_delivery_lost();
    }
  }
  if (completion.has_value()) {
    try {
      callbacks_.complete_assignment(std::move(*completion));
    } catch (...) {
      fail_stop_completion_delivery_lost();
    }
  } else if (assignment_began) {
    fail_stop_completion_delivery_lost();
  }
  mark_completed(record);
}

/** @copydoc WorkerManager::Impl::cancellation_requested */
bool WorkerManager::Impl::cancellation_requested(
    const std::shared_ptr<Record>& record) const noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (record->cancellation_requested || record->shutdown_requested ||
        shutting_down_) {
      return true;
    }
  }
  try {
    return callbacks_.cancellation_requested(record->identity);
  } catch (...) {
    return true;
  }
}

/** @copydoc WorkerManager::Impl::failure_completion */
WorkerManagerCompletion WorkerManager::Impl::failure_completion(
    const AttemptIdentity& identity, JobAttemptFailure failure,
    const char* message) const noexcept {
  try {
    inject_initial_completion_construction_failure_for_test(
        WorkerManagerCompletionConstructionPointForTest::Failure);
    WorkerManagerCompletion completion;
    completion.identity = identity;
    completion.kind = WorkerManagerCompletionKind::Failure;
    completion.failure = failure;
    completion.message = message == nullptr ? "" : message;
    return completion;
  } catch (...) {
    fail_stop_completion_delivery_lost();
  }
}

/** @copydoc WorkerManager::Impl::prefixed_failure_completion */
WorkerManagerCompletion WorkerManager::Impl::prefixed_failure_completion(
    const AttemptIdentity& identity, JobAttemptFailure failure,
    const char* prefix, const char* detail) const noexcept {
  try {
    inject_initial_completion_construction_failure_for_test(
        WorkerManagerCompletionConstructionPointForTest::Failure);
    WorkerManagerCompletion completion;
    completion.identity = identity;
    completion.kind = WorkerManagerCompletionKind::Failure;
    completion.failure = failure;
    if (prefix != nullptr) {
      completion.message = prefix;
    }
    if (detail != nullptr) {
      completion.message.append(detail);
    }
    return completion;
  } catch (...) {
    fail_stop_completion_delivery_lost();
  }
}

/** @copydoc WorkerManager::Impl::wait_status_failure_completion */
WorkerManagerCompletion WorkerManager::Impl::wait_status_failure_completion(
    const AttemptIdentity& identity, JobAttemptFailure failure,
    int status) const noexcept {
  try {
    inject_initial_completion_construction_failure_for_test(
        WorkerManagerCompletionConstructionPointForTest::Failure);
    WorkerManagerCompletion completion;
    completion.identity = identity;
    completion.kind = WorkerManagerCompletionKind::Failure;
    completion.failure = failure;
    completion.message = wait_status_message(status);
    return completion;
  } catch (...) {
    fail_stop_completion_delivery_lost();
  }
}

/** @copydoc WorkerManager::Impl::forced_cancellation_completion */
WorkerManagerCompletion WorkerManager::Impl::forced_cancellation_completion(
    const AttemptIdentity& identity, const char* message) const noexcept {
  try {
    inject_initial_completion_construction_failure_for_test(
        WorkerManagerCompletionConstructionPointForTest::ForcedCancellation);
    WorkerManagerCompletion completion;
    completion.identity = identity;
    completion.kind = WorkerManagerCompletionKind::ForcedCancellation;
    completion.failure = JobAttemptFailure::WorkerCancellationForced;
    completion.message = message == nullptr ? "" : message;
    return completion;
  } catch (...) {
    fail_stop_completion_delivery_lost();
  }
}

/** @copydoc WorkerManager::Impl::report_completion */
WorkerManagerCompletion WorkerManager::Impl::report_completion(
    const AttemptIdentity& identity, JobAttemptReport&& report) const noexcept {
  try {
    inject_initial_completion_construction_failure_for_test(
        WorkerManagerCompletionConstructionPointForTest::Report);
    WorkerManagerCompletion completion;
    completion.identity = identity;
    completion.kind = WorkerManagerCompletionKind::Report;
    completion.report = std::move(report);
    completion.failure = JobAttemptFailure::None;
    return completion;
  } catch (...) {
    fail_stop_completion_delivery_lost();
  }
}

/** @copydoc
 * WorkerManager::Impl::inject_initial_completion_construction_failure_for_test
 */
void WorkerManager::Impl::
    inject_initial_completion_construction_failure_for_test(  // NOLINT(whitespace/indent_namespace)
        WorkerManagerCompletionConstructionPointForTest
            point)  // NOLINT(whitespace/indent_namespace)
    const {         // NOLINT(whitespace/indent_namespace)
  const auto& gate = options_.fail_initial_completion_construction_for_test;
  if (gate == nullptr) {
    return;
  }
  auto expected = point;
  if (gate->compare_exchange_strong(
          expected, WorkerManagerCompletionConstructionPointForTest::None,
          std::memory_order_acq_rel, std::memory_order_acquire)) {
    throw std::bad_alloc();
  }
}

/** @copydoc
 * WorkerManager::Impl::inject_completion_construction_failure_for_test */
void WorkerManager::Impl::inject_completion_construction_failure_for_test()
    const {  // NOLINT(whitespace/indent_namespace)
  if (options_.fail_completion_construction_for_test != nullptr &&
      options_.fail_completion_construction_for_test->exchange(
          false, std::memory_order_acq_rel)) {
    throw std::bad_alloc();
  }
}

/** @copydoc WorkerManager::Impl::set_live_pid */
void WorkerManager::Impl::set_live_pid(const std::shared_ptr<Record>& record,
                                       pid_t pid) noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = records_.find(record->identity.attempt_id.value());
    if (pid <= 0 || found == records_.end() || found->second != record ||
        found->second->identity != record->identity || record->pid > 0) {
      fail_stop_reaping_authority_lost();
    }
    record->pid = pid;
  } catch (...) {
    fail_stop_reaping_authority_lost();
  }
}

/** @copydoc WorkerManager::Impl::clear_reaped_pid */
void WorkerManager::Impl::clear_reaped_pid(
    const std::shared_ptr<Record>& record, pid_t pid) noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    if (record->pid != pid ||
        record->identity.worker_instance_id !=
            record->assignment.identity.worker_instance_id ||
        record->identity.worker_lease_generation !=
            record->assignment.identity.worker_lease_generation) {
      fail_stop_reaping_authority_lost();
    }
    record->pid = -1;
    condition_.notify_all();
  } catch (...) {
    fail_stop_reaping_authority_lost();
  }
}

/** @copydoc WorkerManager::Impl::signal_owned */
OwnedSignalResult WorkerManager::Impl::signal_owned(
    const std::shared_ptr<Record>& record, pid_t pid,
    int signal_number) noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = records_.find(record->identity.attempt_id.value());
    if (pid <= 0 || found == records_.end() || found->second != record ||
        record->pid != pid || found->second->identity != record->identity) {
      fail_stop_reaping_authority_lost();
    }
    if (::kill(pid, signal_number) == 0) {
      return OwnedSignalResult::Delivered;
    }
    return errno == ESRCH ? OwnedSignalResult::AlreadyExited
                          : OwnedSignalResult::Rejected;
  } catch (...) {
    fail_stop_reaping_authority_lost();
  }
}

/** @copydoc WorkerManager::Impl::observe_exit */
bool WorkerManager::Impl::observe_exit(const std::shared_ptr<Record>& record,
                                       ChildProcess* process) {
  if (process == nullptr) {
    throw std::invalid_argument("worker exit observation is null");
  }
  if (process->reaped) {
    return true;
  }
  if (options_.defer_reap_observation_for_test != nullptr &&
      options_.defer_reap_observation_for_test->load(
          std::memory_order_acquire)) {
    return false;
  }
  for (;;) {
    int status = 0;
    const pid_t waited = ::waitpid(process->pid, &status, WNOHANG);
    if (waited == 0) {
      return false;
    }
    if (waited == process->pid) {
      process->reaped = true;
      process->status = status;
      clear_reaped_pid(record, process->pid);
      return true;
    }
    if (waited < 0 && errno == EINTR) {
      continue;
    }
    fail_stop_reaping_authority_lost();
  }
}

/** @copydoc WorkerManager::Impl::wait_for_exit_until */
bool WorkerManager::Impl::wait_for_exit_until(
    const std::shared_ptr<Record>& record, ChildProcess* process,
    std::chrono::steady_clock::time_point deadline) {
  for (;;) {
    if (observe_exit(record, process)) {
      return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return observe_exit(record, process);
    }
    std::this_thread::sleep_until(std::min(
        deadline, checked_worker_deadline(now, kSupervisorPollInterval)));
  }
}

/** @copydoc WorkerManager::Impl::terminate_and_reap */
TerminateAndReapResult WorkerManager::Impl::terminate_and_reap(
    const std::shared_ptr<Record>& record, ChildProcess* process) {
  if (process == nullptr) {
    throw std::invalid_argument("worker termination process is null");
  }
  if (process->reaped) {
    return process->control.get() >= 0
               ? TerminateAndReapResult::ExitedBeforeChannelRevocation
               : TerminateAndReapResult::ReapedAfterChannelRevocation;
  }
  if (observe_exit(record, process)) {
    return TerminateAndReapResult::ExitedBeforeChannelRevocation;
  }
  if (options_.await_pre_signal_zero_exit_for_test) {
    process->control.reset();
    await_pre_signal_zero_exit_for_test(
        process->pid, checked_worker_deadline(std::chrono::steady_clock::now(),
                                              options_.terminate_timeout));
    if (!observe_exit(record, process)) {
      fail_stop_reaping_authority_lost();
    }
    return TerminateAndReapResult::ReapedAfterChannelRevocation;
  }
  const bool term_delivered = signal_owned(record, process->pid, SIGTERM) ==
                              OwnedSignalResult::Delivered;
  process->control.reset();
  if (options_.await_channel_revocation_exit_for_test) {
    await_any_exit_for_test(
        process->pid, checked_worker_deadline(std::chrono::steady_clock::now(),
                                              options_.terminate_timeout));
  }
  if (wait_for_exit_until(
          record, process,
          checked_worker_deadline(std::chrono::steady_clock::now(),
                                  options_.terminate_timeout))) {
    return escalation_matches_wait_status(*process, term_delivered, false)
               ? TerminateAndReapResult::EscalationMatched
               : TerminateAndReapResult::ReapedAfterChannelRevocation;
  }
  const bool kill_delivered = signal_owned(record, process->pid, SIGKILL) ==
                              OwnedSignalResult::Delivered;
  if (!wait_for_exit_until(
          record, process,
          checked_worker_deadline(std::chrono::steady_clock::now(),
                                  options_.kill_reap_timeout))) {
    fail_stop_unreaped_worker();
  }
  return escalation_matches_wait_status(*process, term_delivered,
                                        kill_delivered)
             ? TerminateAndReapResult::EscalationMatched
             : TerminateAndReapResult::ReapedAfterChannelRevocation;
}

/** @copydoc WorkerManager::Impl::mark_completed */
void WorkerManager::Impl::mark_completed(
    const std::shared_ptr<Record>& record) noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    if (record->pid > 0) {
      fail_stop_reaping_authority_lost();
    }
    record->completed = true;
    condition_.notify_all();
  } catch (...) {
    fail_stop_reaping_authority_lost();
  }
}

/** @copydoc WorkerManager::Impl::has_completed_record_locked */
bool WorkerManager::Impl::has_completed_record_locked() const noexcept {
  return std::any_of(records_.begin(), records_.end(),
                     [](const auto& entry) { return entry.second->completed; });
}

/** @copydoc WorkerManager::Impl::reap_supervisors */
void WorkerManager::Impl::reap_supervisors() noexcept {
  for (;;) {
    std::shared_ptr<Record> record;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock, [this] {
        return has_completed_record_locked() ||
               (shutting_down_ && records_.empty());
      });
      const auto completed = std::find_if(
          records_.begin(), records_.end(),
          [](const auto& entry) { return entry.second->completed; });
      if (completed == records_.end()) {
        return;
      }
      record = completed->second;
      if (record->pid > 0) {
        fail_stop_reaping_authority_lost();
      }
      joining_record_ = record;
      records_.erase(completed);
    }
    if (record->supervisor.joinable()) {
      record->supervisor.join();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      joining_record_.reset();
    }
    condition_.notify_all();
  }
}

}  // namespace ps::server
