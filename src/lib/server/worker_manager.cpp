/**
 * @file worker_manager.cpp
 * @brief Implements exact one-attempt POSIX worker process supervision.
 */
#include "server/worker_manager.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "server/worker_protocol.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {
namespace {

/** @brief Fixed descriptor received by the execed worker. */
constexpr int kWorkerControlDescriptor = 3;
/** @brief Fixed close-on-exec child setup-status descriptor. */
constexpr int kWorkerExecStatusDescriptor = 4;
/** @brief Supervisor poll cadence for cancel, exit, and deadline checks. */
constexpr std::chrono::milliseconds kSupervisorPollInterval{20};

/**
 * @brief Typed internal exception carrying one trusted manager failure domain.
 * @throws std::bad_alloc when storing the diagnostic exhausts memory.
 */
class ManagerFailure final : public std::runtime_error {
 public:
  /**
   * @brief Creates one manager failure classification.
   * @param failure Closed manager-owned category.
   * @param message Trusted local diagnostic.
   * @throws std::bad_alloc when storing the diagnostic exhausts memory.
   */
  ManagerFailure(JobAttemptFailure failure, const std::string& message)
      : std::runtime_error(message), failure_(failure) {}

  /**
   * @brief Returns the retained closed failure category.
   * @return Manager-owned category.
   * @throws Nothing.
   */
  JobAttemptFailure failure() const noexcept { return failure_; }

 private:
  /** @brief Exact trusted failure category. */
  JobAttemptFailure failure_;
};

/**
 * @brief Unique descriptor owner used only in trusted parent-side code.
 * @throws Nothing for construction, moves, reset, and destruction.
 */
class UniqueFd final {
 public:
  /** @brief Creates an empty descriptor owner. */
  UniqueFd() noexcept = default;

  /**
   * @brief Takes ownership of one descriptor or the invalid sentinel.
   * @param fd Descriptor to close on destruction, or -1.
   */
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}

  /** @brief Closes the retained descriptor when present. */
  ~UniqueFd() noexcept { reset(); }

  /** @brief Prevents duplicate descriptor ownership. */
  UniqueFd(const UniqueFd& other) = delete;
  /** @brief Prevents duplicate descriptor assignment. */
  UniqueFd& operator=(const UniqueFd& other) = delete;

  /**
   * @brief Transfers one descriptor owner.
   * @param other Source owner cleared by the move.
   */
  UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

  /**
   * @brief Replaces this descriptor with one transferred owner.
   * @param other Source owner cleared by the move.
   * @return This owner.
   */
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  /**
   * @brief Returns the retained descriptor without transferring ownership.
   * @return Descriptor or -1.
   */
  int get() const noexcept { return fd_; }

  /**
   * @brief Transfers the descriptor to the caller.
   * @return Prior descriptor or -1.
   */
  int release() noexcept {
    const int result = fd_;
    fd_ = -1;
    return result;
  }

  /**
   * @brief Closes the current descriptor and takes an optional replacement.
   * @param replacement New descriptor or -1.
   */
  void reset(int replacement = -1) noexcept {
    if (fd_ >= 0) {
      while (::close(fd_) < 0 && errno == EINTR) {
      }
    }
    fd_ = replacement;
  }

 private:
  /** @brief Sole retained descriptor or invalid sentinel. */
  int fd_ = -1;
};

/**
 * @brief Process state retained only by its exact supervision thread.
 * @throws Nothing for value operations.
 */
struct ChildProcess final {
  /** @brief Exact child PID until successful `waitpid`, then -1. */
  pid_t pid = -1;
  /** @brief Parent side of the one-assignment private socket. */
  UniqueFd control;
  /** @brief Whether exact `waitpid` already completed. */
  bool reaped = false;
  /** @brief Exact wait status when `reaped` is true. */
  int status = 0;
};

/**
 * @brief Closed outcome of signaling one exactly retained child PID.
 * @throws Nothing for value operations.
 */
enum class OwnedSignalResult : std::uint8_t {
  /** @brief The kernel accepted signal delivery for the exact owned PID. */
  Delivered,
  /** @brief The exact owned PID no longer names a process. */
  AlreadyExited,
  /** @brief Ownership changed or the kernel rejected signal delivery. */
  Rejected,
};

/**
 * @brief Terminates the authority process after its final reap deadline.
 * @return Never returns.
 * @throws Nothing.
 * @note POSIX provides no bounded blocking reap primitive. Retaining authority
 * while returning without exact `waitpid` would permit zombies, PID reuse, or
 * double reaping, so deadline exhaustion deliberately fail-stops the sole
 * authority process instead.
 */
[[noreturn]] void fail_stop_unreaped_worker() noexcept {
  static constexpr char kMessage[] =
      "photospider WorkerManager fail-stop: exact worker was not reaped "
      "before the SIGKILL deadline\n";
  std::size_t offset = 0U;
  while (offset != sizeof(kMessage) - 1U) {
    const ssize_t written = ::write(STDERR_FILENO, kMessage + offset,
                                    sizeof(kMessage) - 1U - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  std::abort();
}

/**
 * @brief Decides whether owned signal escalation remains terminal authority.
 * @param process Exactly reaped child status.
 * @param term_delivered Whether the kernel accepted owned `SIGTERM` delivery.
 * @param kill_delivered Whether the kernel accepted owned `SIGKILL` delivery.
 * @return True for matching signal death or clean exit after an accepted
 * escalation; false for a pre-existing nonzero or unrelated signal failure.
 * @throws Nothing.
 */
bool escalation_matches_wait_status(const ChildProcess& process,
                                    bool term_delivered,
                                    bool kill_delivered) noexcept {
  if (!process.reaped || (!term_delivered && !kill_delivered)) {
    return false;
  }
  if (WIFSIGNALED(process.status)) {
    const int signal_number = WTERMSIG(process.status);
    return (term_delivered && signal_number == SIGTERM) ||
           (kill_delivered && signal_number == SIGKILL);
  }
  return WIFEXITED(process.status) && WEXITSTATUS(process.status) == 0;
}

/**
 * @brief Sets close-on-exec and nonblocking flags on one parent-created fd.
 * @param fd Valid descriptor.
 * @param nonblocking Whether to add `O_NONBLOCK`.
 * @throws std::system_error on any `fcntl` failure.
 */
void configure_descriptor(int fd, bool nonblocking) {
  const int descriptor_flags = ::fcntl(fd, F_GETFD);
  if (descriptor_flags < 0 ||
      ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "configure worker descriptor close-on-exec");
  }
  if (nonblocking) {
    const int status_flags = ::fcntl(fd, F_GETFL);
    if (status_flags < 0 ||
        ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "configure worker descriptor nonblocking");
    }
  }
}

/**
 * @brief Writes one setup errno to a child status descriptor and exits.
 * @param fd Best available setup-status descriptor.
 * @param error Captured positive errno, normalized to `EIO` when zero.
 * @return Never returns.
 * @note This child-side path uses only async-signal-safe operations.
 */
[[noreturn]] void child_setup_failed(int fd, int error) noexcept {
  const int normalized = error == 0 ? EIO : error;
  const auto* bytes = reinterpret_cast<const unsigned char*>(&normalized);
  std::size_t offset = 0U;
  while (offset != sizeof(normalized)) {
    const ssize_t written =
        ::write(fd, bytes + offset, sizeof(normalized) - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
    } else if (written < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  _exit(127);
}

/**
 * @brief Closes every child descriptor at or above the supplied bound.
 * @param maximum_descriptor Exclusive checked upper bound prepared pre-fork.
 * @note This child-side loop uses only async-signal-safe `close` calls.
 */
void close_child_descriptors(int maximum_descriptor) noexcept {
  for (int fd = kWorkerExecStatusDescriptor + 1; fd < maximum_descriptor;
       ++fd) {
    static_cast<void>(::close(fd));
  }
}

/**
 * @brief Computes one finite descriptor-close upper bound before fork.
 * @return Exclusive positive fd bound no larger than `INT_MAX`.
 * @throws std::system_error when the process limit cannot be queried.
 */
int descriptor_close_bound() {
  rlimit limit{};
  if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "query worker descriptor limit");
  }
  if (limit.rlim_cur == RLIM_INFINITY ||
      limit.rlim_cur > static_cast<rlim_t>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  return std::max(static_cast<int>(limit.rlim_cur),
                  kWorkerExecStatusDescriptor + 1);
}

/**
 * @brief Builds the attempt-derived POSIX address-space soft/hard limit.
 * @param requested_bytes Accepted Job host-memory envelope.
 * @return Limit whose soft bound does not exceed the accepted envelope or the
 * process hard limit.
 * @throws std::system_error when `RLIMIT_AS` cannot be queried.
 */
rlimit worker_address_space_limit(std::uint64_t requested_bytes) {
  rlimit current{};
  if (::getrlimit(RLIMIT_AS, &current) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "query worker address-space limit");
  }
  rlim_t requested = requested_bytes > static_cast<std::uint64_t>(RLIM_INFINITY)
                         ? RLIM_INFINITY
                         : static_cast<rlim_t>(requested_bytes);
  if (current.rlim_max != RLIM_INFINITY) {
    requested = std::min(requested, current.rlim_max);
  }
  current.rlim_cur = requested;
  return current;
}

/**
 * @brief Reads the close-on-exec setup pipe to distinguish exec from failure.
 * @param fd Nonblocking parent read descriptor.
 * @param deadline Absolute startup deadline.
 * @return Empty after successful exec EOF, otherwise child setup errno.
 * @throws ManagerFailure on timeout, truncation, or pipe-system failure.
 */
std::optional<int> read_exec_status(
    int fd, std::chrono::steady_clock::time_point deadline) {
  int child_error = 0;
  auto* bytes = reinterpret_cast<unsigned char*>(&child_error);
  std::size_t offset = 0U;
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      throw ManagerFailure(JobAttemptFailure::WorkerStartup,
                           "worker exec-status deadline expired");
    }
    auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (remaining.count() <= 0) {
      remaining = std::chrono::milliseconds(1);
    }
    const int timeout = remaining.count() > std::numeric_limits<int>::max()
                            ? std::numeric_limits<int>::max()
                            : static_cast<int>(remaining.count());
    pollfd descriptor{fd, POLLIN, 0};
    const int polled = ::poll(&descriptor, 1U, timeout);
    if (polled == 0) {
      continue;
    }
    if (polled < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw ManagerFailure(JobAttemptFailure::WorkerStartup,
                           std::string("worker exec-status poll failed: ") +
                               std::strerror(errno));
    }
    const ssize_t received =
        ::read(fd, bytes + offset, sizeof(child_error) - offset);
    if (received > 0) {
      offset += static_cast<std::size_t>(received);
      if (offset == sizeof(child_error)) {
        return child_error;
      }
      continue;
    }
    if (received == 0) {
      if (offset == 0U) {
        return std::nullopt;
      }
      throw ManagerFailure(JobAttemptFailure::WorkerStartup,
                           "worker exec-status record was truncated");
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      continue;
    }
    throw ManagerFailure(
        JobAttemptFailure::WorkerStartup,
        std::string("worker exec-status read failed: ") + std::strerror(errno));
  }
}

/**
 * @brief Returns a diagnostic for one exact child wait status.
 * @param status Status returned by `waitpid`.
 * @return Stable human-readable exit or signal description.
 * @throws std::bad_alloc when formatting exhausts memory.
 */
std::string wait_status_message(int status) {
  if (WIFEXITED(status)) {
    return "worker exited with status " + std::to_string(WEXITSTATUS(status));
  }
  if (WIFSIGNALED(status)) {
    std::string message =
        "worker died by signal " + std::to_string(WTERMSIG(status));
    if (WTERMSIG(status) == SIGKILL) {
      message.append(" (OOM-compatible SIGKILL)");
    }
    return message;
  }
  return "worker exited with an unsupported wait status";
}

/**
 * @brief Builds one manager-owned terminal completion without a report.
 * @param identity Exact retained assignment identity.
 * @param failure Closed manager failure category.
 * @param message Trusted supervisor diagnostic.
 * @return Complete failure completion.
 * @throws std::bad_alloc when copying values exhausts memory.
 */
WorkerManagerCompletion failure_completion(const AttemptIdentity& identity,
                                           JobAttemptFailure failure,
                                           std::string message) {
  WorkerManagerCompletion completion;
  completion.identity = identity;
  completion.kind = WorkerManagerCompletionKind::Failure;
  completion.failure = failure;
  completion.message = std::move(message);
  return completion;
}

/**
 * @brief Builds forced cancellation after owned signal delivery and reaping.
 * @param identity Exact retained assignment identity.
 * @param message Trusted escalation diagnostic.
 * @return Complete forced-cancellation fact.
 * @throws std::bad_alloc when copying values exhausts memory.
 */
WorkerManagerCompletion forced_cancellation_completion(
    const AttemptIdentity& identity, std::string message) {
  WorkerManagerCompletion completion;
  completion.identity = identity;
  completion.kind = WorkerManagerCompletionKind::ForcedCancellation;
  completion.failure = JobAttemptFailure::WorkerCancellationForced;
  completion.message = std::move(message);
  return completion;
}

}  // namespace

/**
 * @brief Complete private WorkerManager implementation and record registry.
 * @throws Constructor behavior is documented by `WorkerManager`.
 */
class WorkerManager::Impl final {
 public:
  /**
   * @brief One move-only exact assignment/supervision ownership record.
   * @throws Construction may allocate while retaining assignment values.
   */
  struct Record final {
    /**
     * @brief Retains one exact immutable assignment before thread creation.
     * @param value Assignment moved into sole record ownership.
     * @throws Nothing after argument construction.
     */
    explicit Record(JobAssignment value) noexcept
        : identity(value.identity), assignment(std::move(value)) {}

    /** @brief Complete immutable attempt identity used for every action. */
    AttemptIdentity identity;
    /** @brief Exact immutable assignment retained through supervision. */
    JobAssignment assignment;
    /** @brief Sole joinable supervision-thread handle. */
    std::thread supervisor;
    /** @brief Monotonic external cancellation request. */
    bool cancellation_requested = false;
    /** @brief Monotonic manager shutdown request. */
    bool shutdown_requested = false;
    /** @brief Whether callback/terminal processing reached its final tail. */
    bool completed = false;
    /** @brief Exact live child PID, or -1 before spawn/after reaping. */
    pid_t pid = -1;
  };

  /**
   * @brief Validates configuration and starts one internal handle reaper.
   * @param factory Non-null worker factory.
   * @param callbacks Complete service callback set.
   * @param options Bounded manager policy.
   * @param in_process_test_mode Explicit non-installed test execution mode.
   * @throws As `WorkerManager::WorkerManager`.
   */
  Impl(std::shared_ptr<JobAttemptWorkerFactory> factory,
       WorkerManagerCallbacks callbacks, WorkerManagerOptions options,
       bool in_process_test_mode)
      : factory_(std::move(factory)),
        callbacks_(std::move(callbacks)),
        options_(std::move(options)),
        in_process_test_mode_(in_process_test_mode) {
    validate_configuration();
    reaper_ = std::thread(&Impl::reap_supervisors, this);
  }

  /**
   * @brief Drains all records before member teardown.
   * @throws Nothing.
   */
  ~Impl() noexcept { shutdown(); }

  /** @copydoc WorkerManager::start */
  void start(JobAssignment assignment) {
    validate_attempt_identity(assignment.identity);
    if (assignment.spec == nullptr) {
      throw std::invalid_argument("worker manager assignment has no JobSpec");
    }
    validate_job_spec(*assignment.spec);
    auto record = std::make_shared<Record>(std::move(assignment));
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
      record->supervisor = std::thread(&Impl::supervise, this, record);
    } catch (...) {
      records_.erase(inserted.first);
      condition_.notify_all();
      throw;
    }
  }

  /** @copydoc WorkerManager::request_cancel */
  bool request_cancel(const AttemptIdentity& identity) noexcept {
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

  /** @copydoc WorkerManager::owns_attempt */
  bool owns_attempt(const AttemptIdentity& identity) const noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = records_.find(identity.attempt_id.value());
      if (found != records_.end() && found->second->identity == identity) {
        return true;
      }
      return joining_record_ != nullptr &&
             joining_record_->identity == identity;
    } catch (...) {
      return false;
    }
  }

  /** @copydoc WorkerManager::shutdown */
  void shutdown() noexcept {
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

  /** @copydoc WorkerManager::ownership_snapshot */
  WorkerManagerOwnershipSnapshot ownership_snapshot() const noexcept {
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

  /** @copydoc WorkerManager::wait_for_owned_count_at_most */
  bool wait_for_owned_count_at_most(std::size_t maximum_count,
                                    std::chrono::milliseconds timeout) const {
    if (timeout.count() < 0) {
      throw std::invalid_argument("worker ownership wait timeout is negative");
    }
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [&] {
      return records_.size() + (joining_record_ == nullptr ? 0U : 1U) <=
             maximum_count;
    });
  }

 private:
  /**
   * @brief Validates every callback, duration, factory, and product executable.
   * @throws std::invalid_argument for any fail-closed configuration error.
   */
  void validate_configuration() const {
    if (factory_ == nullptr || !callbacks_.begin_assignment ||
        !callbacks_.cancellation_requested || !callbacks_.complete_assignment) {
      throw std::invalid_argument("worker manager configuration is incomplete");
    }
    const bool durations_valid =
        options_.startup_timeout.count() > 0 &&
        options_.heartbeat_interval.count() > 0 &&
        options_.heartbeat_timeout.count() > 0 &&
        options_.attempt_runtime_timeout.count() > 0 &&
        options_.post_report_timeout.count() > 0 &&
        options_.cooperative_cancel_timeout.count() > 0 &&
        options_.terminate_timeout.count() > 0 &&
        options_.kill_reap_timeout.count() > 0 &&
        options_.io_timeout.count() > 0 &&
        options_.heartbeat_interval < options_.heartbeat_timeout;
    if (!durations_valid) {
      throw std::invalid_argument("worker manager duration bounds are invalid");
    }
    if (in_process_test_mode_) {
      return;
    }
    if (!factory_->supports_external_assignment() ||
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

  /**
   * @brief Runs one exact record and contains every exception at thread scope.
   * @param record Shared stable record retained by the registry and thread.
   * @return Nothing after optional completion callback and completed marking.
   * @throws Nothing.
   */
  void supervise(const std::shared_ptr<Record>& record) noexcept {
    std::optional<WorkerManagerCompletion> completion;
    try {
      if (callbacks_.begin_assignment(record->identity)) {
        completion = in_process_test_mode_ ? run_in_process(record)
                                           : run_external_process(record);
      }
    } catch (const ManagerFailure& error) {
      try {
        completion =
            failure_completion(record->identity, error.failure(), error.what());
      } catch (...) {
      }
    } catch (const std::exception& error) {
      try {
        completion = failure_completion(
            record->identity, JobAttemptFailure::WorkerStartup,
            std::string("worker supervision raised: ") + error.what());
      } catch (...) {
      }
    } catch (...) {
      try {
        completion = failure_completion(
            record->identity, JobAttemptFailure::WorkerStartup,
            "worker supervision raised a non-standard exception");
      } catch (...) {
      }
    }
    if (completion.has_value()) {
      try {
        callbacks_.complete_assignment(std::move(*completion));
      } catch (...) {
        // The service callback is specified no-throw. Contain a future broken
        // callback so lifecycle ownership can still reach exact completion.
      }
    }
    mark_completed(record);
  }

  /**
   * @brief Executes the explicit deterministic in-process test marker path.
   * @param record Exact assignment record.
   * @return One report completion without an OS-isolation claim.
   * @throws Allocation failures while constructing completion unchanged.
   */
  WorkerManagerCompletion run_in_process(
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
    WorkerManagerCompletion completion;
    completion.identity = record->identity;
    completion.kind = WorkerManagerCompletionKind::Report;
    completion.report = std::move(report);
    completion.failure = JobAttemptFailure::None;
    return completion;
  }

  /**
   * @brief Forks and immediately execs one fresh exact worker process.
   * @param record Exact assignment record whose PID registry is updated.
   * @return Live child and private parent socket.
   * @throws ManagerFailure for setup, fork, resource-limit, or exec failure.
   * @throws std::system_error for pre-fork descriptor setup failure.
   * @note The fork child performs only async-signal-safe descriptor, limit,
   * status-write, and exec operations using storage prepared before fork.
   */
  ChildProcess spawn_process(const std::shared_ptr<Record>& record) {
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
      throw ManagerFailure(JobAttemptFailure::WorkerStartup,
                           std::string("worker exec-status pipe failed: ") +
                               std::strerror(errno));
    }
    UniqueFd status_read(status_pipe[0]);
    UniqueFd status_write(status_pipe[1]);
    configure_descriptor(status_read.get(), true);
    configure_descriptor(status_write.get(), false);

    const int close_bound = descriptor_close_bound();
    const rlimit address_space = worker_address_space_limit(
        record->assignment.spec->resource_request().host_memory_bytes);
    const std::string executable = options_.worker_executable.string();
    const std::string control_argument = "--control-fd=3";
    const char* const executable_pointer = executable.c_str();
    const char* const control_pointer = control_argument.c_str();

    const pid_t pid = ::fork();
    if (pid < 0) {
      throw ManagerFailure(
          JobAttemptFailure::WorkerStartup,
          std::string("worker fork failed: ") + std::strerror(errno));
    }
    if (pid == 0) {
      const int control_copy = ::fcntl(child_socket.get(), F_DUPFD_CLOEXEC,
                                       kWorkerExecStatusDescriptor + 1);
      if (control_copy < 0) {
        child_setup_failed(status_write.get(), errno);
      }
      const int status_copy = ::fcntl(status_write.get(), F_DUPFD_CLOEXEC,
                                      kWorkerExecStatusDescriptor + 1);
      if (status_copy < 0) {
        child_setup_failed(status_write.get(), errno);
      }
      if (::dup2(control_copy, kWorkerControlDescriptor) < 0) {
        child_setup_failed(status_copy, errno);
      }
      if (::dup2(status_copy, kWorkerExecStatusDescriptor) < 0) {
        child_setup_failed(status_copy, errno);
      }
      if (::fcntl(kWorkerControlDescriptor, F_SETFD, 0) < 0 ||
          ::fcntl(kWorkerExecStatusDescriptor, F_SETFD, FD_CLOEXEC) < 0 ||
          ::setrlimit(RLIMIT_AS, &address_space) != 0) {
        child_setup_failed(kWorkerExecStatusDescriptor, errno);
      }
      close_child_descriptors(close_bound);
      char* const arguments[] = {const_cast<char*>(executable_pointer),
                                 const_cast<char*>(control_pointer), nullptr};
      ::execv(executable_pointer, arguments);
      child_setup_failed(kWorkerExecStatusDescriptor, errno);
    }

    child_socket.reset();
    status_write.reset();
    set_live_pid(record, pid);
    ChildProcess process;
    process.pid = pid;
    process.control = std::move(parent_socket);
    try {
      const std::optional<int> child_error =
          read_exec_status(status_read.get(), std::chrono::steady_clock::now() +
                                                  options_.startup_timeout);
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

  /**
   * @brief Runs the complete assignment/heartbeat/report/exit state machine.
   * @param record Exact immutable attempt record.
   * @return One report, failure, or forced-cancellation completion after reap.
   * @throws ManagerFailure for preparation or spawn failures before a process
   * state machine can classify locally.
   */
  WorkerManagerCompletion run_external_process(
      const std::shared_ptr<Record>& record) {
    ResolvedGraphArtifact graph;
    try {
      graph = factory_->prepare_external_graph(record->assignment);
    } catch (const std::exception& error) {
      throw ManagerFailure(
          JobAttemptFailure::WorkerStartup,
          std::string("external assignment preparation failed: ") +
              error.what());
    }
    PreparedWorkerAssignment prepared{record->assignment, std::move(graph),
                                      options_.heartbeat_interval};
    ChildProcess process = spawn_process(record);
    bool accepted = false;
    try {
      const auto startup_deadline =
          std::chrono::steady_clock::now() + options_.startup_timeout;
      send_worker_assignment(
          process.control.get(), prepared,
          std::min(startup_deadline,
                   std::chrono::steady_clock::now() + options_.io_timeout));
      const WorkerProtocolFrame acceptance =
          read_worker_frame(process.control.get(), startup_deadline);
      if (decode_worker_identity(acceptance,
                                 WorkerMessageKind::AssignmentAccepted) !=
          record->identity) {
        throw WorkerProtocolError(
            "worker acceptance identity does not match its exact lease");
      }
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
      terminate_and_reap(record, &process);
      return failure_completion(record->identity,
                                JobAttemptFailure::WorkerChannel, error.what());
    } catch (const WorkerProtocolEof& error) {
      try {
        if (!process.reaped) {
          static_cast<void>(wait_for_exit_until(
              record, &process,
              std::chrono::steady_clock::now() + options_.post_report_timeout));
        }
      } catch (...) {
        if (!process.reaped) {
          terminate_and_reap(record, &process);
        }
        throw;
      }
      if (process.reaped && WIFSIGNALED(process.status)) {
        process.control.reset();
        return failure_completion(record->identity,
                                  JobAttemptFailure::WorkerSignal,
                                  wait_status_message(process.status));
      }
      if (process.reaped &&
          (!WIFEXITED(process.status) || WEXITSTATUS(process.status) != 0)) {
        process.control.reset();
        return failure_completion(record->identity,
                                  JobAttemptFailure::WorkerExit,
                                  wait_status_message(process.status));
      }
      terminate_and_reap(record, &process);
      return failure_completion(record->identity,
                                JobAttemptFailure::WorkerChannel, error.what());
    } catch (const WorkerProtocolError& error) {
      terminate_and_reap(record, &process);
      return failure_completion(
          record->identity, JobAttemptFailure::WorkerProtocol, error.what());
    } catch (const std::exception& error) {
      terminate_and_reap(record, &process);
      return failure_completion(record->identity,
                                accepted ? JobAttemptFailure::WorkerProtocol
                                         : JobAttemptFailure::WorkerStartup,
                                error.what());
    }
  }

  /**
   * @brief Monitors one accepted worker until exact exit/reap classification.
   * @param record Exact assignment record.
   * @param process Non-null live/reap-tracked child owner.
   * @return One completion only after `process` is reaped.
   * @throws Worker protocol/channel exceptions for the outer classifier.
   * @throws std::invalid_argument when `process` is null.
   * @note Short read slices share one stateful frame decoder. Cancellation-send
   * failure starts the same cooperative deadline and keeps draining worker
   * report/EOF/exit truth; only delivered TERM/KILL escalation yields
   * `ForcedCancellation`.
   */
  WorkerManagerCompletion monitor_process(const std::shared_ptr<Record>& record,
                                          ChildProcess* process) {
    if (process == nullptr) {
      throw std::invalid_argument("worker monitor process is null");
    }
    const auto started = std::chrono::steady_clock::now();
    auto heartbeat_deadline = started + options_.heartbeat_timeout;
    const auto runtime_deadline = started + options_.attempt_runtime_timeout;
    std::optional<std::chrono::steady_clock::time_point> cancel_deadline;
    std::optional<std::chrono::steady_clock::time_point> report_deadline;
    std::optional<std::chrono::steady_clock::time_point> eof_deadline;
    std::optional<JobAttemptReport> candidate_report;
    bool cancel_attempted = false;
    bool cancel_delivery_failed = false;
    bool channel_eof = false;
    WorkerFrameDecoder frame_decoder;

    for (;;) {
      observe_exit(record, process);
      const auto now = std::chrono::steady_clock::now();
      const bool cancel = cancellation_requested(record);
      if (cancel && !cancel_attempted && !process->reaped) {
        cancel_attempted = true;
        cancel_deadline = now + options_.cooperative_cancel_timeout;
        try {
          send_worker_identity(
              process->control.get(), WorkerMessageKind::Cancel,
              record->identity,
              std::min(now + options_.io_timeout, *cancel_deadline));
        } catch (...) {
          cancel_delivery_failed = true;
        }
      }
      if (cancel_deadline.has_value() && !process->reaped &&
          std::chrono::steady_clock::now() >= *cancel_deadline) {
        const bool signal_delivered = terminate_and_reap(record, process);
        if (signal_delivered) {
          return forced_cancellation_completion(
              record->identity,
              cancel_delivery_failed
                  ? "cancellation channel failed and manager signal "
                    "escalation reaped the worker"
                  : "worker exceeded cooperative cancellation grace; "
                    "manager signal escalation reaped it");
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
      if (!cancel && !candidate_report.has_value() && !process->reaped &&
          now >= heartbeat_deadline) {
        terminate_and_reap(record, process);
        return failure_completion(record->identity,
                                  JobAttemptFailure::WorkerHeartbeatTimeout,
                                  "worker heartbeat deadline expired");
      }
      if (report_deadline.has_value() && !process->reaped &&
          now >= *report_deadline) {
        terminate_and_reap(record, process);
        return failure_completion(
            record->identity, JobAttemptFailure::WorkerProtocol,
            "worker reported but did not close and exit within its bound");
      }

      if (process->reaped) {
        if (WIFSIGNALED(process->status)) {
          process->control.reset();
          return failure_completion(record->identity,
                                    JobAttemptFailure::WorkerSignal,
                                    wait_status_message(process->status));
        }
        if (!WIFEXITED(process->status) || WEXITSTATUS(process->status) != 0) {
          process->control.reset();
          return failure_completion(record->identity,
                                    JobAttemptFailure::WorkerExit,
                                    wait_status_message(process->status));
        }
        if (channel_eof) {
          process->control.reset();
          if (!candidate_report.has_value()) {
            return failure_completion(
                record->identity, JobAttemptFailure::WorkerChannel,
                "worker exited cleanly without one report");
          }
          WorkerManagerCompletion completion;
          completion.identity = record->identity;
          completion.kind = WorkerManagerCompletionKind::Report;
          completion.report = std::move(candidate_report);
          completion.failure = JobAttemptFailure::None;
          return completion;
        }
      }

      if (channel_eof) {
        if (!candidate_report.has_value()) {
          if (eof_deadline.has_value() && now >= *eof_deadline) {
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

      auto read_deadline =
          std::chrono::steady_clock::now() + kSupervisorPollInterval;
      if (!cancel && !candidate_report.has_value()) {
        read_deadline = std::min(read_deadline, heartbeat_deadline);
      }
      read_deadline = std::min(read_deadline, runtime_deadline);
      if (cancel_deadline.has_value()) {
        read_deadline = std::min(read_deadline, *cancel_deadline);
      }
      if (report_deadline.has_value()) {
        read_deadline = std::min(read_deadline, *report_deadline);
      }
      try {
        WorkerProtocolFrame frame =
            frame_decoder.read_frame(process->control.get(), read_deadline);
        if (candidate_report.has_value()) {
          throw WorkerProtocolError(
              "worker sent an extra frame after its terminal report");
        }
        if (frame.kind == WorkerMessageKind::Heartbeat) {
          if (decode_worker_identity(frame, WorkerMessageKind::Heartbeat) !=
              record->identity) {
            throw WorkerProtocolError(
                "worker heartbeat identity does not match its exact lease");
          }
          heartbeat_deadline =
              std::chrono::steady_clock::now() + options_.heartbeat_timeout;
        } else if (frame.kind == WorkerMessageKind::Report) {
          JobAttemptReport report =
              decode_worker_report(frame, *record->assignment.spec);
          if (report.identity != record->identity) {
            throw WorkerProtocolError(
                "worker report identity does not match its exact lease");
          }
          candidate_report = std::move(report);
          report_deadline =
              std::chrono::steady_clock::now() + options_.post_report_timeout;
        } else {
          throw WorkerProtocolError(
              "worker sent a message invalid for its active state");
        }
      } catch (const WorkerProtocolTimeout&) {
        // The short poll slice exists only to revisit process/deadline state.
      } catch (const WorkerProtocolEof&) {
        channel_eof = true;
        eof_deadline =
            std::chrono::steady_clock::now() + options_.post_report_timeout;
      }
    }
  }

  /**
   * @brief Observes manager/service monotonic cancellation without lock
   * nesting.
   * @param record Exact retained record.
   * @return True for manager request, shutdown, or service fencing intent.
   * @throws Nothing; callback failures conservatively request cancellation.
   */
  bool cancellation_requested(
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

  /**
   * @brief Publishes one exact live PID into its immutable record.
   * @param record Exact retained record.
   * @param pid Positive freshly forked child PID.
   * @throws std::logic_error when record ownership changed unexpectedly.
   */
  void set_live_pid(const std::shared_ptr<Record>& record, pid_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = records_.find(record->identity.attempt_id.value());
    if (pid <= 0 || found == records_.end() || found->second != record ||
        found->second->identity != record->identity || record->pid > 0) {
      throw std::logic_error("worker PID ownership publication failed");
    }
    record->pid = pid;
  }

  /**
   * @brief Clears one PID immediately after exact successful waitpid reaping.
   * @param record Exact retained record.
   * @param pid Exact reaped PID.
   * @throws std::logic_error when retained ownership does not match.
   */
  void clear_reaped_pid(const std::shared_ptr<Record>& record, pid_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (record->pid != pid ||
        record->identity.worker_instance_id !=
            record->assignment.identity.worker_instance_id ||
        record->identity.worker_lease_generation !=
            record->assignment.identity.worker_lease_generation) {
      throw std::logic_error("worker PID reap ownership changed");
    }
    record->pid = -1;
    condition_.notify_all();
  }

  /**
   * @brief Signals only the PID retained by one exact current record.
   * @param record Exact retained record.
   * @param pid Candidate child PID, which must equal retained ownership.
   * @param signal_number `SIGTERM` or `SIGKILL`.
   * @return Whether delivery occurred, the process vanished, or signaling was
   * rejected.
   * @throws Nothing.
   */
  OwnedSignalResult signal_owned(const std::shared_ptr<Record>& record,
                                 pid_t pid, int signal_number) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = records_.find(record->identity.attempt_id.value());
    if (pid <= 0 || found == records_.end() || found->second != record ||
        record->pid != pid || found->second->identity != record->identity) {
      return OwnedSignalResult::Rejected;
    }
    if (::kill(pid, signal_number) == 0) {
      return OwnedSignalResult::Delivered;
    }
    return errno == ESRCH ? OwnedSignalResult::AlreadyExited
                          : OwnedSignalResult::Rejected;
  }

  /**
   * @brief Performs one nonblocking exact waitpid observation.
   * @param record Exact retained record.
   * @param process Non-null child state.
   * @return True when this call or an earlier call has reaped the child.
   * @throws std::invalid_argument when `process` is null.
   * @throws ManagerFailure for waitpid errors other than interruption.
   * @note The source-private test gate may suppress the syscall to exercise
   * final deadline handling; product configuration leaves that gate null.
   */
  bool observe_exit(const std::shared_ptr<Record>& record,
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
      throw ManagerFailure(
          JobAttemptFailure::WorkerExit,
          std::string("worker waitpid failed: ") + std::strerror(errno));
    }
  }

  /**
   * @brief Waits nonblocking for one exact child until a local deadline.
   * @param record Exact retained record.
   * @param process Non-null child state.
   * @param deadline Absolute monotonic deadline.
   * @return True when reaped before deadline.
   * @throws std::invalid_argument when `process` is null.
   * @throws ManagerFailure from waitpid unchanged.
   */
  bool wait_for_exit_until(const std::shared_ptr<Record>& record,
                           ChildProcess* process,
                           std::chrono::steady_clock::time_point deadline) {
    for (;;) {
      if (observe_exit(record, process)) {
        return true;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return observe_exit(record, process);
      }
      std::this_thread::sleep_until(
          std::min(deadline, now + kSupervisorPollInterval));
    }
  }

  /**
   * @brief Revokes channel, escalates TERM/KILL, and exactly reaps one child.
   * @param record Exact retained assignment record.
   * @param process Non-null process owner.
   * @return True only when authority-owned signal escalation was delivered and
   * remains consistent with the exact wait status; false when channel closure
   * or a pre-existing worker failure allowed exact reaping.
   * @throws ManagerFailure for unrecoverable waitpid failure.
   * @throws std::invalid_argument when `process` is null.
   * @note Every signal revalidates complete record/PID ownership under mutex.
   * Missing the final reap deadline fail-stops the authority process; this
   * function never falls back to an unbounded `waitpid`.
   */
  bool terminate_and_reap(const std::shared_ptr<Record>& record,
                          ChildProcess* process) {
    if (process == nullptr) {
      throw std::invalid_argument("worker termination process is null");
    }
    if (process->reaped) {
      return false;
    }
    process->control.reset();
    if (observe_exit(record, process)) {
      return false;
    }
    const bool term_delivered = signal_owned(record, process->pid, SIGTERM) ==
                                OwnedSignalResult::Delivered;
    if (wait_for_exit_until(
            record, process,
            std::chrono::steady_clock::now() + options_.terminate_timeout)) {
      return escalation_matches_wait_status(*process, term_delivered, false);
    }
    const bool kill_delivered = signal_owned(record, process->pid, SIGKILL) ==
                                OwnedSignalResult::Delivered;
    if (!wait_for_exit_until(
            record, process,
            std::chrono::steady_clock::now() + options_.kill_reap_timeout)) {
      fail_stop_unreaped_worker();
    }
    return escalation_matches_wait_status(*process, term_delivered,
                                          kill_delivered);
  }

  /**
   * @brief Marks one supervision record complete and wakes the handle reaper.
   * @param record Exact retained record whose thread is returning.
   * @throws Nothing.
   */
  void mark_completed(const std::shared_ptr<Record>& record) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      record->completed = true;
      condition_.notify_all();
    } catch (...) {
      std::terminate();
    }
  }

  /**
   * @brief Reports whether any completed supervision handle can be transferred.
   * @return True when one map record is completed.
   * @throws Nothing.
   * @note Caller holds `mutex_`.
   */
  bool has_completed_record_locked() const noexcept {
    return std::any_of(records_.begin(), records_.end(), [](const auto& entry) {
      return entry.second->completed;
    });
  }

  /**
   * @brief Joins completed supervision threads outside the manager mutex.
   * @return Nothing after shutdown and complete record drainage.
   * @throws Nothing.
   */
  void reap_supervisors() noexcept {
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

  /** @brief Shared factory retained through every supervision thread. */
  std::shared_ptr<JobAttemptWorkerFactory> factory_;
  /** @brief Complete callback set invoked without the manager mutex. */
  WorkerManagerCallbacks callbacks_;
  /** @brief Immutable bounded process lifecycle configuration. */
  WorkerManagerOptions options_;
  /** @brief Explicit non-installed deterministic test-only mode. */
  bool in_process_test_mode_ = false;
  /** @brief Serializes exact record/PID/cancel/handle ownership. */
  mutable std::mutex mutex_;
  /** @brief Wakes supervisors, reaper, shutdown, and test observers. */
  mutable std::condition_variable condition_;
  /** @brief JobAttemptId text to exact active/completed record. */
  std::map<std::string, std::shared_ptr<Record>> records_;
  /** @brief Sole record currently joining outside `mutex_`, if any. */
  std::shared_ptr<Record> joining_record_;
  /** @brief Monotonic shutdown and no-new-assignment state. */
  bool shutting_down_ = false;
  /** @brief Sole infrastructure thread that joins supervision handles. */
  std::thread reaper_;
};

/** @copydoc ps::server::WorkerManagerOwnershipSnapshot::total */
std::size_t WorkerManagerOwnershipSnapshot::total() const {
  if (active > std::numeric_limits<std::size_t>::max() - completed ||
      active + completed > std::numeric_limits<std::size_t>::max() - joining) {
    throw std::overflow_error("worker manager ownership snapshot overflowed");
  }
  return active + completed + joining;
}

/** @copydoc ps::server::WorkerManager::WorkerManager */
WorkerManager::WorkerManager(std::shared_ptr<JobAttemptWorkerFactory> factory,
                             WorkerManagerCallbacks callbacks,
                             WorkerManagerOptions options,
                             bool in_process_test_mode)
    : impl_(std::make_unique<Impl>(
          std::move(factory), std::move(callbacks),
          std::move(options),        // NOLINT(whitespace/indent_namespace)
          in_process_test_mode)) {}  // NOLINT(whitespace/indent_namespace)

/** @copydoc ps::server::WorkerManager::~WorkerManager */
WorkerManager::~WorkerManager() noexcept = default;

/** @copydoc ps::server::WorkerManager::start */
void WorkerManager::start(JobAssignment assignment) {
  impl_->start(std::move(assignment));
}

/** @copydoc ps::server::WorkerManager::request_cancel */
bool WorkerManager::request_cancel(const AttemptIdentity& identity) noexcept {
  return impl_->request_cancel(identity);
}

/** @copydoc ps::server::WorkerManager::owns_attempt */
bool WorkerManager::owns_attempt(
    const AttemptIdentity& identity) const noexcept {
  return impl_->owns_attempt(identity);
}

/** @copydoc ps::server::WorkerManager::shutdown */
void WorkerManager::shutdown() noexcept {
  impl_->shutdown();
}

/** @copydoc ps::server::WorkerManager::ownership_snapshot */
WorkerManagerOwnershipSnapshot WorkerManager::ownership_snapshot()
    const noexcept {
  return impl_->ownership_snapshot();
}

/** @copydoc ps::server::WorkerManager::wait_for_owned_count_at_most */
bool WorkerManager::wait_for_owned_count_at_most(
    std::size_t maximum_count, std::chrono::milliseconds timeout) const {
  return impl_->wait_for_owned_count_at_most(maximum_count, timeout);
}

}  // namespace ps::server
