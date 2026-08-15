#pragma once

/**
 * @file worker_manager_posix.hpp
 * @brief Defines source-private POSIX descriptor and child-process support.
 *
 * Inline helpers share process-lifetime test and ownership state across the
 * split WorkerManager translation units without exposing an installed API.
 */

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#endif
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
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "server/single_tenant_job_service_test_access.hpp"  // NOLINT(build/include_subdir)
#include "server/worker/worker_artifact_data_plane.hpp"  // NOLINT(build/include_subdir)
#include "server/worker/worker_manager.hpp"
#include "server/worker/worker_manager_test_access.hpp"  // NOLINT(build/include_subdir)
#include "server/worker/worker_process_launch.hpp"  // NOLINT(build/include_subdir)
#include "server/worker/worker_protocol.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {

namespace worker_manager_detail {

/** @brief Fixed descriptor received by the execed worker. */
constexpr int kWorkerControlDescriptor = 3;
/** @brief Fixed close-on-exec child setup-status descriptor. */
constexpr int kWorkerExecStatusDescriptor = 4;
/** @brief Fixed receive-only checkpoint artifact data-plane descriptor. */
constexpr int kWorkerCheckpointDataDescriptor = 5;
/** @brief Fixed send-only output-stage artifact data-plane descriptor. */
constexpr int kWorkerOutputDataDescriptor = 6;
/** @brief Supervisor poll cadence for cancel, exit, and deadline checks. */
constexpr std::chrono::milliseconds kSupervisorPollInterval{20};

/**
 * @brief Subordinates one ordinary channel deadline to active cancellation.
 * @param ordinary_deadline Optional EOF or candidate-Report deadline.
 * @param cancel_deadline Optional active cooperative-cancellation deadline.
 * @return No deadline when `ordinary_deadline` is absent; otherwise the later
 * of the ordinary and active cancellation deadlines.
 * @throws Nothing.
 * @note This is the sole ordinary-deadline arbitration for both channel EOF
 * and a candidate Report whose worker remains alive. Equal deadlines remain
 * equal, and the monitor checks cancellation before either ordinary branch so
 * manager-owned cancellation settlement wins the tie.
 */
inline std::optional<std::chrono::steady_clock::time_point>
subordinate_ordinary_deadline(
    const std::optional<std::chrono::steady_clock::time_point>&
        ordinary_deadline,
    const std::optional<std::chrono::steady_clock::time_point>&
        cancel_deadline) noexcept {
  if (!ordinary_deadline.has_value() || !cancel_deadline.has_value()) {
    return ordinary_deadline;
  }
  return std::max(*ordinary_deadline, *cancel_deadline);
}

/**
 * @brief Invokes POSIX `close` for one manager-owned descriptor.
 * @param descriptor Nonnegative descriptor whose owner has already changed.
 * @param context Unused callback context.
 * @return The raw `close` result with `errno` preserved on failure.
 * @throws Nothing.
 */
inline int close_manager_descriptor(int descriptor, void* context) noexcept {
  static_cast<void>(context);
  return ::close(descriptor);
}

/**
 * @brief Replaces descriptor ownership and closes the former value.
 * @param descriptor Non-null descriptor owner.
 * @param replacement Replacement descriptor or `-1`.
 * @param close_call Non-null allocation-free close-style callback.
 * @param context Borrowed callback context.
 * @return Nothing after the former descriptor receives one close attempt.
 * @throws Nothing; callers provide valid pointers and a non-throwing callback.
 * @note Ownership changes before `close_call` runs. Its result, including
 * `EINTR`, is ignored because Linux may already have released and reused the
 * numeric descriptor before reporting interruption; retrying could close an
 * unrelated descriptor acquired by another thread.
 */
inline void reset_manager_descriptor(
    int* descriptor, int replacement,
    WorkerManagerTestAccess::DescriptorCloseCall close_call,
    void* context) noexcept {
  const int owned = std::exchange(*descriptor, replacement);
  if (owned >= 0) {
    static_cast<void>(close_call(owned, context));
  }
}

/**
 * @brief Current-thread state for one source-private manager-start injection.
 * @throws Nothing for value operations.
 * @note Capture pointers are non-null only while their non-movable guard is
 * alive on this thread. Product threads leave this state disarmed.
 */
struct ManagerThreadStartFailureInjectionState final {
  /** @brief Guard-owned destination for the rolled-back JobId, or null. */
  std::optional<JobId>* attempted_job_id = nullptr;
  /** @brief Guard-owned proof of successful manager-record insertion. */
  bool* manager_record_inserted_before_failure = nullptr;
  /** @brief Whether the next manager supervision-thread start must raise. */
  bool armed = false;
};

/** @brief Guarantees fault capture cannot replace the injected exception. */
static_assert(std::is_nothrow_move_constructible_v<JobId>);

/**
 * @brief Per-calling-thread deterministic manager-thread start injection.
 * @note Thread-local storage prevents concurrent tests or submitters on other
 * threads from consuming this source-private test arm.
 */
inline thread_local ManagerThreadStartFailureInjectionState
    g_manager_thread_start_failure;  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Arms one current-thread manager supervision-start failure.
 * @param attempted_job_id Non-null guard-owned JobId capture destination.
 * @param record_inserted Non-null guard-owned registry-insertion proof.
 * @return Nothing.
 * @throws std::invalid_argument when either capture destination is null.
 * @throws std::logic_error when another guard already owns this thread's seam.
 */
inline void arm_manager_thread_start_failure(
    std::optional<JobId>* attempted_job_id, bool* record_inserted) {
  if (attempted_job_id == nullptr || record_inserted == nullptr) {
    throw std::invalid_argument("manager-start failure capture is null");
  }
  if (g_manager_thread_start_failure.attempted_job_id != nullptr) {
    throw std::logic_error("manager-start failure injection is already armed");
  }
  attempted_job_id->reset();
  *record_inserted = false;
  g_manager_thread_start_failure.attempted_job_id = attempted_job_id;
  g_manager_thread_start_failure.manager_record_inserted_before_failure =
      record_inserted;
  g_manager_thread_start_failure.armed = true;
}

/**
 * @brief Disarms one matching current-thread manager-start injection guard.
 * @param attempted_job_id Exact JobId capture supplied while arming.
 * @param record_inserted Exact insertion-proof capture supplied while arming.
 * @return Nothing.
 * @throws Nothing.
 * @note Mismatched pointers are ignored so one guard cannot disarm another.
 */
inline void disarm_manager_thread_start_failure(
    const std::optional<JobId>* attempted_job_id,
    const bool* record_inserted) noexcept {
  if (g_manager_thread_start_failure.attempted_job_id != attempted_job_id ||
      g_manager_thread_start_failure.manager_record_inserted_before_failure !=
          record_inserted) {
    return;
  }
  g_manager_thread_start_failure.armed = false;
  g_manager_thread_start_failure.attempted_job_id = nullptr;
  g_manager_thread_start_failure.manager_record_inserted_before_failure =
      nullptr;
}

/**
 * @brief Raises one armed failure at the real manager thread-start boundary.
 * @param attempted_job_id Mutable JobId in the exact inserted manager record;
 * moved into the guard only when the injection is consumed.
 * @return Nothing when the current thread has no armed injection.
 * @throws std::system_error with resource-unavailable status when armed.
 * @throws std::bad_alloc only if constructing the injected diagnostic fails.
 * @note The caller invokes this helper only after `records_.emplace()` and
 * inside the catch boundary that erases that exact record. Consumption occurs
 * before native `std::thread` construction and cannot affect another thread.
 */
inline void throw_manager_thread_start_failure_if_armed(
    JobId& attempted_job_id) {
  if (!g_manager_thread_start_failure.armed) {
    return;
  }
  g_manager_thread_start_failure.armed = false;
  *g_manager_thread_start_failure.manager_record_inserted_before_failure = true;
  g_manager_thread_start_failure.attempted_job_id->emplace(
      std::move(attempted_job_id));
  throw std::system_error(
      std::make_error_code(std::errc::resource_unavailable_try_again),
      "injected manager supervision-thread start failure");
}

/**
 * @brief Immutable platform input for closing inherited child descriptors.
 * @throws Nothing for value construction and copies.
 * @note Darwin needs the kernel-wide exclusive descriptor ceiling prepared
 * before `fork`; Linux performs one unbounded kernel `close_range` operation
 * and therefore leaves the field unused.
 */
struct ChildDescriptorClosurePlan final {
  /** @brief Darwin `kern.maxfilesperproc` exclusive descriptor ceiling. */
  int darwin_exclusive_maximum = 0;
};

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
   * @return Nothing.
   * @throws Nothing; close failures are intentionally ignored.
   * @note Ownership changes before exactly one close attempt. `EINTR` is not
   * retried because the numeric descriptor may already have been reused.
   */
  void reset(int replacement = -1) noexcept {
    reset_manager_descriptor(&fd_, replacement, close_manager_descriptor,
                             nullptr);
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
  /**
   * @brief Parent side of the one-assignment private socket.
   * @note May outlive exact natural reaping only through the monitor's bounded
   * buffered report/EOF drain; revocation clears it before escalation.
   */
  UniqueFd control;
  /**
   * @brief Direction-reduced attempt-local artifact stream lanes.
   * @note Child-facing originals are closed in the manager immediately after
   * fork. Nonblocking manager endpoints remain only through checkpoint commit,
   * candidate EOF/join, or revocation cleanup; no pathname is created.
   */
  WorkerArtifactDataPlane data_plane;
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
 * @brief Closed result of one exact terminate-and-reap transaction.
 * @throws Nothing for value operations.
 * @note `ExitedBeforeChannelRevocation` leaves the manager-owned control
 * descriptor open so its caller can drain report/EOF bytes that the worker
 * committed before normal exit. Every other result follows channel revocation
 * and therefore has no remaining ordinary report-delivery path.
 */
enum class TerminateAndReapResult : std::uint8_t {
  /** @brief Exact natural exit was reaped before channel revocation. */
  ExitedBeforeChannelRevocation,
  /** @brief Exact exit was reaped after the channel had been revoked. */
  ReapedAfterChannelRevocation,
  /** @brief Exact signal death matched one delivered owned escalation. */
  EscalationMatched,
};

/**
 * @brief Writes one allocation-free authority diagnostic and aborts.
 * @param message Non-null trusted null-terminated diagnostic.
 * @return Never returns.
 * @throws Nothing.
 * @note This terminal path performs no callback, record deletion, or ordinary
 * completion publication because exact process authority can no longer be
 * represented safely.
 */
[[noreturn]] inline void fail_stop_worker_authority(
    const char* message) noexcept {
  if (message != nullptr) {
    const std::size_t size = std::strlen(message);
    std::size_t offset = 0U;
    while (offset != size) {
      const ssize_t written =
          ::write(STDERR_FILENO, message + offset, size - offset);
      if (written > 0) {
        offset += static_cast<std::size_t>(written);
        continue;
      }
      if (written < 0 && errno == EINTR) {
        continue;
      }
      break;
    }
  }
  std::abort();
}

/**
 * @brief Terminates the authority process after its final reap deadline.
 * @return Never returns.
 * @throws Nothing.
 * @note POSIX provides no bounded blocking reap primitive. Retaining authority
 * while returning without exact `waitpid` would permit zombies, PID reuse, or
 * double reaping, so deadline exhaustion deliberately fail-stops the sole
 * authority process instead.
 */
[[noreturn]] inline void fail_stop_unreaped_worker() noexcept {
  static constexpr char kMessage[] =
      "photospider WorkerManager fail-stop: exact worker was not reaped "
      "before the SIGKILL deadline\n";
  fail_stop_worker_authority(kMessage);
}

/**
 * @brief Terminates after exact PID/reaping authority becomes unavailable.
 * @return Never returns.
 * @throws Nothing.
 * @note `ECHILD`, another non-retryable `waitpid` error, or an impossible
 * retained-PID transition means no ordinary Job completion can be proven.
 */
[[noreturn]] inline void fail_stop_reaping_authority_lost() noexcept {
  static constexpr char kMessage[] =
      "photospider WorkerManager fail-stop: exact worker reaping authority "
      "was lost\n";
  fail_stop_worker_authority(kMessage);
}

/**
 * @brief Terminates when one required typed terminal fact cannot be retained
 * or delivered.
 * @return Never returns.
 * @throws Nothing.
 * @note The child may already be exactly reaped, but Job/quota reconciliation
 * is still incomplete. This allocation-free path therefore runs before the
 * completion callback can be forged or retried and before the record is marked
 * completed or exposed to the handle reaper.
 */
[[noreturn]] inline void fail_stop_completion_delivery_lost() noexcept {
  static constexpr char kMessage[] =
      "photospider WorkerManager fail-stop: terminal completion fact could "
      "not be constructed or delivered\n";
  fail_stop_worker_authority(kMessage);
}

/**
 * @brief Reports whether one `SIGCHLD` action enables kernel auto-reaping.
 * @param action Process-global action returned by `sigaction`.
 * @return True for `SIG_IGN` or `SA_NOCLDWAIT`.
 * @throws Nothing.
 */
inline bool sigchld_action_auto_reaps(const struct sigaction& action) noexcept {
  if (action.sa_handler == SIG_IGN) {
    return true;
  }
#ifdef SA_NOCLDWAIT
  return (action.sa_flags & SA_NOCLDWAIT) != 0;
#else
  return false;
#endif
}

/**
 * @brief Validates that product construction can retain exact child statuses.
 * @return Nothing when the current `SIGCHLD` disposition preserves waitability.
 * @throws std::system_error when the process action cannot be queried.
 * @throws std::invalid_argument for `SIG_IGN` or `SA_NOCLDWAIT`.
 * @note The caller invokes this before starting any manager thread or worker.
 */
inline void validate_sigchld_reaping_configuration() {
  struct sigaction action{};
  if (::sigaction(SIGCHLD, nullptr, &action) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "query WorkerManager SIGCHLD disposition");
  }
  if (sigchld_action_auto_reaps(action)) {
    throw std::invalid_argument(
        "WorkerManager SIGCHLD disposition must not use SIG_IGN or "
        "SA_NOCLDWAIT");
  }
}

/**
 * @brief Revalidates exact child waitability immediately before `fork`.
 * @return Nothing while the process-wide action still preserves wait status.
 * @throws Nothing.
 * @note A host mutation after successful service construction is an authority
 * violation, not an attempt-local startup failure, and therefore fail-stops.
 */
inline void require_sigchld_reaping_authority_before_fork() noexcept {
  struct sigaction action{};
  if (::sigaction(SIGCHLD, nullptr, &action) != 0 ||
      sigchld_action_auto_reaps(action)) {
    fail_stop_reaping_authority_lost();
  }
}

/**
 * @brief Decides whether owned signal escalation remains terminal authority.
 * @param process Exactly reaped child status.
 * @param term_delivered Whether the kernel accepted owned `SIGTERM` delivery.
 * @param kill_delivered Whether the kernel accepted owned `SIGKILL` delivery.
 * @return True only for signal death matching an accepted owned escalation;
 * false for every normal exit, pre-existing failure, or unrelated signal.
 * @throws Nothing.
 * @note `kill()` success does not prove causality for a zombie. With no worker
 * TERM-exit handshake, a normal zero exit remains ordinary report/channel/exit
 * truth even when the kernel accepted a later signal request for that PID.
 */
inline bool escalation_matches_wait_status(const ChildProcess& process,
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
  return false;
}

/**
 * @brief Waits until a child has exited zero without consuming its wait status.
 * @param pid Exact positive child PID still owned by the calling supervisor.
 * @param deadline Absolute monotonic test-only observation deadline.
 * @return Nothing once `waitid(WNOWAIT)` proves a normal zero exit.
 * @throws ManagerFailure when the child exits abnormally or the deterministic
 * test deadline expires.
 * @throws std::overflow_error if the monotonic clock cannot represent the next
 * bounded poll slice.
 * @note This source-private seam is reached only when explicitly enabled by a
 * test option. The later production `waitpid` remains the sole exact reaper;
 * inability to observe this owned child fail-stops as authority loss.
 */
inline void await_pre_signal_zero_exit_for_test(
    pid_t pid, std::chrono::steady_clock::time_point deadline) {
  for (;;) {
    siginfo_t information{};
    const int observed = ::waitid(P_PID, static_cast<id_t>(pid), &information,
                                  WEXITED | WNOHANG | WNOWAIT);
    if (observed == 0 && information.si_pid == pid) {
      if (information.si_code != CLD_EXITED || information.si_status != 0) {
        throw ManagerFailure(
            JobAttemptFailure::WorkerExit,
            "pre-signal test child did not reach a normal zero exit");
      }
      return;
    }
    if (observed < 0 && errno != EINTR) {
      fail_stop_reaping_authority_lost();
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      throw ManagerFailure(
          JobAttemptFailure::WorkerExit,
          "pre-signal test child did not become a zero-exit zombie");
    }
    std::this_thread::sleep_until(std::min(
        deadline, checked_worker_deadline(now, kSupervisorPollInterval)));
  }
}

/**
 * @brief Waits until a child has exited without consuming its wait status.
 * @param pid Exact positive child PID still owned by the calling supervisor.
 * @param deadline Absolute monotonic test-only observation deadline.
 * @return Nothing once `waitid(WNOWAIT)` proves any terminal child status.
 * @throws ManagerFailure when the deterministic test deadline expires.
 * @throws std::overflow_error if the monotonic clock cannot represent the next
 * bounded poll slice.
 * @note This source-private seam is reached only when explicitly enabled by a
 * test option. The later production `waitpid` remains the sole exact reaper;
 * inability to observe this owned child fail-stops as authority loss.
 */
inline void await_any_exit_for_test(
    pid_t pid, std::chrono::steady_clock::time_point deadline) {
  for (;;) {
    siginfo_t information{};
    const int observed = ::waitid(P_PID, static_cast<id_t>(pid), &information,
                                  WEXITED | WNOHANG | WNOWAIT);
    if (observed == 0 && information.si_pid == pid) {
      return;
    }
    if (observed < 0 && errno != EINTR) {
      fail_stop_reaping_authority_lost();
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      throw ManagerFailure(
          JobAttemptFailure::WorkerExit,
          "cancel-channel test child did not reach a terminal status");
    }
    std::this_thread::sleep_until(std::min(
        deadline, checked_worker_deadline(now, kSupervisorPollInterval)));
  }
}

/**
 * @brief Sets close-on-exec and nonblocking flags on one parent-created fd.
 * @param fd Valid descriptor.
 * @param nonblocking Whether to add `O_NONBLOCK`.
 * @throws std::system_error on any `fcntl` failure.
 */
inline void configure_descriptor(int fd, bool nonblocking) {
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
[[noreturn]] inline void child_setup_failed(int fd, int error) noexcept {
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
 * @brief Closes every inherited child descriptor except exact fd 0 through 6.
 * @param plan Immutable platform input prepared before `fork`.
 * @return Zero after complete closure, otherwise the positive setup errno.
 * @throws Nothing.
 * @note Darwin scans the kernel-authoritative process descriptor space with
 * async-signal-safe `close` calls and accepts only `EBADF` for unused slots.
 * Linux requests `[7, UINT_MAX]` in one raw `close_range` syscall. An absent or
 * rejected Linux syscall fails closed instead of falling back to a finite cap.
 */
inline int close_child_descriptors(
    const ChildDescriptorClosurePlan& plan) noexcept {
#if defined(__APPLE__)
  for (int fd = kWorkerOutputDataDescriptor + 1;
       fd < plan.darwin_exclusive_maximum; ++fd) {
    if (::close(fd) == 0 || errno == EBADF) {
      continue;
    }
    return errno == 0 ? EIO : errno;
  }
  return 0;
#elif defined(__linux__)
  static_cast<void>(plan);
#if defined(SYS_close_range)
  const auto result =
      ::syscall(SYS_close_range,
                static_cast<unsigned int>(kWorkerOutputDataDescriptor + 1),
                std::numeric_limits<unsigned int>::max(), 0U);
  if (result == 0) {
    return 0;
  }
  return errno == 0 ? EIO : errno;
#else
  return ENOSYS;
#endif
#else
  static_cast<void>(plan);
  return ENOSYS;
#endif
}

/**
 * @brief Prepares platform state for exact post-fork descriptor closure.
 * @return Immutable closure input containing Darwin's kernel-wide descriptor
 * ceiling, or an empty Linux plan for the later raw syscall.
 * @throws std::system_error when Darwin's authoritative ceiling cannot be
 * queried or is invalid, or when the build platform is unsupported.
 * @note The Darwin query deliberately does not use soft `RLIMIT_NOFILE`:
 * descriptors opened before a soft-limit decrease remain valid above that
 * limit. `kern.maxfilesperproc` is the kernel ceiling, not an arbitrary cap.
 */
inline ChildDescriptorClosurePlan prepare_child_descriptor_closure() {
#if defined(__APPLE__)
  int maximum_descriptor = 0;
  std::size_t result_size = sizeof(maximum_descriptor);
  if (::sysctlbyname("kern.maxfilesperproc", &maximum_descriptor, &result_size,
                     nullptr, 0U) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "query Darwin worker descriptor ceiling");
  }
  if (result_size != sizeof(maximum_descriptor) ||
      maximum_descriptor <= kWorkerOutputDataDescriptor) {
    throw std::system_error(EIO, std::generic_category(),
                            "invalid Darwin worker descriptor ceiling");
  }
  return ChildDescriptorClosurePlan{maximum_descriptor};
#elif defined(__linux__)
  return ChildDescriptorClosurePlan{};
#else
  throw std::system_error(ENOSYS, std::generic_category(),
                          "worker descriptor closure is unsupported");
#endif
}

/**
 * @brief Builds the attempt-derived POSIX address-space soft/hard limit.
 * @param requested_bytes Accepted Job host-memory envelope.
 * @return Limit whose soft bound does not exceed the accepted envelope or the
 * process hard limit.
 * @throws std::system_error when `RLIMIT_AS` cannot be queried.
 */
inline rlimit worker_address_space_limit(std::uint64_t requested_bytes) {
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
 * @brief Builds the attempt-derived POSIX regular-file soft/hard limit.
 * @param requested_bytes Accepted output-stage byte maximum.
 * @return Limit whose soft bound exactly matches the accepted envelope.
 * @throws ManagerFailure with `WorkerStartup` when `RLIMIT_FSIZE` cannot be
 * queried, represented, or established beneath the inherited hard limit.
 * @note The check runs before `fork`, so the manager never publishes metadata
 * for a larger accepted stage while silently installing a smaller file-size
 * limit. This grants no quota or publication authority.
 */
inline rlimit worker_file_size_limit(std::uint64_t requested_bytes) {
  rlimit current{};
  if (::getrlimit(RLIMIT_FSIZE, &current) != 0) {
    throw ManagerFailure(JobAttemptFailure::WorkerStartup,
                         std::string("query worker file-size limit failed: ") +
                             std::strerror(errno));
  }
  if (requested_bytes >= static_cast<std::uint64_t>(RLIM_INFINITY)) {
    throw ManagerFailure(JobAttemptFailure::WorkerStartup,
                         "accepted worker output stage has no finite "
                         "RLIMIT_FSIZE representation");
  }
  const rlim_t requested = static_cast<rlim_t>(requested_bytes);
  if (current.rlim_max != RLIM_INFINITY && current.rlim_max < requested) {
    throw ManagerFailure(
        JobAttemptFailure::WorkerStartup,
        "accepted worker output stage exceeds inherited hard RLIMIT_FSIZE");
  }
  current.rlim_cur = requested;
  return current;
}

/**
 * @brief Returns the real clock or one manager's exec-status test replacement.
 * @param hooks Optional source-private callbacks retained by manager options.
 * @return Monotonic time used only by the exec-status bootstrap deadline.
 * @throws Nothing.
 * @note Product configuration leaves `hooks` null. A test callback receives no
 * descriptor, PID, process-lifecycle, Job, quota, artifact, or completion
 * authority.
 */
inline std::chrono::steady_clock::time_point worker_exec_status_now(
    const std::shared_ptr<const WorkerManagerExecStatusDeadlineTestHooks>&
        hooks) noexcept {
  if (hooks != nullptr && hooks->now != nullptr) {
    return hooks->now(hooks->context.get());
  }
  return std::chrono::steady_clock::now();
}

/**
 * @brief Notifies one deterministic exec-status acceptance observation.
 * @param hooks Optional source-private callbacks retained by manager options.
 * @param point Exact non-authorizing implementation boundary reached.
 * @return Nothing.
 * @throws Nothing.
 */
inline void observe_worker_exec_status_deadline_test_point(
    const std::shared_ptr<const WorkerManagerExecStatusDeadlineTestHooks>&
        hooks,
    WorkerManagerExecStatusDeadlineTestPoint point) noexcept {
  if (hooks != nullptr && hooks->observe != nullptr) {
    hooks->observe(hooks->context.get(), point);
  }
}

/**
 * @brief Rejects an exec-status result at or beyond its absolute deadline.
 * @param deadline Exact parent-side exec-bootstrap acceptance deadline.
 * @param test_hooks Optional deterministic source-private clock replacement.
 * @return The checked monotonic observation strictly before `deadline`.
 * @throws ManagerFailure with `WorkerStartup` when the deadline is reached.
 * @note The check runs both before a blocking wait and after a complete errno
 * or clean close-on-exec EOF has been read, so scheduler delay cannot revive a
 * late exec result or replace the deadline fact with a child errno.
 */
inline std::chrono::steady_clock::time_point
require_worker_exec_status_before_deadline(
    std::chrono::steady_clock::time_point deadline,
    const std::shared_ptr<const WorkerManagerExecStatusDeadlineTestHooks>&
        test_hooks) {
  const auto now = worker_exec_status_now(test_hooks);
  if (now >= deadline) {
    throw ManagerFailure(JobAttemptFailure::WorkerStartup,
                         "worker exec-status deadline expired");
  }
  return now;
}

/**
 * @brief Reads the close-on-exec setup pipe to distinguish exec from failure.
 * @param fd Nonblocking parent read descriptor.
 * @param deadline Absolute startup deadline.
 * @param test_hooks Optional deterministic source-private clock/observer.
 * @return Empty after successful exec EOF, otherwise child setup errno.
 * @throws ManagerFailure on timeout, truncation, or pipe-system failure.
 * @note A complete errno and clean close-on-exec EOF are accepted only after a
 * fresh strict deadline check. The test observer runs immediately before that
 * check; product configuration uses the real monotonic clock and no observer.
 */
inline std::optional<int> read_exec_status(
    int fd, std::chrono::steady_clock::time_point deadline,
    const std::shared_ptr<const WorkerManagerExecStatusDeadlineTestHooks>&
        test_hooks) {
  int child_error = 0;
  auto* bytes = reinterpret_cast<unsigned char*>(&child_error);
  std::size_t offset = 0U;
  for (;;) {
    const auto now =
        require_worker_exec_status_before_deadline(deadline, test_hooks);
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
        observe_worker_exec_status_deadline_test_point(
            test_hooks, WorkerManagerExecStatusDeadlineTestPoint::
                            ResultReadyBeforeAcceptance);
        require_worker_exec_status_before_deadline(deadline, test_hooks);
        return child_error;
      }
      continue;
    }
    if (received == 0) {
      if (offset == 0U) {
        observe_worker_exec_status_deadline_test_point(
            test_hooks, WorkerManagerExecStatusDeadlineTestPoint::
                            ResultReadyBeforeAcceptance);
        require_worker_exec_status_before_deadline(deadline, test_hooks);
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
inline std::string wait_status_message(int status) {
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

}  // namespace worker_manager_detail
using namespace worker_manager_detail;  // NOLINT(build/namespaces)

}  // namespace ps::server
