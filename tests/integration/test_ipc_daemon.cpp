#include <arpa/inet.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "ipc/codec.hpp"
#include "ipc/frame.hpp"
#include "ipc/server.hpp"
#include "ipc/server_lifecycle_test_access.hpp"
#include "ipc/unix_socket.hpp"
#include "photospider/ipc/client.hpp"
#include "support/ipc_host_spy.hpp"

#ifndef PS_PHOTOSPIDERD_PATH
#error "PS_PHOTOSPIDERD_PATH must name the real daemon executable"
#endif

#ifndef PS_IPC_OUTPUT_FIXTURE_DAEMON_PATH
#error "PS_IPC_OUTPUT_FIXTURE_DAEMON_PATH must name the output fixture"
#endif

#ifndef PS_TEST_OP_PLUGIN_DIR
#error \
    "PS_TEST_OP_PLUGIN_DIR must name the lifecycle operation plugin directory"
#endif

#ifndef PS_TEST_POLICY_PLUGIN_PATH
#error "PS_TEST_POLICY_PLUGIN_PATH must name the active-build policy DSO"
#endif

namespace ps::ipc {
namespace {

/** @brief Canonical type exported by the pure-C policy fixture. */
constexpr const char* kFixturePolicyType = "fixture_policy";

/**
 * @brief Owns one protected unique temporary directory for daemon tests.
 *
 * @throws std::filesystem::filesystem_error when setup fails.
 * @note Destruction removes transient graph/session/cache/socket content.
 */
class ScopedDaemonDirectory {
 public:
  /**
   * @brief Creates an empty mode-0700 directory.
   *
   * @param label Test-specific name prefix.
   * @param require_short_path Whether to use `/tmp` for `sun_path` tests.
   * @throws std::filesystem::filesystem_error when setup or chmod fails.
   */
  explicit ScopedDaemonDirectory(const std::string& label,
                                 bool require_short_path = false)
      : path_((require_short_path ? std::filesystem::path("/tmp")
                                  : std::filesystem::temp_directory_path()) /
              (label + "-" + std::to_string(::getpid()) + "-" +
               std::to_string(sequence_++))) {
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
    if (::chmod(path_.c_str(), 0700) != 0) {
      throw std::filesystem::filesystem_error(
          "chmod", path_, std::error_code(errno, std::generic_category()));
    }
  }

  /**
   * @brief Prevents two test owners from deleting one daemon work tree.
   *
   * @throws Nothing because this operation is unavailable.
   * @note Each fixture owns one unique protected directory.
   */
  ScopedDaemonDirectory(const ScopedDaemonDirectory&) = delete;

  /**
   * @brief Prevents replacing daemon-work cleanup ownership by copy.
   *
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   * @note The protected path remains fixed for the helper lifetime.
   */
  ScopedDaemonDirectory& operator=(const ScopedDaemonDirectory&) = delete;

  /**
   * @brief Removes the complete transient tree best-effort.
   *
   * @throws Nothing.
   */
  ~ScopedDaemonDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  /**
   * @brief Returns the owned directory path.
   *
   * @return Stable path reference valid until destruction.
   * @throws Nothing.
   */
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  /** @brief Owned temporary root. */
  std::filesystem::path path_;

  /** @brief Process-local uniqueness sequence. */
  static std::uint64_t sequence_;
};

/**
 * @brief Parent-side owner of one cross-process monotonic clock control file.
 *
 * @throws std::runtime_error if fixed-width file creation or update fails.
 * @note The file contains exactly one host-local `uint64_t` nanosecond value.
 *       Each update holds an exclusive advisory lock around one offset-zero
 *       `pwrite`; the fixture takes a shared lock around one matching `pread`.
 *       No environment variable, product flag, wire method, or real TTL sleep
 *       is used.
 */
class ManualProcessClock final {
 public:
  /**
   * @brief Creates a new mode-0600 eight-byte clock initialized to zero.
   * @param path Test-owned absolute control-file path.
   * @throws std::runtime_error if secure creation or initialization fails.
   */
  explicit ManualProcessClock(std::filesystem::path path)
      : path_(std::move(path)) {
    fd_ = ::open(path_.c_str(),
                 O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd_ < 0 || ::fchmod(fd_, 0600) != 0) {
      if (fd_ >= 0) {
        (void)::close(fd_);
        fd_ = -1;
      }
      throw std::runtime_error("cannot create fixture clock control file");
    }
    try {
      write_current();
    } catch (...) {
      (void)::close(fd_);
      fd_ = -1;
      (void)::unlink(path_.c_str());
      throw;
    }
  }

  /** @brief Closes the owned control descriptor. @throws Nothing. */
  ~ManualProcessClock() noexcept {
    if (fd_ >= 0) {
      (void)::close(fd_);
    }
  }

  /**
   * @brief Prevents duplicate control-file ownership.
   * @throws Nothing because this operation is unavailable.
   */
  ManualProcessClock(const ManualProcessClock&) = delete;

  /**
   * @brief Prevents replacing control-file ownership by assignment.
   * @return No value because assignment is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  ManualProcessClock& operator=(const ManualProcessClock&) = delete;

  /**
   * @brief Returns the stable absolute control-file path.
   * @return Borrowed path valid for this clock's lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& path() const noexcept { return path_; }

  /**
   * @brief Advances monotonic fixture time and atomically publishes it.
   * @param duration Nonnegative interval to add.
   * @return Nothing.
   * @throws std::invalid_argument for negative or overflowing durations.
   * @throws std::runtime_error if the locked fixed-width write fails.
   */
  void advance(std::chrono::nanoseconds duration) {
    if (duration.count() < 0 ||
        static_cast<std::uint64_t>(duration.count()) >
            std::numeric_limits<std::uint64_t>::max() - nanoseconds_) {
      throw std::invalid_argument("manual process clock advance overflowed");
    }
    nanoseconds_ += static_cast<std::uint64_t>(duration.count());
    write_current();
  }

 private:
  /**
   * @brief Publishes the current exact-width value under an exclusive lock.
   * @return Nothing.
   * @throws std::runtime_error if lock, write, or unlock coordination fails.
   */
  void write_current() {
    int lock_result = -1;
    do {
      lock_result = ::flock(fd_, LOCK_EX);
    } while (lock_result != 0 && errno == EINTR);
    if (lock_result != 0) {
      throw std::runtime_error("cannot lock fixture clock control file");
    }
    const ssize_t count = ::pwrite(fd_, &nanoseconds_, sizeof(nanoseconds_), 0);
    const int write_error = errno;
    int unlock_result = -1;
    do {
      unlock_result = ::flock(fd_, LOCK_UN);
    } while (unlock_result != 0 && errno == EINTR);
    if (count != static_cast<ssize_t>(sizeof(nanoseconds_)) ||
        unlock_result != 0) {
      errno = write_error;
      throw std::runtime_error("cannot update fixture clock control file");
    }
  }

  /** @brief Absolute path passed only to the test fixture CLI. */
  std::filesystem::path path_;

  /** @brief Owned control-file descriptor. */
  int fd_ = -1;

  /** @brief Current monotonic value in nanoseconds. */
  std::uint64_t nanoseconds_ = 0;
};

std::uint64_t ScopedDaemonDirectory::sequence_ = 0;

/**
 * @brief Returns the active-build lifecycle operation plugin directory.
 * @return CMake-provided target output directory.
 * @throws std::bad_alloc if path storage cannot allocate.
 * @note `TARGET_FILE_DIR` keeps the real-process test independent of build-tree
 *       names and multi-config output layout.
 */
std::filesystem::path lifecycle_operation_plugin_dir() {
  return std::filesystem::path(PS_TEST_OP_PLUGIN_DIR);
}

/**
 * @brief Returns the active-build pure-C policy fixture DSO path.
 * @return CMake-provided absolute target file path.
 * @throws std::bad_alloc if path storage cannot allocate.
 * @note `TARGET_FILE` keeps multi-config and non-default build trees free of
 *       hard-coded library names or output directories.
 */
std::filesystem::path policy_fixture_plugin_path() {
  return std::filesystem::path(PS_TEST_POLICY_PLUGIN_PATH);
}

/**
 * @brief Releases multiple forked daemon children from one shared start gate.
 *
 * @throws std::runtime_error if pipe creation, descriptor protection, or gate
 *         release fails.
 * @note Both descriptors are close-on-exec; each child consumes exactly one
 *       byte before executing `photospiderd`.
 */
class ConcurrentStartGate {
 public:
  /**
   * @brief Creates one close-on-exec synchronization pipe.
   *
   * @throws std::runtime_error if pipe creation or `fcntl` fails.
   */
  ConcurrentStartGate() {
    int descriptors[2] = {-1, -1};
    if (::pipe(descriptors) != 0) {
      throw std::runtime_error("pipe failed for daemon start gate");
    }
    read_end_.reset(descriptors[0]);
    write_end_.reset(descriptors[1]);
    for (const int descriptor : descriptors) {
      const int flags = ::fcntl(descriptor, F_GETFD, 0);
      if (flags < 0 || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
        throw std::runtime_error("fcntl failed for daemon start gate");
      }
    }
  }

  /**
   * @brief Prevents two gates from owning one pipe.
   *
   * @throws Nothing because this operation is unavailable.
   */
  ConcurrentStartGate(const ConcurrentStartGate&) = delete;

  /**
   * @brief Prevents replacing gate ownership by copy assignment.
   *
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  ConcurrentStartGate& operator=(const ConcurrentStartGate&) = delete;

  /**
   * @brief Closes both remaining gate descriptors.
   *
   * @throws Nothing.
   */
  ~ConcurrentStartGate() = default;

  /**
   * @brief Returns the descriptor children block on before exec.
   *
   * @return Borrowed read descriptor valid until gate destruction.
   * @throws Nothing.
   */
  int read_descriptor() const noexcept { return read_end_.get(); }

  /**
   * @brief Releases the requested number of forked children.
   *
   * @param participant_count Number of one-byte waiters to release.
   * @return Nothing.
   * @throws std::runtime_error if a complete token write fails.
   * @note The write end closes after all tokens are sent; the gate is
   *       single-use.
   */
  void release(std::size_t participant_count) {
    const char token = 'g';
    for (std::size_t index = 0; index < participant_count; ++index) {
      ssize_t written = -1;
      do {
        written = ::write(write_end_.get(), &token, 1);
      } while (written < 0 && errno == EINTR);
      if (written != 1) {
        throw std::runtime_error("write failed for daemon start gate");
      }
    }
    write_end_.reset();
  }

 private:
  /** @brief Parent/child read end used for the one-byte wait. */
  internal::UniqueFd read_end_;

  /** @brief Parent write end used to release all participants. */
  internal::UniqueFd write_end_;
};

/**
 * @brief RAII owner for one real daemon or output-fixture child process.
 *
 * The destructor performs finite SIGTERM and SIGKILL phases using only
 * nonblocking `waitpid` observations, so an assertion or daemon bug cannot
 * make CTest wait indefinitely. Explicit `stop()` calls retain a child pid
 * after a timeout or inconclusive wait error so the same owner can retry.
 *
 * @throws std::runtime_error from `start()` when prior ownership cannot be
 *         reclaimed or fork fails.
 * @note Readiness is established by typed `daemon.ping`, never fixed sleep.
 *       An empty fixture mode executes the product `photospiderd`; a known
 *       mode executes the non-installed deterministic output fixture.
 */
class DaemonProcess {
 public:
  /**
   * @brief Creates a not-yet-started process owner.
   *
   * @throws Nothing.
   */
  DaemonProcess() = default;

  /**
   * @brief Prevents two process owners from signaling/reaping one child.
   *
   * @throws Nothing because this operation is unavailable.
   * @note Child pid ownership is unique.
   */
  DaemonProcess(const DaemonProcess&) = delete;

  /**
   * @brief Prevents replacing child-process ownership by copy assignment.
   *
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   * @note Call `stop()` before reusing the same process owner with `start()`.
   */
  DaemonProcess& operator=(const DaemonProcess&) = delete;

  /**
   * @brief Makes one bounded best-effort attempt to terminate and reap a child.
   *
   * @throws Nothing.
   * @note No blocking wait is performed and no cleanup failure escapes the
   *       destructor. Under an exceptional OS wait failure or both finite
   *       deadline expirations, process ownership cannot be retried after the
   *       owner itself is destroyed.
   */
  ~DaemonProcess() noexcept { stop(); }

  /**
   * @brief Starts the real product daemon or deterministic output fixture.
   *
   * @param socket_path Expected socket path; passed through `--socket` when
   *        `explicit_socket` is true.
   * @param explicit_socket Whether to use the explicit option.
   * @param xdg_runtime Optional protected runtime root installed in the child
   *        environment for default-path tests.
   * @param start_gate_fd Optional descriptor from which the child must consume
   *        one byte before exec.
   * @param fixture_image_mode Empty for `photospiderd`, otherwise one of the
   *        deterministic fixture modes `empty`, `nonempty`, or `invalid`.
   * @param fixture_clock_control Absolute fixed-width manual-clock file passed
   *        only to the output fixture; ignored for the product daemon.
   * @throws std::bad_alloc if copied path/environment storage cannot be
   *         allocated.
   * @throws std::runtime_error if the previous child remains owned after the
   *         bounded stop preflight or if fork fails.
   * @note The child uses `execl` and never initializes Host before exec.
   */
  void start(std::string socket_path, bool explicit_socket = true,
             std::string xdg_runtime = {}, int start_gate_fd = -1,
             std::string fixture_image_mode = {},
             std::string fixture_clock_control = {}) {
    stop();
    if (pid_ >= 0) {
      throw std::runtime_error(
          "previous daemon child could not be reaped before restart");
    }
    exited_ = false;
    normal_exit_ = false;
    exit_code_ = -1;
    socket_path_ = std::move(socket_path);
    pid_ = ::fork();
    if (pid_ < 0) {
      throw std::runtime_error("fork failed for photospiderd integration test");
    }
    if (pid_ == 0) {
      if (start_gate_fd >= 0) {
        char token = 0;
        ssize_t received = -1;
        do {
          received = ::read(start_gate_fd, &token, 1);
        } while (received < 0 && errno == EINTR);
        (void)::close(start_gate_fd);
        if (received != 1) {
          ::_exit(125);
        }
      }
      if (!xdg_runtime.empty()) {
        if (::setenv("XDG_RUNTIME_DIR", xdg_runtime.c_str(), 1) != 0) {
          ::_exit(126);
        }
      }
      if (!fixture_image_mode.empty()) {
        ::execl(PS_IPC_OUTPUT_FIXTURE_DAEMON_PATH,
                PS_IPC_OUTPUT_FIXTURE_DAEMON_PATH, "--socket",
                socket_path_.c_str(), "--image-mode",
                fixture_image_mode.c_str(), "--clock-control",
                fixture_clock_control.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
      }
      if (explicit_socket) {
        ::execl(PS_PHOTOSPIDERD_PATH, PS_PHOTOSPIDERD_PATH, "--socket",
                socket_path_.c_str(), static_cast<char*>(nullptr));
      } else {
        ::execl(PS_PHOTOSPIDERD_PATH, PS_PHOTOSPIDERD_PATH,
                static_cast<char*>(nullptr));
      }
      ::_exit(127);
    }
  }

  /**
   * @brief Waits until a typed ping succeeds or the hard deadline expires.
   *
   * @param timeout Maximum readiness duration.
   * @return True after connect/ping success; false on timeout or early exit.
   * @throws std::bad_alloc if client diagnostics cannot be allocated.
   */
  bool wait_ready(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (poll_exit()) {
        return false;
      }
      Client client;
      if (client.connect(socket_path_).ok) {
        const IpcResult<DaemonPing> ping = client.ping();
        client.disconnect();
        if (ping.status.ok && ping.value.pong) {
          return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }

  /**
   * @brief Waits for child exit using finite nonblocking observations.
   *
   * @param timeout Maximum wait duration.
   * @return True if a terminal status was reaped or `ECHILD` proved that no
   *         waitable child remains; false while it is running, after the
   *         deadline, or after another wait error.
   * @throws Nothing.
   * @note A terminal status is recorded only when `waitpid` returns the owned
   *       pid. `ECHILD` clears stale ownership and records an unknown failure.
   *       Zero, timeout, `EINTR` at the deadline, and every other error retain
   *       the pid so `stop()` or its caller can retry. All observations use
   *       `WNOHANG`; even a zero timeout performs one observation.
   */
  bool wait_for_exit(std::chrono::milliseconds timeout) noexcept {
    if (pid_ < 0) {
      return true;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      int status = 0;
      const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
      if (waited == pid_) {
        record_exit(status);
        return true;
      }
      if (waited < 0) {
        const int wait_error = errno;
        if (wait_error == ECHILD) {
          record_unknown_exit();
          return true;
        }
        if (wait_error != EINTR) {
          return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          return false;
        }
        continue;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return false;
      }
      if (waited == 0) {
        std::this_thread::sleep_until(
            std::min(deadline, now + kExitPollInterval));
      } else {
        // A positive pid other than the uniquely owned child is impossible
        // for waitpid(pid_, ...), so preserve ownership and report failure.
        return false;
      }
    }
  }

  /**
   * @brief Requests graceful signal-driven shutdown without waiting for exit.
   *
   * @return True when no child remains or SIGTERM was delivered successfully.
   * @throws Nothing.
   * @note Tests use this split phase to observe that active Host work keeps
   *       shutdown incomplete until its deterministic input gate is released.
   */
  bool request_stop() noexcept {
    if (pid_ < 0) {
      return true;
    }
    if (::kill(pid_, SIGTERM) == 0) {
      return true;
    }
    return errno == ESRCH && wait_for_exit(std::chrono::milliseconds::zero());
  }

  /**
   * @brief Simulates abrupt process death and reaps it within one deadline.
   * @param timeout Maximum wait after successful SIGKILL delivery.
   * @return True after the child is conclusively absent, false on signal
   *         failure, wait failure, or timeout.
   * @throws Nothing.
   * @note This is used only to leave a genuine stale socket/output instance
   *       for restart cleanup. Normal lifecycle tests use graceful stop.
   */
  bool crash_and_wait(std::chrono::milliseconds timeout) noexcept {
    return signal_and_wait(SIGKILL, timeout);
  }

  /**
   * @brief Performs one nonblocking child-exit observation.
   *
   * @return True when the child is absent or has just been reaped.
   * @throws Nothing.
   * @note This reuses the exact wait classification in `wait_for_exit()`. A
   *       running child or inconclusive wait error leaves ownership unchanged;
   *       `ECHILD` records an unknown failure and clears stale ownership.
   */
  bool poll_exit() noexcept {
    return wait_for_exit(std::chrono::milliseconds::zero());
  }

  /**
   * @brief Attempts graceful shutdown, then one independently bounded kill.
   *
   * @return Nothing.
   * @throws Nothing.
   * @note Normal daemon shutdown receives a five-second WNOHANG polling
   *       deadline. If that phase cannot reap the child, SIGKILL receives a
   *       separate two-second WNOHANG polling deadline. A timeout or
   *       inconclusive non-`ECHILD` wait error after the final phase retains
   *       `pid_`, allowing a later explicit `stop()` or `start()` preflight to
   *       retry without overwriting ownership.
   */
  void stop() noexcept {
    if (signal_and_wait(SIGTERM, kGracefulStopTimeout)) {
      return;
    }
    (void)signal_and_wait(SIGKILL, kForcedReapTimeout);
  }

  /**
   * @brief Reports whether the reaped process exited normally with status zero.
   *
   * @return True only after a successful normal exit.
   * @throws Nothing.
   */
  bool exited_successfully() const noexcept {
    return exited_ && normal_exit_ && exit_code_ == 0;
  }

  /**
   * @brief Reports a nonzero, abnormal, or unknown-absence child outcome.
   *
   * @return True after any non-successful terminal state.
   * @throws Nothing.
   */
  bool exited_with_failure() const noexcept {
    return exited_ && (!normal_exit_ || exit_code_ != 0);
  }

  /**
   * @brief Reports a specific normal process exit status.
   *
   * @param expected_code Required normal exit code.
   * @return True only after a normal exit with exactly that code.
   * @throws Nothing.
   */
  bool exited_with_code(int expected_code) const noexcept {
    return exited_ && normal_exit_ && exit_code_ == expected_code;
  }

  /**
   * @brief Reports whether a terminal or unknown-absence outcome was recorded.
   *
   * @return True after a terminal wait status or `ECHILD` absence was recorded.
   * @throws Nothing.
   */
  bool has_exited() const noexcept { return exited_; }

 private:
  /** @brief Interval between nonblocking child-exit observations. */
  static constexpr std::chrono::milliseconds kExitPollInterval{10};

  /** @brief Grace period allowed after SIGTERM delivery. */
  static constexpr std::chrono::milliseconds kGracefulStopTimeout{5000};

  /** @brief Independent reap deadline after SIGKILL delivery. */
  static constexpr std::chrono::milliseconds kForcedReapTimeout{2000};

  /**
   * @brief Sends one signal and performs a finite nonblocking reap phase.
   * @param signal_number Signal to deliver to the currently owned child.
   * @param timeout Maximum WNOHANG polling duration after delivery or ESRCH.
   * @return True when no child was owned or the child became conclusively
   *         absent; false on signal failure, wait error, or timeout.
   * @throws Nothing.
   * @note `ESRCH` still enters `wait_for_exit()` so a zombie can be reaped or
   *       `ECHILD` can clear stale ownership. Every other signal failure and
   *       every inconclusive wait preserve `pid_` for a later retry.
   */
  bool signal_and_wait(int signal_number,
                       std::chrono::milliseconds timeout) noexcept {
    if (pid_ < 0) {
      return true;
    }
    if (::kill(pid_, signal_number) != 0 && errno != ESRCH) {
      return false;
    }
    return wait_for_exit(timeout);
  }

  /**
   * @brief Stores one waitpid terminal status and clears child ownership.
   *
   * @param status Raw waitpid status.
   * @throws Nothing.
   */
  void record_exit(int status) noexcept {
    exited_ = true;
    normal_exit_ = WIFEXITED(status);
    exit_code_ = normal_exit_ ? WEXITSTATUS(status) : -1;
    pid_ = -1;
  }

  /**
   * @brief Records that no waitable child remains without a terminal status.
   * @return Nothing.
   * @throws Nothing.
   * @note Used only for `ECHILD`; the absence is exposed as an unknown failure
   *       rather than fabricating a successful or signal-derived exit status.
   */
  void record_unknown_exit() noexcept {
    exited_ = true;
    normal_exit_ = false;
    exit_code_ = -1;
    pid_ = -1;
  }

  /** @brief Owned child pid, or -1 once reaped/absent/unknown via `ECHILD`. */
  pid_t pid_ = -1;

  /** @brief Socket path used for readiness probes. */
  std::string socket_path_;

  /** @brief Whether a terminal status or `ECHILD` absence was recorded. */
  bool exited_ = false;

  /** @brief Whether the terminal state was a normal process exit. */
  bool normal_exit_ = false;

  /** @brief Normal exit code, or -1 for signal/unknown-absence outcomes. */
  int exit_code_ = -1;
};

/**
 * @brief Writes a deterministic single-node graph with no compute dependency.
 *
 * @param path YAML source path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure on IO
 *         failure.
 * @note Operation lookup is deferred until compute, so graph/inspection IPC
 *       can load this fixture without plugin registration.
 */
void write_ipc_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output.exceptions(std::ios::badbit | std::ios::failbit);
  output << "- id: 1\n"
            "  name: ipc_source\n"
            "  type: ipc_fixture\n"
            "  subtype: source\n"
            "  parameters:\n"
            "    width: 6\n"
            "    height: 4\n";
}

/**
 * @brief Returns permission and special-mode bits for one filesystem path.
 *
 * @param path Existing path to inspect with `lstat`.
 * @return Permission plus set-id/sticky bits, or mode_t(-1) on failure.
 * @throws Nothing.
 */
mode_t permissions_of(const std::filesystem::path& path) noexcept {
  struct stat metadata{};
  return ::lstat(path.c_str(), &metadata) == 0
             ? static_cast<mode_t>(metadata.st_mode & 07777)
             : static_cast<mode_t>(-1);
}

/**
 * @brief Owns deterministic post-bind pathname replacement test state.
 *
 * @throws std::bad_alloc when symlink target storage is assigned.
 * @note The invoking test owns the state for the complete lifecycle callback
 *       interval and explicitly closes any retained replacement descriptor.
 *       Callbacks run only on the listener startup thread.
 */
struct ListenerReplacementState {
  /**
   * @brief Filesystem object installed at the bound pathname.
   *
   * @throws Nothing.
   * @note Values select only test-owned local filesystem operations and never
   *       enter a product option, environment variable, or wire value.
   */
  enum class Kind {
    /** @brief Observe the stage without replacing the pathname. */
    None,

    /** @brief Install a same-user regular file. */
    RegularFile,

    /** @brief Install a symbolic link to `symlink_target`. */
    Symlink,

    /** @brief Install and retain an exact-mode Unix socket. */
    Socket,
  };

  /** @brief Lifecycle boundary at which replacement occurs. */
  internal::ServerLifecycleTestStage stage =
      internal::ServerLifecycleTestStage::AfterBind;

  /** @brief Replacement kind to install. */
  Kind kind = Kind::RegularFile;

  /** @brief Preallocated symlink target used only by symlink cases. */
  std::string symlink_target;

  /** @brief Bound replacement socket retained to preserve its inode. */
  int replacement_socket = -1;

  /** @brief Whether a socket replacement enters listening state. */
  bool listen_on_replacement = false;

  /** @brief Device number captured immediately after socket replacement. */
  dev_t replacement_device = 0;

  /** @brief Inode number captured immediately after socket replacement. */
  ino_t replacement_inode = 0;

  /** @brief Mode observed immediately after the daemon bind. */
  mode_t initial_mode = static_cast<mode_t>(-1);

  /** @brief Zero on successful hook execution, otherwise captured `errno`. */
  int error = 0;

  /** @brief True when Active cleanup observed a still-open listener fd. */
  bool listener_open_during_cleanup = false;

  /** @brief Optional write end signalled after Active replacement. */
  int stop_writer = -1;
};

/** @brief State for deterministic parent rename/recreation. */
struct ParentReplacementState {
  /** @brief Original protected parent pathname. */
  std::string parent;

  /** @brief Renamed original directory pathname. */
  std::string moved_parent;

  /** @brief Zero on success, otherwise captured errno. */
  int error = 0;
};

/**
 * @brief Owns clients queued before listener pathname self-proof.
 *
 * @throws Nothing during default construction.
 * @note The fixed array matches the daemon's 32-client cap and avoids a test
 *       container allocation inside the lifecycle callback. Each element owns
 *       one descriptor until the test releases it.
 */
struct PendingClientState {
  /** @brief Fixed owners for all clients admitted before self-proof. */
  std::array<internal::UniqueFd, 32> clients;

  /** @brief Number of successfully connected and framed clients. */
  std::size_t count = 0;

  /** @brief Number of clients the stage hook should connect. */
  std::size_t target_count = 32;

  /** @brief Initial frame prefix written before listener self-proof. */
  std::size_t initial_frame_bytes = std::numeric_limits<std::size_t>::max();

  /** @brief Zero on callback success, otherwise the captured failure errno. */
  int error = 0;

  /** @brief True after the startup-thread callback has returned. */
  std::atomic<bool> hook_complete{false};

  /** @brief True after listener cleanup ownership becomes Active. */
  std::atomic<bool> activated{false};

  /** @brief Shared absolute deadline for every callback connect and write. */
  std::chrono::steady_clock::time_point deadline;

  /** @brief Cancellation latch set by the controlling test on timeout. */
  std::atomic<bool> cancel{false};
};

/**
 * @brief Builds the valid daemon ping frame used by proof-time clients.
 * @return Network-order length prefix plus complete bounded JSON payload.
 * @throws std::bad_alloc if frame storage cannot allocate.
 */
std::vector<unsigned char> pending_ping_frame() {
  static constexpr std::string_view request =
      R"({"protocol_version":2,"id":"pending","method":"daemon.ping","params":{}})";
  std::vector<unsigned char> frame(sizeof(std::uint32_t) + request.size());
  const std::uint32_t network_length =
      htonl(static_cast<std::uint32_t>(request.size()));
  std::memcpy(frame.data(), &network_length, sizeof(network_length));
  std::memcpy(frame.data() + sizeof(network_length), request.data(),
              request.size());
  return frame;
}

/**
 * @brief Records incomplete matching proof prefixes observed without consume.
 * @throws Nothing during default construction.
 */
struct ProofPrefixState {
  /** @brief Largest matching incomplete prefix reported by the proof loop. */
  std::atomic<std::size_t> largest_observed{0};
};

/**
 * @brief Publishes one matching incomplete proof-prefix observation.
 * @param opaque Borrowed `ProofPrefixState`.
 * @param observed_bytes Number of matching bytes visible through `MSG_PEEK`.
 * @throws Nothing.
 */
void observe_proof_prefix(void* opaque, std::size_t observed_bytes) noexcept {
  auto* state = static_cast<ProofPrefixState*>(opaque);
  std::size_t prior = state->largest_observed.load(std::memory_order_relaxed);
  while (prior < observed_bytes &&
         !state->largest_observed.compare_exchange_weak(
             prior, observed_bytes, std::memory_order_release,
             std::memory_order_relaxed)) {
  }
}

/**
 * @brief Replaces a bound socket pathname at one deterministic lifecycle stage.
 * @param opaque Borrowed `ListenerReplacementState`.
 * @param stage Current private listener lifecycle boundary.
 * @param path Bound socket pathname.
 * @throws Nothing; syscall failures are captured in state.
 * @note The hook performs no allocation and never uses GTest assertions.
 */
void replace_listener_path(void* opaque,
                           internal::ServerLifecycleTestStage stage,
                           const char* path) noexcept {
  auto* state = static_cast<ListenerReplacementState*>(opaque);
  if (stage == internal::ServerLifecycleTestStage::AfterBind) {
    struct stat metadata{};
    if (::lstat(path, &metadata) == 0) {
      state->initial_mode = metadata.st_mode & 07777;
    } else {
      state->error = errno;
      return;
    }
  }
  if (stage != state->stage ||
      state->kind == ListenerReplacementState::Kind::None) {
    return;
  }
  const auto signal_stop = [state]() noexcept {
    if (state->stop_writer >= 0) {
      const char token = 's';
      if (::write(state->stop_writer, &token, 1) != 1 && state->error == 0) {
        state->error = errno;
      }
    }
  };
  if (::unlink(path) != 0) {
    state->error = errno;
    signal_stop();
    return;
  }
  if (state->kind == ListenerReplacementState::Kind::RegularFile) {
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
    if (fd < 0) {
      state->error = errno;
      signal_stop();
      return;
    }
    if (::close(fd) != 0) {
      state->error = errno;
    }
    signal_stop();
    return;
  }
  if (state->kind == ListenerReplacementState::Kind::Symlink) {
    if (::symlink(state->symlink_target.c_str(), path) != 0) {
      state->error = errno;
    }
    signal_stop();
    return;
  }
  state->replacement_socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (state->replacement_socket < 0) {
    state->error = errno;
    signal_stop();
    return;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path, std::strlen(path) + 1);
  const socklen_t length = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + std::strlen(path) + 1);
  if (::bind(state->replacement_socket, reinterpret_cast<sockaddr*>(&address),
             length) != 0) {
    state->error = errno;
    signal_stop();
    return;
  }
  if (::chmod(path, 0600) != 0) {
    state->error = errno;
    signal_stop();
    return;
  }
  if (state->listen_on_replacement &&
      ::listen(state->replacement_socket, 1) != 0) {
    state->error = errno;
    signal_stop();
    return;
  }
  struct stat replacement{};
  if (::lstat(path, &replacement) != 0) {
    state->error = errno;
    signal_stop();
    return;
  }
  state->replacement_device = replacement.st_dev;
  state->replacement_inode = replacement.st_ino;
  signal_stop();
}

/**
 * @brief Deterministic failing replacement for the POSIX `listen` boundary.
 * @param fd Ignored bound descriptor.
 * @param backlog Ignored listener backlog.
 * @return Always -1 with `errno` set to `EIO`.
 * @throws Nothing.
 */
int fail_listener(int fd, int backlog) noexcept {
  (void)fd;
  (void)backlog;
  errno = EIO;
  return -1;
}

/**
 * @brief Closes an optional replacement socket descriptor.
 * @param state Replacement state whose raw descriptor is owned by the test.
 * @throws Nothing.
 */
void close_replacement_socket(ListenerReplacementState* state) noexcept {
  if (state->replacement_socket >= 0) {
    (void)::close(state->replacement_socket);
    state->replacement_socket = -1;
  }
}

/**
 * @brief Verifies that Active cleanup runs before listener close.
 * @param opaque Borrowed `ListenerReplacementState`.
 * @param listener_fd Descriptor expected to remain valid.
 * @param path Ignored configured path.
 * @throws Nothing.
 */
void observe_active_cleanup(void* opaque, int listener_fd,
                            const char* path) noexcept {
  (void)path;
  auto* state = static_cast<ListenerReplacementState*>(opaque);
  struct stat metadata{};
  state->listener_open_during_cleanup = listener_fd >= 0 &&
                                        ::fstat(listener_fd, &metadata) == 0 &&
                                        S_ISSOCK(metadata.st_mode);
}

/**
 * @brief Counts an Active-cleanup callback without dereferencing server state.
 * @param opaque Borrowed atomic call counter that outlives the server.
 * @param listener_fd Ignored listener descriptor.
 * @param path Ignored configured socket pathname.
 * @return Nothing.
 * @throws Nothing.
 * @note Used to prove an exceptional run drops its dependency borrow before the
 *       caller ends that dependency object's lifetime.
 */
void count_active_cleanup(void* opaque, int listener_fd,
                          const char* path) noexcept {
  (void)listener_fd;
  (void)path;
  auto* calls = static_cast<std::atomic<std::size_t>*>(opaque);
  calls->fetch_add(1, std::memory_order_relaxed);
}

/**
 * @brief Renames and recreates the configured parent after activation.
 * @param opaque Borrowed `ParentReplacementState`.
 * @param stage Current lifecycle stage.
 * @param path Ignored socket path below the prepared parent.
 * @throws Nothing; failures are captured in state.
 */
void replace_parent_path(void* opaque, internal::ServerLifecycleTestStage stage,
                         const char* path) noexcept {
  (void)path;
  if (stage !=
      internal::ServerLifecycleTestStage::AfterActivationBeforeRuntimeStart) {
    return;
  }
  auto* state = static_cast<ParentReplacementState*>(opaque);
  if (::rename(state->parent.c_str(), state->moved_parent.c_str()) != 0 ||
      ::mkdir(state->parent.c_str(), 0700) != 0 ||
      ::chmod(state->parent.c_str(), 0700) != 0) {
    state->error = errno;
  }
}

/**
 * @brief Queues a configured bounded set of clients before pathname proof.
 *
 * @param opaque Borrowed `PendingClientState` owned by the running test.
 * @param stage Current listener lifecycle boundary.
 * @param path Bound and listening Unix socket pathname.
 * @return Nothing.
 * @throws Nothing; connection and write failures are captured in state.
 * @note Each client sends the configured prefix of one valid request and reads
 *       no response. The callback leaves every descriptor open so the proof
 *       path must peek without consuming bytes and later transfer complete or
 *       partial requests to normal runtime workers.
 */
void queue_pending_clients(void* opaque,
                           internal::ServerLifecycleTestStage stage,
                           const char* path) noexcept {
  auto* state = static_cast<PendingClientState*>(opaque);
  if (stage ==
      internal::ServerLifecycleTestStage::AfterActivationBeforeRuntimeStart) {
    state->activated.store(true, std::memory_order_release);
    return;
  }
  if (stage != internal::ServerLifecycleTestStage::AfterListenBeforeProof) {
    return;
  }
  try {
    const std::vector<unsigned char> frame = pending_ping_frame();
    if (state->target_count > state->clients.size()) {
      state->error = EINVAL;
      state->hook_complete.store(true, std::memory_order_release);
      return;
    }
    const std::size_t initial_bytes =
        std::min(state->initial_frame_bytes, frame.size());
    for (std::size_t index = 0; index < state->target_count; ++index) {
      std::string message;
      state->clients[index] = internal::create_unix_stream_socket(&message);
      const auto cancelled = [state] {
        return state->cancel.load(std::memory_order_acquire) ||
               std::chrono::steady_clock::now() >= state->deadline;
      };
      if (!state->clients[index] ||
          !internal::connect_prepared_unix_socket(state->clients[index].get(),
                                                  path, cancelled, &message)) {
        state->error = errno == 0 ? EIO : errno;
        break;
      }
      const int flags = ::fcntl(state->clients[index].get(), F_GETFL, 0);
      if (flags < 0 || ::fcntl(state->clients[index].get(), F_SETFL,
                               flags | O_NONBLOCK) != 0) {
        state->error = errno;
        break;
      }
      std::size_t written = 0;
      while (written < initial_bytes && !cancelled()) {
        const ssize_t result =
            ::send(state->clients[index].get(), frame.data() + written,
                   initial_bytes - written, MSG_DONTWAIT);
        if (result > 0) {
          written += static_cast<std::size_t>(result);
          continue;
        }
        if (result < 0 && errno != EINTR && errno != EAGAIN &&
            errno != EWOULDBLOCK) {
          state->error = errno;
          break;
        }
        pollfd writable{state->clients[index].get(), POLLOUT, 0};
        (void)::poll(&writable, 1, 10);
      }
      if (written != initial_bytes) {
        state->error = state->error == 0 ? ETIMEDOUT : state->error;
        break;
      }
      ++state->count;
    }
  } catch (...) {
    state->error = ENOMEM;
  }
  state->hook_complete.store(true, std::memory_order_release);
}

/**
 * @brief Performs one raw `daemon.ping` before an absolute deadline.
 *
 * @param socket_path Running or starting daemon socket path.
 * @param deadline Absolute deadline shared by connect, send, and receive.
 * @param message Receives transport, frame, or response diagnostics.
 * @return True only after one correlated successful ping response.
 * @throws std::bad_alloc if request, response, or diagnostic storage allocates.
 * @note The implementation appears after the shared nonblocking frame helpers;
 *       this declaration lets capacity observation itself remain hard-bounded.
 */
bool ping_before_deadline(const std::string& socket_path,
                          std::chrono::steady_clock::time_point deadline,
                          std::string* message);

/**
 * @brief Waits until a newly accepted connection can ping successfully.
 *
 * @param socket_path Running daemon socket path.
 * @param timeout Hard capacity-recovery deadline.
 * @return True after a fresh connection/ping succeeds.
 * @throws std::bad_alloc if client diagnostics cannot be allocated.
 * @note The readiness loop replaces fixed sleeps after releasing bounded
 *       worker slots and disconnects every probe.
 */
bool wait_for_connection_capacity(const std::string& socket_path,
                                  std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto attempt_deadline =
        std::min(deadline, std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(250));
    std::string message;
    if (ping_before_deadline(socket_path, attempt_deadline, &message)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

/** @brief Short deterministic ordinary-stage budget used by deadline tests. */
constexpr auto kShortOrdinaryStageTimeout = std::chrono::milliseconds(100);

/**
 * @brief Owns one in-process real server with a test-only short stage budget.
 *
 * @throws std::runtime_error if stop-pipe creation or bounded listener
 *         readiness fails.
 * @throws std::system_error if server-thread construction fails.
 * @note The borrowed Host must outlive this object. Destruction closes the
 *       sole stop-pipe writer, joins the real server run, and leaves pathname
 *       cleanup to the production lifecycle path.
 */
class ScopedOrdinaryDeadlineServer final {
 public:
  /**
   * @brief Starts a real private server and proves one complete ping roundtrip.
   * @param host Borrowed Host implementation that outlives this object.
   * @param stage_timeout Positive timeout copied by the source-tree seam.
   * @throws std::runtime_error if setup or bounded readiness fails.
   * @throws std::system_error if server-thread construction fails.
   */
  ScopedOrdinaryDeadlineServer(Host& host,
                               std::chrono::milliseconds stage_timeout)
      : temp_("ps-ordinary-stage-deadline", true),
        socket_path_((temp_.path() / "listener.sock").string()),
        server_(host, "ordinary-deadline-test") {
    int descriptors[2] = {-1, -1};
    if (::pipe(descriptors) != 0) {
      throw std::runtime_error(std::string("deadline stop pipe failed: ") +
                               std::strerror(errno));
    }
    stop_reader_ = internal::UniqueFd(descriptors[0]);
    stop_writer_ = internal::UniqueFd(descriptors[1]);
    dependencies_.ordinary_connection_stage_timeout = stage_timeout;
    server_thread_ = std::thread([this] {
      run_status_ = internal::test_run_server(
          server_, internal::ServerOptions{socket_path_}, stop_reader_.get(),
          dependencies_);
    });
    if (!wait_for_connection_capacity(socket_path_, std::chrono::seconds(2))) {
      stop();
      throw std::runtime_error("ordinary deadline server did not become ready");
    }
  }

  /**
   * @brief Stops and joins the server when still active.
   * @throws Nothing.
   */
  ~ScopedOrdinaryDeadlineServer() { stop(); }

  /**
   * @brief Prevents copying live server, pipe, thread, and directory ownership.
   * @throws Nothing because this operation is unavailable.
   */
  ScopedOrdinaryDeadlineServer(const ScopedOrdinaryDeadlineServer&) = delete;

  /**
   * @brief Prevents replacing live server and thread ownership.
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  ScopedOrdinaryDeadlineServer& operator=(const ScopedOrdinaryDeadlineServer&) =
      delete;

  /**
   * @brief Returns the active Unix socket pathname.
   * @return Borrowed path valid for this object's lifetime.
   * @throws Nothing.
   */
  const std::string& socket_path() const noexcept { return socket_path_; }

  /**
   * @brief Ends the server run and returns its terminal status.
   * @return Borrowed terminal status: success for the expected stop-pipe
   *         lifecycle, otherwise the real server diagnostic.
   * @throws Nothing.
   */
  const OperationStatus& finish() noexcept {
    stop();
    return run_status_;
  }

 private:
  /**
   * @brief Closes the sole stop writer and joins the running server thread.
   * @return Nothing.
   * @throws Nothing.
   * @note Stop-pipe hangup is an established normal stop signal and avoids a
   *       potentially failing or interrupted destructor write.
   */
  void stop() noexcept {
    if (!server_thread_.joinable()) {
      return;
    }
    stop_writer_.reset();
    server_thread_.join();
    stop_reader_.reset();
  }

  /** @brief Protected directory removed after server destruction. */
  ScopedDaemonDirectory temp_;

  /** @brief Stable socket path below `temp_`. */
  std::string socket_path_;

  /** @brief Real private server under test. */
  internal::Server server_;

  /** @brief Run-scoped source-tree timeout injection. */
  internal::ServerLifecycleTestDependencies dependencies_;

  /** @brief Stop-pipe read descriptor observed by the server. */
  internal::UniqueFd stop_reader_;

  /** @brief Sole stop-pipe writer closed by `stop()`. */
  internal::UniqueFd stop_writer_;

  /** @brief Terminal result published before the server thread exits. */
  OperationStatus run_status_;

  /** @brief Joinable owner of the real blocking server run. */
  std::thread server_thread_;
};

/**
 * @brief Converts one absolute monotonic deadline to a bounded poll timeout.
 *
 * @param deadline Absolute operation deadline.
 * @return Zero after expiry, otherwise at least one millisecond and at most
 *         `INT_MAX` milliseconds.
 * @throws Nothing.
 */
int remaining_poll_timeout(
    std::chrono::steady_clock::time_point deadline) noexcept {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return 0;
  }
  const auto remaining = deadline - now;
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
  if (milliseconds <= 0) {
    return 1;
  }
  return static_cast<int>(
      std::min<std::int64_t>(milliseconds, std::numeric_limits<int>::max()));
}

/**
 * @brief Waits for one descriptor readiness event before an absolute deadline.
 *
 * @param fd Nonblocking descriptor to observe.
 * @param events Requested poll readiness mask.
 * @param deadline Absolute deadline shared by the complete RPC.
 * @param message Receives a timeout or poll diagnostic.
 * @return True when requested, hangup, or error state is observable and the
 *         caller should attempt its next syscall.
 * @throws std::bad_alloc if diagnostic construction cannot allocate.
 */
bool wait_for_descriptor(int fd, std::int16_t events,
                         std::chrono::steady_clock::time_point deadline,
                         std::string* message) {
  while (true) {
    const int timeout = remaining_poll_timeout(deadline);
    if (timeout == 0) {
      *message = "real daemon RPC exceeded its hard deadline";
      return false;
    }
    pollfd descriptor{fd, events, 0};
    const int ready = ::poll(&descriptor, 1, timeout);
    if (ready > 0) {
      if ((descriptor.revents & POLLNVAL) != 0) {
        *message = "real daemon RPC descriptor became invalid";
        return false;
      }
      if ((descriptor.revents & (events | POLLERR | POLLHUP)) != 0) {
        return true;
      }
      continue;
    }
    if (ready == 0) {
      *message = "real daemon RPC exceeded its hard deadline";
      return false;
    }
    if (errno != EINTR) {
      *message =
          std::string("real daemon RPC poll failed: ") + std::strerror(errno);
      return false;
    }
  }
}

/**
 * @brief Waits for server-side connection closure without draining input.
 *
 * @param fd Connected nonblocking client descriptor.
 * @param deadline Absolute monotonic observation deadline.
 * @param message Receives timeout, invalid-descriptor, or poll diagnostics.
 * @return True only after peer hangup or socket error becomes observable.
 * @throws std::bad_alloc if diagnostic construction allocates.
 * @note `MSG_PEEK` distinguishes EOF from readable response bytes without
 *       consuming them, preserving write-side backpressure for deadline tests.
 *       A one-millisecond retry interval prevents queued bytes from causing a
 *       readiness spin while the server write deadline is still running.
 */
bool wait_for_peer_hangup(int fd,
                          std::chrono::steady_clock::time_point deadline,
                          std::string* message) {
  while (true) {
    const int timeout = remaining_poll_timeout(deadline);
    if (timeout == 0) {
      *message = "ordinary connection did not close before test deadline";
      return false;
    }
    pollfd descriptor{fd, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, timeout);
    if (ready > 0) {
      if ((descriptor.revents & POLLNVAL) != 0) {
        *message = "ordinary connection descriptor became invalid";
        return false;
      }
      if ((descriptor.revents & (POLLHUP | POLLERR)) != 0) {
        return true;
      }
      if ((descriptor.revents & POLLIN) != 0) {
        unsigned char byte = 0;
        const ssize_t peeked =
            ::recv(fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
        if (peeked == 0) {
          return true;
        }
        if (peeked < 0 && errno != EINTR && errno != EAGAIN &&
            errno != EWOULDBLOCK) {
          *message = std::string("ordinary connection peek failed: ") +
                     std::strerror(errno);
          return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      continue;
    }
    if (ready == 0) {
      *message = "ordinary connection did not close before test deadline";
      return false;
    }
    if (errno != EINTR) {
      *message = std::string("ordinary connection poll failed: ") +
                 std::strerror(errno);
      return false;
    }
  }
}

/**
 * @brief Drains bytes already queued after the server connection is closed.
 *
 * @param fd Connected nonblocking client descriptor after server shutdown.
 * @param maximum_bytes Test-owned safety bound for accumulated response bytes.
 * @param message Receives overflow or non-transient receive diagnostics.
 * @return Bytes observed before EOF, temporary exhaustion, or failure.
 * @throws std::bad_alloc if byte or diagnostic storage allocation fails.
 * @note This helper runs only after closure observation or explicit server
 * stop, so it cannot relieve backpressure while the write deadline is active.
 */
std::vector<unsigned char> drain_closed_peer_bytes(int fd,
                                                   std::size_t maximum_bytes,
                                                   std::string* message) {
  std::vector<unsigned char> bytes;
  std::array<unsigned char, 64U * 1024U> buffer{};
  while (true) {
    const ssize_t received =
        ::recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
    if (received > 0) {
      const std::size_t count = static_cast<std::size_t>(received);
      if (bytes.size() > maximum_bytes ||
          count > maximum_bytes - bytes.size()) {
        *message = "closed-peer response exceeded the test safety bound";
        return bytes;
      }
      bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
      continue;
    }
    if (received == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
      return bytes;
    }
    if (errno == EINTR) {
      continue;
    }
    *message = std::string("closed-peer response receive failed: ") +
               std::strerror(errno);
    return bytes;
  }
}

/**
 * @brief Opens one nonblocking Unix connection before an absolute deadline.
 *
 * @param socket_path Running daemon socket path.
 * @param deadline Absolute deadline shared by connect, send, and receive.
 * @param message Receives validation or socket diagnostics.
 * @return Owned connected nonblocking descriptor, or empty on failure.
 * @throws std::bad_alloc if diagnostics cannot allocate.
 * @note This helper performs one connect attempt and never retries an RPC.
 */
internal::UniqueFd connect_before_deadline(
    const std::string& socket_path,
    std::chrono::steady_clock::time_point deadline, std::string* message) {
  if (socket_path.empty() || socket_path.front() != '/' ||
      socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
    *message = "real daemon RPC socket path is invalid";
    return {};
  }
  internal::UniqueFd connection(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (!connection) {
    *message =
        std::string("real daemon RPC socket failed: ") + std::strerror(errno);
    return {};
  }
  const int descriptor_flags = ::fcntl(connection.get(), F_GETFD, 0);
  const int status_flags = ::fcntl(connection.get(), F_GETFL, 0);
  if (descriptor_flags < 0 || status_flags < 0 ||
      ::fcntl(connection.get(), F_SETFD, descriptor_flags | FD_CLOEXEC) != 0 ||
      ::fcntl(connection.get(), F_SETFL, status_flags | O_NONBLOCK) != 0 ||
      !internal::configure_no_sigpipe(connection.get(), message)) {
    if (message->empty()) {
      *message =
          std::string("real daemon RPC fcntl failed: ") + std::strerror(errno);
    }
    return {};
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
  const socklen_t address_length = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + socket_path.size() + 1);
  int connected = -1;
  do {
    connected =
        ::connect(connection.get(), reinterpret_cast<sockaddr*>(&address),
                  address_length);
  } while (connected < 0 && errno == EINTR &&
           std::chrono::steady_clock::now() < deadline);
  if (connected == 0) {
    return connection;
  }
  if (errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK) {
    *message =
        std::string("real daemon RPC connect failed: ") + std::strerror(errno);
    return {};
  }
  if (!wait_for_descriptor(connection.get(), POLLOUT, deadline, message)) {
    return {};
  }
  int socket_error = 0;
  socklen_t error_size = sizeof(socket_error);
  if (::getsockopt(connection.get(), SOL_SOCKET, SO_ERROR, &socket_error,
                   &error_size) != 0 ||
      socket_error != 0) {
    const int failure = socket_error != 0 ? socket_error : errno;
    *message = std::string("real daemon RPC connect failed: ") +
               std::strerror(failure);
    return {};
  }
  return connection;
}

/**
 * @brief Sends one exact byte range before the shared RPC deadline.
 *
 * @param fd Connected nonblocking socket.
 * @param data Bytes to send.
 * @param size Exact byte count.
 * @param deadline Absolute RPC deadline.
 * @param message Receives timeout or IO diagnostics.
 * @return True only after every byte is sent.
 * @throws std::bad_alloc if diagnostics cannot allocate.
 */
bool send_before_deadline(int fd, const void* data, std::size_t size,
                          std::chrono::steady_clock::time_point deadline,
                          std::string* message) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::size_t offset = 0;
  while (offset < size) {
    if (!wait_for_descriptor(fd, POLLOUT, deadline, message)) {
      return false;
    }
#ifdef MSG_NOSIGNAL
    constexpr int kSendFlags = MSG_NOSIGNAL;
#else
    constexpr int kSendFlags = 0;
#endif
    const ssize_t written =
        ::send(fd, bytes + offset, size - offset, kSendFlags);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 &&
        (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    *message = std::string("real daemon RPC send failed: ") +
               (written == 0 ? "zero-byte progress" : std::strerror(errno));
    return false;
  }
  return true;
}

/**
 * @brief Receives one exact byte range before the shared RPC deadline.
 *
 * @param fd Connected nonblocking socket.
 * @param data Destination byte range.
 * @param size Exact byte count.
 * @param deadline Absolute RPC deadline.
 * @param message Receives timeout, EOF, or IO diagnostics.
 * @return True only after every byte is received.
 * @throws std::bad_alloc if diagnostics cannot allocate.
 */
bool receive_before_deadline(int fd, void* data, std::size_t size,
                             std::chrono::steady_clock::time_point deadline,
                             std::string* message) {
  auto* bytes = static_cast<unsigned char*>(data);
  std::size_t offset = 0;
  while (offset < size) {
    if (!wait_for_descriptor(fd, POLLIN, deadline, message)) {
      return false;
    }
    const ssize_t received = ::recv(fd, bytes + offset, size - offset, 0);
    if (received > 0) {
      offset += static_cast<std::size_t>(received);
      continue;
    }
    if (received < 0 &&
        (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    *message = received == 0
                   ? "real daemon RPC peer closed before a complete frame"
                   : std::string("real daemon RPC receive failed: ") +
                         std::strerror(errno);
    return false;
  }
  return true;
}

/**
 * @brief Reads one complete frame from a nonblocking test socket by deadline.
 *
 * @param fd Connected nonblocking Unix stream descriptor.
 * @param deadline Absolute deadline shared by header and payload reads.
 * @return Complete payload, bounded-length failure, or exact timeout/IO
 *         diagnostic.
 * @throws std::bad_alloc if payload or diagnostic construction allocates.
 * @note `poll(POLLIN)` proves only that at least one byte is readable. This
 *       helper uses `receive_before_deadline()` for both frame ranges so stream
 *       fragmentation cannot turn a valid partial response into `EAGAIN`.
 */
internal::FrameReadResult read_frame_before_deadline(
    int fd, std::chrono::steady_clock::time_point deadline) {
  std::uint32_t network_size = 0;
  std::string message;
  if (!receive_before_deadline(fd, &network_size, sizeof(network_size),
                               deadline, &message)) {
    return {internal::FrameReadState::IoError, {}, std::move(message)};
  }
  const std::uint32_t payload_size = ntohl(network_size);
  if (payload_size == 0 || payload_size > kMaximumFramePayloadBytes) {
    return {internal::FrameReadState::InvalidLength,
            {},
            "frame payload length is outside 1..16777216 bytes"};
  }
  std::string payload(payload_size, '\0');
  if (!receive_before_deadline(fd, payload.data(), payload.size(), deadline,
                               &message)) {
    return {internal::FrameReadState::IoError, {}, std::move(message)};
  }
  return {internal::FrameReadState::Complete, std::move(payload), {}};
}

/** @copydoc ping_before_deadline */
bool ping_before_deadline(const std::string& socket_path,
                          std::chrono::steady_clock::time_point deadline,
                          std::string* message) {
  internal::UniqueFd connection =
      connect_before_deadline(socket_path, deadline, message);
  if (!connection) {
    return false;
  }
  const std::string request = internal::Json{
      {"protocol_version", kProtocolVersion},
      {"id", "deadline-capacity-probe"},
      {"method", "daemon.ping"},
      {"params",
       internal::Json::object()}}.dump();
  const std::uint32_t network_size =
      htonl(static_cast<std::uint32_t>(request.size()));
  if (!send_before_deadline(connection.get(), &network_size,
                            sizeof(network_size), deadline, message) ||
      !send_before_deadline(connection.get(), request.data(), request.size(),
                            deadline, message)) {
    return false;
  }
  const internal::FrameReadResult response =
      read_frame_before_deadline(connection.get(), deadline);
  if (response.state != internal::FrameReadState::Complete ||
      response.payload.find("\"id\":\"deadline-capacity-probe\"") ==
          std::string::npos ||
      response.payload.find("\"pong\":true") == std::string::npos) {
    *message = response.message.empty()
                   ? "daemon ping returned an invalid response"
                   : response.message;
    return false;
  }
  return true;
}

/**
 * @brief Sends one complete typed-envelope request and returns before reading.
 *
 * @param socket_path Running daemon Unix socket.
 * @param method Exact version 1 method.
 * @param params Complete typed params object.
 * @param id Correlated request identity.
 * @param deadline Hard deadline shared across connect and frame send.
 * @param message Receives transport or framing diagnostics.
 * @return Connected descriptor after the complete request frame is sent, or
 *         empty when setup or transmission fails.
 * @throws std::bad_alloc if request or diagnostic storage cannot allocate.
 * @note The caller decides whether to read the response or abandon the
 *       connection. The helper performs no retry, so closing the returned
 *       descriptor models an ambiguous/lost response without replaying the
 *       mutating request.
 */
internal::UniqueFd send_raw_daemon_request(
    const std::string& socket_path, const std::string& method,
    internal::Json params, std::string id,
    std::chrono::steady_clock::time_point deadline, std::string* message) {
  internal::UniqueFd connection =
      connect_before_deadline(socket_path, deadline, message);
  if (!connection) {
    return {};
  }
  const std::string request = internal::Json{
      {"protocol_version", kProtocolVersion},
      {"id", std::move(id)},
      {"method", method},
      {"params",
       std::move(params)}}.dump();
  if (request.empty() || request.size() > kMaximumFramePayloadBytes) {
    *message = "real daemon RPC request exceeds frame bounds";
    return {};
  }
  const std::uint32_t network_size =
      htonl(static_cast<std::uint32_t>(request.size()));
  if (!send_before_deadline(connection.get(), &network_size,
                            sizeof(network_size), deadline, message) ||
      !send_before_deadline(connection.get(), request.data(), request.size(),
                            deadline, message)) {
    return {};
  }
  return connection;
}

/**
 * @brief Performs one short-lived typed-envelope call without public Client
 *        compute helpers.
 *
 * @param socket_path Running daemon Unix socket.
 * @param method Exact version 1 method.
 * @param params Complete typed params object.
 * @param id Correlated request identity.
 * @param timeout Hard deadline shared across connect, frame send, and frame
 *        receive.
 * @param report_failure Whether transport/framing failures are test failures.
 * @return Parsed complete response object, or an empty object after a test
 *         assertion records enabled transport/framing/parsing failure.
 * @throws std::bad_alloc if request or response storage cannot allocate.
 * @note The currently installed direct Client has no compute helpers. Each
 *       invocation therefore creates and closes one real connection, which
 *       also verifies that compute ownership is daemon-scoped.
 */
internal::Json raw_daemon_call(
    const std::string& socket_path, const std::string& method,
    internal::Json params, std::string id = "raw-daemon-call",
    std::chrono::milliseconds timeout = std::chrono::seconds(3),
    bool report_failure = true) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string message;
  internal::UniqueFd connection =
      send_raw_daemon_request(socket_path, method, std::move(params),
                              std::move(id), deadline, &message);
  if (!connection) {
    if (report_failure) {
      ADD_FAILURE() << message;
    }
    return internal::Json::object();
  }

  std::uint32_t response_network_size = 0;
  if (!receive_before_deadline(connection.get(), &response_network_size,
                               sizeof(response_network_size), deadline,
                               &message)) {
    if (report_failure) {
      ADD_FAILURE() << message;
    }
    return internal::Json::object();
  }
  const std::uint32_t response_size = ntohl(response_network_size);
  if (response_size == 0 || response_size > 16U * 1024U * 1024U) {
    if (report_failure) {
      ADD_FAILURE() << "real daemon returned an invalid frame length";
    }
    return internal::Json::object();
  }
  std::string response(response_size, '\0');
  if (!receive_before_deadline(connection.get(), response.data(),
                               response.size(), deadline, &message)) {
    if (report_failure) {
      ADD_FAILURE() << message;
    }
    return internal::Json::object();
  }
  const internal::JsonParseResult parsed = internal::parse_json(response);
  if (!parsed.ok || !parsed.value.is_object()) {
    if (report_failure) {
      ADD_FAILURE() << "real daemon returned malformed JSON: "
                    << parsed.message;
    }
    return internal::Json::object();
  }
  return parsed.value;
}

/**
 * @brief Reusable deterministic release gate for concurrent raw RPC starts.
 *
 * @throws Nothing.
 * @note The coordinator observes readiness before releasing all participants,
 *       so the test covers overlapping server requests rather than two merely
 *       sequential client calls.
 */
class ConcurrentCallGate final {
 public:
  /**
   * @brief Records one ready caller and waits boundedly for release.
   * @param timeout Hard upper bound for coordinator release.
   * @return True after release; false when the gate deadline expires.
   * @throws Nothing.
   * @note Timeout prevents a failed second task construction from stranding a
   *       successfully started first participant indefinitely.
   */
  bool arrive_and_wait(std::chrono::milliseconds timeout) noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    ++ready_;
    changed_.notify_all();
    return changed_.wait_for(lock, timeout, [this] { return released_; });
  }

  /**
   * @brief Waits until at least the requested number of callers are ready.
   * @param count Minimum participant count.
   * @param timeout Bounded observation duration.
   * @return True when the participant count was reached.
   * @throws Nothing.
   */
  bool wait_ready(std::size_t count,
                  std::chrono::milliseconds timeout) noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout,
                             [this, count] { return ready_ >= count; });
  }

  /**
   * @brief Releases every current and future participant.
   * @return Nothing.
   * @throws Nothing.
   */
  void release() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    changed_.notify_all();
  }

 private:
  /** @brief Serializes readiness and release state. */
  std::mutex mutex_;

  /** @brief Signals readiness changes and the single release transition. */
  std::condition_variable changed_;

  /** @brief Number of callers waiting at the gate. */
  std::size_t ready_ = 0;

  /** @brief Whether participants may enter their raw RPC call. */
  bool released_ = false;
};

/**
 * @brief Runs one hard-deadline raw daemon call on an owned joinable thread.
 *
 * @throws std::bad_alloc or std::system_error if state/thread creation fails.
 * @note Tests explicitly stop/release daemon work before joining failure paths;
 *       the worker itself also has a hard transport deadline, so destruction
 *       cannot inherit the unbounded `std::future` join behavior this helper
 *       replaces. The task retains its optional start gate and releases it
 *       before every join/destruction path; a failed peer construction can
 *       therefore unwind the first task without stranding its worker.
 */
class RawDaemonCallTask final {
 public:
  /**
   * @brief Starts one exact RPC operation.
   *
   * @param socket_path Running daemon socket.
   * @param method Exact protocol method.
   * @param params Complete typed params.
   * @param id Correlated request id.
   * @param timeout Hard end-to-end RPC deadline.
   * @param report_failure Whether expected transport closure records failure.
   * @param start_gate Optional shared gate entered immediately before RPC.
   * @throws std::bad_alloc or std::system_error on setup failure.
   */
  RawDaemonCallTask(std::string socket_path, std::string method,
                    internal::Json params, std::string id,
                    std::chrono::milliseconds timeout,
                    bool report_failure = true,
                    std::shared_ptr<ConcurrentCallGate> start_gate = {})
      : state_(std::make_shared<State>()),
        start_gate_(std::move(start_gate)),
        worker_([state = state_, socket_path = std::move(socket_path),
                 method = std::move(method), params = std::move(params),
                 id = std::move(id), timeout, report_failure,
                 start_gate = start_gate_]() mutable {
          try {
            if (start_gate && !start_gate->arrive_and_wait(timeout)) {
              throw std::runtime_error(
                  "concurrent raw RPC start gate timed out");
            }
            internal::Json response =
                raw_daemon_call(socket_path, method, std::move(params),
                                std::move(id), timeout, report_failure);
            std::lock_guard<std::mutex> lock(state->mutex);
            state->response = std::move(response);
          } catch (...) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->exception = std::current_exception();
          }
          {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->complete = true;
          }
          state->changed.notify_all();
        }) {}

  /**
   * @brief Prevents two helpers from owning one RPC thread.
   * @param other Existing owner.
   * @throws Nothing because construction is unavailable.
   */
  RawDaemonCallTask(const RawDaemonCallTask& other) = delete;

  /**
   * @brief Prevents replacing one RPC thread owner.
   * @param other Existing owner.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  RawDaemonCallTask& operator=(const RawDaemonCallTask& other) = delete;

  /**
   * @brief Joins the bounded worker when explicit test cleanup was skipped.
   * @throws Nothing.
   */
  ~RawDaemonCallTask() noexcept {
    release_gate();
    try {
      if (worker_.joinable()) {
        worker_.join();
      }
    } catch (...) {
    }
  }

  /**
   * @brief Observes completion without waiting.
   * @return True after the worker published its response.
   * @throws Nothing.
   */
  bool complete() const noexcept {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->complete;
  }

  /**
   * @brief Waits for response publication up to a bounded observation window.
   * @param timeout Maximum observation duration.
   * @return True when the call completed.
   * @throws Nothing.
   */
  bool wait_for(std::chrono::milliseconds timeout) const noexcept {
    std::unique_lock<std::mutex> lock(state_->mutex);
    return state_->changed.wait_for(lock, timeout,
                                    [this] { return state_->complete; });
  }

  /**
   * @brief Joins the worker and transfers its response.
   * @return Complete parsed response, or an empty object after contained IO
   *         failure.
   * @throws std::system_error if thread joining fails.
   * @throws Whatever exception the worker captured while starting/running the
   *         operation, after the thread has been reclaimed.
   * @note Call once after `wait_for()` or after deterministic daemon cleanup.
   */
  internal::Json join() {
    release_gate();
    if (worker_.joinable()) {
      worker_.join();
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->exception) {
      std::rethrow_exception(state_->exception);
    }
    return std::move(state_->response);
  }

 private:
  /**
   * @brief Idempotently releases a held start gate before any blocking join.
   * @return Nothing.
   * @throws Nothing.
   */
  void release_gate() noexcept {
    if (start_gate_) {
      start_gate_->release();
    }
  }

  /**
   * @brief Shared completion state published by the worker.
   *
   * @throws std::bad_alloc when the owned JSON response or exception state
   *         allocates.
   * @note The heap owner outlives the joinable worker. Every field is accessed
   *       under `mutex`; the worker publishes response/exception before setting
   *       `complete` and notifying waiters.
   */
  struct State {
    /** @brief Mutex protecting response publication. */
    mutable std::mutex mutex;

    /** @brief Condition variable signaled exactly once on completion. */
    mutable std::condition_variable changed;

    /** @brief Whether the response has been published. */
    bool complete = false;

    /** @brief Parsed response or contained-failure empty object. */
    internal::Json response = internal::Json::object();

    /** @brief Worker failure rethrown only after explicit join/reclamation. */
    std::exception_ptr exception;
  };

  /** @brief Heap-owned state safe across the worker lifetime. */
  std::shared_ptr<State> state_;

  /** @brief Optional gate retained so destruction/join can always release it.
   */
  std::shared_ptr<ConcurrentCallGate> start_gate_;

  /** @brief Sole joined RPC worker. */
  std::thread worker_;
};

/**
 * @brief Owns one FIFO used to block a real Host call deterministically.
 *
 * @throws std::runtime_error if FIFO creation or permission setup fails.
 * @note A successful nonblocking writer-only open proves the daemon already
 *       has a reader. The helper then keeps an `O_RDWR` descriptor open so an
 *       assertion cleanup can close the gate without risking SIGPIPE.
 */
class BlockingHostFifo final {
 public:
  /**
   * @brief Creates one exact mode-0600 FIFO.
   * @param path Unique FIFO path below a protected test directory.
   * @throws std::runtime_error if creation or chmod fails.
   */
  explicit BlockingHostFifo(std::filesystem::path path)
      : path_(std::move(path)) {
    if (::mkfifo(path_.c_str(), 0600) != 0 ||
        ::chmod(path_.c_str(), 0600) != 0) {
      throw std::runtime_error(std::string("failed to create Host FIFO: ") +
                               std::strerror(errno));
    }
  }

  /**
   * @brief Prevents duplicate ownership of one FIFO gate.
   * @param other Existing owner.
   * @throws Nothing because construction is unavailable.
   */
  BlockingHostFifo(const BlockingHostFifo& other) = delete;

  /**
   * @brief Prevents replacement of one FIFO gate.
   * @param other Existing owner.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  BlockingHostFifo& operator=(const BlockingHostFifo& other) = delete;

  /**
   * @brief Closes an unreleased writer so a blocked reader observes EOF.
   * @throws Nothing.
   */
  ~BlockingHostFifo() = default;

  /**
   * @brief Waits until the daemon has opened the FIFO for reading.
   * @param timeout Hard reader-observation deadline.
   * @return True after a nonblocking writer-only open proves reader presence.
   * @throws std::bad_alloc if an error diagnostic cannot allocate.
   * @note `ENXIO` is the expected not-yet-reading state; no fixed sleep is a
   *       correctness condition.
   */
  bool wait_for_reader(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      internal::UniqueFd proof(
          ::open(path_.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC));
      if (proof) {
        internal::UniqueFd stable(
            ::open(path_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
        if (!stable) {
          message_ = std::string("failed to stabilize Host FIFO: ") +
                     std::strerror(errno);
          return false;
        }
        writer_ = std::move(stable);
        return true;
      }
      if (errno != ENXIO && errno != EINTR) {
        message_ = std::string("failed to observe Host FIFO reader: ") +
                   std::strerror(errno);
        return false;
      }
      std::this_thread::yield();
    }
    message_ = "Host FIFO reader was not observed before deadline";
    return false;
  }

  /**
   * @brief Writes one valid graph and closes the FIFO to release reload.
   * @return True after the complete YAML payload and EOF are published.
   * @throws std::bad_alloc if diagnostics cannot allocate.
   * @note The payload is smaller than `PIPE_BUF`; the `O_RDWR` ownership keeps
   *       the write safe even if the daemon has already closed its reader.
   */
  bool release() {
    static constexpr std::string_view kGraph =
        "- id: 1\n"
        "  name: ipc_source\n"
        "  type: ipc_fixture\n"
        "  subtype: source\n"
        "  parameters:\n"
        "    width: 6\n"
        "    height: 4\n";
    return write_and_close(kGraph, "reload");
  }

  /**
   * @brief Returns the last FIFO setup or release diagnostic.
   * @return Stable borrowed message until the next helper operation.
   * @throws Nothing.
   */
  const std::string& message() const noexcept { return message_; }

 private:
  /**
   * @brief Writes one bounded payload and closes the stable FIFO writer.
   * @param payload Complete bytes consumed by the blocked Host-side reader.
   * @param purpose Diagnostic operation label.
   * @return True after every byte and EOF are published.
   * @throws std::bad_alloc if a failure diagnostic cannot allocate.
   * @note The retained `O_RDWR` descriptor prevents SIGPIPE if the reader
   *       consumes only a prefix and closes before this helper finishes.
   */
  bool write_and_close(std::string_view payload, const char* purpose) {
    if (!writer_) {
      message_ = std::string(purpose) + " FIFO has no confirmed reader";
      return false;
    }
    std::size_t offset = 0;
    while (offset < payload.size()) {
      const ssize_t written = ::write(writer_.get(), payload.data() + offset,
                                      payload.size() - offset);
      if (written > 0) {
        offset += static_cast<std::size_t>(written);
        continue;
      }
      if (written < 0 && errno == EINTR) {
        continue;
      }
      message_ = std::string("failed to release ") + purpose + " FIFO: " +
                 (written == 0 ? "zero-byte progress" : std::strerror(errno));
      writer_.reset();
      return false;
    }
    writer_.reset();
    return true;
  }

  /** @brief Owned FIFO path removed with the surrounding test directory. */
  std::filesystem::path path_;

  /** @brief Stable read/write gate descriptor held until release. */
  internal::UniqueFd writer_;

  /** @brief Last contained setup/release failure. */
  std::string message_;
};

/**
 * @brief Bounds one RPC or polling wait by an aggregate monotonic deadline.
 *
 * @param deadline Absolute aggregate deadline measured by `steady_clock`.
 * @param cap Maximum duration for the individual operation.
 * @return Positive whole-millisecond timeout no greater than either bound, or
 *         zero when no whole millisecond remains or the cap is nonpositive.
 * @throws Nothing.
 * @note Callers must not start an RPC or sleep when this helper returns zero.
 */
std::chrono::milliseconds timeout_before_deadline(
    std::chrono::steady_clock::time_point deadline,
    std::chrono::milliseconds cap) noexcept {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline || cap <= std::chrono::milliseconds::zero()) {
    return std::chrono::milliseconds::zero();
  }
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  return std::min(cap, remaining);
}

/**
 * @brief Polls one real-daemon job to a terminal state with a hard watchdog.
 * @param socket_path Running daemon socket.
 * @param compute_id Opaque accepted job identity.
 * @param timeout Maximum observation duration.
 * @return Last complete response, terminal on success.
 * @throws std::bad_alloc if raw request/response storage cannot allocate.
 * @note Every poll uses a fresh connection and a unique id. The 10 ms wait
 *       reduces process-test spin; correctness is determined by state, not a
 *       fixed completion sleep.
 */
internal::Json wait_for_real_compute_terminal(
    const std::string& socket_path, const std::string& compute_id,
    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::size_t sequence = 0;
  internal::Json response;
  while (true) {
    const auto rpc_timeout = timeout_before_deadline(
        deadline, std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::seconds(3)));
    if (rpc_timeout <= std::chrono::milliseconds::zero()) {
      break;
    }
    response = raw_daemon_call(socket_path, "compute.status",
                               internal::Json{{"compute_id", compute_id}},
                               "terminal-poll-" + std::to_string(sequence++),
                               rpc_timeout);
    if (!response.contains("result")) {
      return response;
    }
    const std::string state = response["result"]["state"].get<std::string>();
    if (state == "succeeded" || state == "failed") {
      return response;
    }
    const auto sleep_timeout =
        timeout_before_deadline(deadline, std::chrono::milliseconds(10));
    if (sleep_timeout <= std::chrono::milliseconds::zero()) {
      break;
    }
    std::this_thread::sleep_for(sleep_timeout);
  }
  ADD_FAILURE() << "real daemon compute did not terminate before deadline";
  return response;
}

/**
 * @brief Builds the smallest valid status-mode compute submission.
 *
 * @param session Active opaque daemon session id.
 * @param node Target public node id.
 * @return Params containing only required fields and status result mode.
 * @throws std::bad_alloc if JSON storage cannot allocate.
 * @note Missing option objects, intent, and dirty ROI exercise public Host
 *       defaults rather than spelling those defaults into process tests.
 */
internal::Json minimal_compute_submit_params(const IpcSessionId& session,
                                             std::int64_t node = 1) {
  return internal::Json{{"session_id", session.value},
                        {"node_id", node},
                        {"result_mode", "status"}};
}

/**
 * @brief Builds the smallest valid image-mode compute submission.
 * @param session Active opaque daemon session id.
 * @param node Target public node id recorded by the deterministic Host.
 * @return Params containing only required fields and image result mode.
 * @throws std::bad_alloc if JSON storage cannot allocate.
 */
internal::Json image_compute_submit_params(const IpcSessionId& session,
                                           std::int64_t node = 1) {
  return internal::Json{{"session_id", session.value},
                        {"node_id", node},
                        {"result_mode", "image"}};
}

/**
 * @brief Temporarily changes one real directory's permission bits.
 *
 * @throws std::runtime_error when the path is not a real directory or its
 *         mode cannot be observed or changed.
 * @note The original mode is restored best-effort during destruction. Tests
 *       call `restore()` explicitly before depending on the restored access.
 */
class ScopedDirectoryMode final {
 public:
  /**
   * @brief Installs one temporary directory mode without changing identity.
   * @param path Existing test-owned directory that must not be a symlink.
   * @param temporary_mode Permission bits active until `restore()`.
   * @throws std::runtime_error when validation or chmod fails.
   */
  ScopedDirectoryMode(std::filesystem::path path, mode_t temporary_mode)
      : path_(std::move(path)) {
    struct stat metadata{};
    if (::lstat(path_.c_str(), &metadata) != 0 || !S_ISDIR(metadata.st_mode)) {
      throw std::runtime_error("output fixture instance is not a directory");
    }
    original_mode_ = metadata.st_mode & 07777;
    if (::chmod(path_.c_str(), temporary_mode) != 0) {
      throw std::runtime_error(
          "cannot install temporary output directory mode");
    }
    active_ = true;
  }

  /**
   * @brief Restores the original mode best-effort when still active.
   * @throws Nothing; restoration failures are contained.
   */
  ~ScopedDirectoryMode() noexcept {
    if (active_) {
      (void)::chmod(path_.c_str(), original_mode_);
    }
  }

  /**
   * @brief Prevents duplicate restoration ownership.
   * @throws Nothing because construction is unavailable.
   */
  ScopedDirectoryMode(const ScopedDirectoryMode&) = delete;

  /**
   * @brief Prevents replacing restoration ownership by assignment.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedDirectoryMode& operator=(const ScopedDirectoryMode&) = delete;

  /**
   * @brief Restores the original mode and disarms destructor cleanup.
   * @return Nothing.
   * @throws std::runtime_error when chmod fails.
   */
  void restore() {
    if (!active_) {
      return;
    }
    if (::chmod(path_.c_str(), original_mode_) != 0) {
      throw std::runtime_error("cannot restore output directory mode");
    }
    active_ = false;
  }

 private:
  /** @brief Stable test-owned directory path. */
  std::filesystem::path path_;

  /** @brief Permission bits captured before the temporary change. */
  mode_t original_mode_ = 0;

  /** @brief Whether destruction must still attempt restoration. */
  bool active_ = false;
};

/**
 * @brief Finds the sole live output-store instance below one fixture socket.
 * @param socket_path Running fixture socket whose store has completed start.
 * @return Absolute path of the one valid `instance-<opaque-id>` directory.
 * @throws std::runtime_error unless exactly one real instance directory exists.
 * @throws std::filesystem::filesystem_error when enumeration fails.
 */
std::filesystem::path find_output_instance_directory(
    const std::string& socket_path) {
  const std::filesystem::path base(socket_path + ".outputs");
  std::optional<std::filesystem::path> instance;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(base)) {
    const std::string name = entry.path().filename().string();
    constexpr std::string_view prefix = "instance-";
    struct stat metadata{};
    if (name.rfind(prefix, 0) != 0 ||
        !internal::valid_opaque_id(name.substr(prefix.size())) ||
        ::lstat(entry.path().c_str(), &metadata) != 0 ||
        !S_ISDIR(metadata.st_mode) || instance.has_value()) {
      throw std::runtime_error(
          "output fixture does not own exactly one safe instance");
    }
    instance = entry.path();
  }
  if (!instance.has_value()) {
    throw std::runtime_error("output fixture has no live output instance");
  }
  return *instance;
}

/**
 * @brief Reads one complete small artifact through a no-follow descriptor.
 * @param path Protected absolute artifact path returned by compute.result.
 * @return Exact bytes read to EOF.
 * @throws std::runtime_error if open or read fails.
 * @throws std::bad_alloc if byte storage cannot grow.
 */
std::vector<std::uint8_t> read_artifact_bytes(const std::string& path) {
  internal::UniqueFd descriptor(
      ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  if (!descriptor) {
    throw std::runtime_error("cannot open fixture output artifact");
  }
  std::vector<std::uint8_t> bytes;
  std::array<std::uint8_t, 256> buffer{};
  while (true) {
    const ssize_t count =
        ::read(descriptor.get(), buffer.data(), buffer.size());
    if (count > 0) {
      bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
      continue;
    }
    if (count == 0) {
      return bytes;
    }
    if (errno != EINTR) {
      throw std::runtime_error("cannot read fixture output artifact");
    }
  }
}

/**
 * @brief Loads one opaque session through the public typed Client.
 * @param socket_path Running product or fixture daemon socket.
 * @param root_dir Absolute test-owned graph root.
 * @param session_name Safe display name copied into the Host request.
 * @return Loaded opaque session summary or exact connect/load failure.
 * @throws std::bad_alloc if request or result storage cannot allocate.
 * @note The deterministic Host does not inspect a YAML file; this helper still
 *       traverses the real Client/frame/router/session-registry stack.
 */
IpcResult<GraphSessionSummary> load_fixture_session(
    const std::string& socket_path, const std::filesystem::path& root_dir,
    std::string session_name) {
  Client client;
  OperationStatus connected = client.connect(socket_path);
  if (!connected.ok) {
    return {std::move(connected), {}};
  }
  GraphLoadRequest request;
  request.session = GraphSessionId{std::move(session_name)};
  request.root_dir = root_dir.string();
  IpcResult<GraphSessionSummary> loaded = client.load_graph(request);
  client.disconnect();
  return loaded;
}

/**
 * @brief Failure-safe best-effort release for one fixture compute job.
 * @throws std::bad_alloc when copied identities cannot allocate.
 * @note Destruction performs one bounded raw release without recording a new
 *       assertion. Tests dismiss the guard after their explicit release.
 */
class ScopedFixtureJobRelease final {
 public:
  /**
   * @brief Arms cleanup for one accepted job.
   * @param socket_path Running fixture socket.
   * @param compute_id Accepted opaque compute identity.
   * @throws std::bad_alloc if identity storage cannot allocate.
   */
  ScopedFixtureJobRelease(std::string socket_path, std::string compute_id)
      : socket_path_(std::move(socket_path)),
        compute_id_(std::move(compute_id)) {}

  /**
   * @brief Performs bounded best-effort cleanup when still armed.
   * @throws Nothing; request construction and transport failures are
   *         contained.
   */
  ~ScopedFixtureJobRelease() noexcept {
    if (!armed_) {
      return;
    }
    try {
      internal::Json params{{"compute_id", compute_id_}};
      if (delivery_id_.has_value()) {
        params["delivery_id"] = *delivery_id_;
      }
      (void)raw_daemon_call(socket_path_, "compute.release", std::move(params),
                            "fixture-guard-release",
                            std::chrono::milliseconds(500), false);
    } catch (...) {
    }
  }

  /**
   * @brief Prevents duplicate job-cleanup ownership.
   * @throws Nothing because construction is unavailable.
   */
  ScopedFixtureJobRelease(const ScopedFixtureJobRelease&) = delete;

  /**
   * @brief Prevents replacing job-cleanup ownership by assignment.
   * @return No value because copying is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedFixtureJobRelease& operator=(const ScopedFixtureJobRelease&) = delete;

  /**
   * @brief Adds the stable lease identity to failure cleanup.
   * @param delivery_id Valid delivery id returned by compute.result.
   * @return Nothing.
   * @throws std::bad_alloc if copied identity storage cannot allocate.
   */
  void protect_delivery(std::string delivery_id) {
    delivery_id_ = std::move(delivery_id);
  }

  /**
   * @brief Disarms cleanup after explicit successful release.
   * @return Nothing.
   * @throws Nothing.
   */
  void dismiss() noexcept { armed_ = false; }

 private:
  /** @brief Running fixture socket path. */
  std::string socket_path_;

  /** @brief Accepted job identity. */
  std::string compute_id_;

  /** @brief Optional matching delivery identity. */
  std::optional<std::string> delivery_id_;

  /** @brief Whether destruction must attempt cleanup. */
  bool armed_ = true;
};

/**
 * @brief Terminal nonempty image job plus its first protected delivery.
 * @throws std::bad_alloc when owned JSON or identity storage cannot allocate.
 */
struct FixtureImageDelivery {
  /** @brief Accepted opaque compute identity. */
  std::string compute_id;

  /** @brief Exact non-null output object returned by compute.result. */
  internal::Json output;
};

/**
 * @brief Submits, awaits, and obtains one nonempty fixture image delivery.
 * @param socket_path Running nonempty output fixture socket.
 * @param session Active opaque fixture session.
 * @param label Unique request-id prefix.
 * @return Terminal job id and first protected metadata delivery.
 * @throws std::runtime_error if any correlated stage is not successful.
 * @throws std::bad_alloc if request/response storage cannot allocate.
 */
FixtureImageDelivery deliver_fixture_image(const std::string& socket_path,
                                           const IpcSessionId& session,
                                           const std::string& label) {
  const internal::Json submitted =
      raw_daemon_call(socket_path, "compute.submit",
                      image_compute_submit_params(session), label + "-submit");
  if (!submitted.contains("result") ||
      !submitted["result"].value("compute_id", internal::Json()).is_string()) {
    throw std::runtime_error("fixture image submission failed: " +
                             submitted.dump());
  }
  FixtureImageDelivery delivery;
  delivery.compute_id = submitted["result"]["compute_id"].get<std::string>();
  const internal::Json terminal = wait_for_real_compute_terminal(
      socket_path, delivery.compute_id, std::chrono::seconds(3));
  if (!terminal.contains("result") ||
      terminal["result"].value("state", "") != "succeeded") {
    throw std::runtime_error("fixture image did not succeed: " +
                             terminal.dump());
  }
  const internal::Json result = raw_daemon_call(
      socket_path, "compute.result",
      internal::Json{{"compute_id", delivery.compute_id}}, label + "-result");
  if (!result.contains("result") ||
      !result["result"].value("output", internal::Json()).is_object()) {
    throw std::runtime_error("fixture image delivery failed: " + result.dump());
  }
  delivery.output = result["result"]["output"];
  return delivery;
}

/**
 * @brief Opens one fixture artifact without following a replacement symlink.
 * @param path Protected output path from a successful delivery.
 * @return Owned read-only descriptor.
 * @throws std::runtime_error if open fails.
 */
internal::UniqueFd open_fixture_artifact(const std::string& path) {
  internal::UniqueFd descriptor(
      ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  if (!descriptor) {
    throw std::runtime_error("cannot retain fixture artifact descriptor");
  }
  return descriptor;
}

/**
 * @brief Reads one already-open fixture artifact by descriptor identity.
 * @param fd Descriptor retained before pathname cleanup.
 * @return Exact complete bytes, including after the pathname is unlinked.
 * @throws std::runtime_error if metadata or exact-offset reading fails.
 * @throws std::bad_alloc if byte storage cannot allocate.
 */
std::vector<std::uint8_t> read_open_fixture_artifact(int fd) {
  struct stat metadata{};
  if (::fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < 0 || metadata.st_size > 256) {
    throw std::runtime_error("open fixture artifact metadata is invalid");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(metadata.st_size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t count =
        ::pread(fd, bytes.data() + offset, bytes.size() - offset,
                static_cast<off_t>(offset));
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else {
      throw std::runtime_error("open fixture artifact read failed");
    }
  }
  return bytes;
}

/**
 * @brief Triggers lazy compute/output expiry without addressing a live job.
 * @param socket_path Running fixture socket.
 * @param label Unique request id.
 * @return Nothing.
 * @throws std::bad_alloc if request/response storage cannot allocate.
 * @note The well-formed absent job/delivery pair returns job_not_found after
 *       both registries observe the shared manual clock. It is not a test wire
 *       method or product failpoint.
 */
void trigger_fixture_cleanup(const std::string& socket_path,
                             const std::string& label) {
  const internal::Json response = raw_daemon_call(
      socket_path, "compute.release",
      internal::Json{{"compute_id", "ffffffffffffffffffffffffffffffff"},
                     {"delivery_id", "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"}},
      label);
  EXPECT_TRUE(response.contains("error")) << response.dump();
  if (response.contains("error")) {
    EXPECT_EQ(response["error"]["name"], "job_not_found");
  }
}

/**
 * @brief Polls one real-daemon job until an exact nonterminal state appears.
 *
 * @param socket_path Running daemon socket.
 * @param compute_id Accepted opaque job id.
 * @param expected Exact state to observe.
 * @param timeout Hard aggregate observation deadline.
 * @return Last complete response, with the expected state on success.
 * @throws std::bad_alloc if request/response storage cannot allocate.
 */
internal::Json wait_for_real_compute_state(const std::string& socket_path,
                                           const std::string& compute_id,
                                           std::string_view expected,
                                           std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::size_t sequence = 0;
  internal::Json response;
  while (true) {
    const auto rpc_timeout =
        timeout_before_deadline(deadline, std::chrono::milliseconds(500));
    if (rpc_timeout <= std::chrono::milliseconds::zero()) {
      break;
    }
    response = raw_daemon_call(socket_path, "compute.status",
                               internal::Json{{"compute_id", compute_id}},
                               "state-poll-" + std::to_string(sequence++),
                               rpc_timeout);
    if (!response.contains("result") ||
        response["result"]["state"] == expected) {
      return response;
    }
    std::this_thread::yield();
  }
  ADD_FAILURE() << "real daemon compute did not reach state " << expected
                << " before deadline";
  return response;
}

/**
 * @brief Waits until signal shutdown has closed real-daemon admission.
 *
 * @param socket_path Daemon socket being shut down.
 * @param timeout Hard aggregate observation deadline.
 * @return True when a one-attempt ping can no longer complete.
 * @throws std::bad_alloc if probe storage cannot allocate.
 * @note Expected connect/EOF failures are contained and do not record test
 *       failures; successful pings are fresh state observations, not sleeps.
 */
bool wait_for_listener_shutdown(const std::string& socket_path,
                                std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::size_t sequence = 0;
  while (true) {
    const auto rpc_timeout =
        timeout_before_deadline(deadline, std::chrono::milliseconds(100));
    if (rpc_timeout <= std::chrono::milliseconds::zero()) {
      break;
    }
    const internal::Json response = raw_daemon_call(
        socket_path, "daemon.ping", internal::Json::object(),
        "shutdown-probe-" + std::to_string(sequence++), rpc_timeout, false);
    if (!response.contains("result")) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

TEST(IpcDaemonLifecycle, ExplicitDefaultLiveStaleAndUnsafeSocketPolicy) {
  ScopedDaemonDirectory temp("photospider-ipc-daemon-lifecycle");
  const std::string socket_path = (temp.path() / "explicit.sock").string();
  DaemonProcess daemon;
  daemon.start(socket_path);
  ASSERT_TRUE(daemon.wait_ready(std::chrono::seconds(5)));
  EXPECT_EQ(permissions_of(temp.path()), 0700);
  EXPECT_EQ(permissions_of(socket_path), 0600);

  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const auto ping = client.ping();
  const auto version = client.version();
  ASSERT_TRUE(ping.status.ok);
  EXPECT_TRUE(ping.value.pong);
  ASSERT_TRUE(version.status.ok);
  EXPECT_EQ(version.value.protocol_version, 2);
  EXPECT_EQ(version.value.service_name, "photospiderd");
  EXPECT_FALSE(version.value.service_version.empty());
  EXPECT_EQ(version.value.server_instance_id, ping.value.server_instance_id);
  EXPECT_EQ(version.value.server_instance_id.size(), 32U);
  EXPECT_TRUE(std::all_of(version.value.server_instance_id.begin(),
                          version.value.server_instance_id.end(),
                          [](char character) {
                            return (character >= '0' && character <= '9') ||
                                   (character >= 'a' && character <= 'f');
                          }));
  EXPECT_EQ(version.value.transport, "unix");
  std::vector<std::string> expected_methods;
  expected_methods.reserve(internal::kVersionTwoMethodNames.size());
  for (std::string_view method : internal::kVersionTwoMethodNames) {
    expected_methods.emplace_back(method);
  }
  EXPECT_EQ(version.value.methods, expected_methods);

  DaemonProcess contender;
  contender.start(socket_path);
  EXPECT_TRUE(contender.wait_for_exit(std::chrono::seconds(5)));
  EXPECT_TRUE(contender.exited_with_failure());
  EXPECT_TRUE(client.ping().status.ok);
  client.disconnect();
  daemon.stop();
  EXPECT_TRUE(daemon.exited_successfully());
  EXPECT_FALSE(std::filesystem::exists(socket_path));

  internal::UniqueFd stale_socket(::socket(AF_UNIX, SOCK_STREAM, 0));
  ASSERT_TRUE(stale_socket);
  sockaddr_un stale_address{};
  stale_address.sun_family = AF_UNIX;
  std::memcpy(stale_address.sun_path, socket_path.c_str(),
              socket_path.size() + 1);
  const socklen_t stale_length = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + socket_path.size() + 1);
  ASSERT_EQ(::bind(stale_socket.get(),
                   reinterpret_cast<sockaddr*>(&stale_address), stale_length),
            0);
  stale_socket.reset();
  DaemonProcess reclaimed;
  reclaimed.start(socket_path);
  ASSERT_TRUE(reclaimed.wait_ready(std::chrono::seconds(5)));
  reclaimed.stop();
  EXPECT_TRUE(reclaimed.exited_successfully());
  EXPECT_FALSE(std::filesystem::exists(socket_path));

  const std::filesystem::path regular = temp.path() / "regular";
  {
    std::ofstream file(regular);
    file << "preserve";
  }
  DaemonProcess unsafe;
  unsafe.start(regular.string());
  EXPECT_TRUE(unsafe.wait_for_exit(std::chrono::seconds(5)));
  EXPECT_TRUE(unsafe.exited_with_failure());
  EXPECT_TRUE(std::filesystem::is_regular_file(regular));

  const std::filesystem::path symlink = temp.path() / "symlink";
  std::filesystem::create_symlink(regular, symlink);
  DaemonProcess symlink_daemon;
  symlink_daemon.start(symlink.string());
  EXPECT_TRUE(symlink_daemon.wait_for_exit(std::chrono::seconds(5)));
  EXPECT_TRUE(symlink_daemon.exited_with_failure());
  EXPECT_TRUE(std::filesystem::is_symlink(symlink));

  ScopedDaemonDirectory unsafe_lock_temp("pslk", true);
  const std::string unsafe_lock_socket =
      (unsafe_lock_temp.path() / "unsafe-lock.sock").string();
  const std::filesystem::path unsafe_lock_path = unsafe_lock_socket + ".lock";
  {
    std::ofstream file(unsafe_lock_path);
    file << "preserve";
  }
  ASSERT_EQ(::chmod(unsafe_lock_path.c_str(), 0644), 0);
  DaemonProcess unsafe_lock_daemon;
  unsafe_lock_daemon.start(unsafe_lock_socket);
  EXPECT_TRUE(unsafe_lock_daemon.wait_for_exit(std::chrono::seconds(5)));
  EXPECT_TRUE(unsafe_lock_daemon.exited_with_failure());
  EXPECT_EQ(permissions_of(unsafe_lock_path), 0644);
  std::filesystem::remove(unsafe_lock_path);

  {
    std::ofstream file(unsafe_lock_path);
    file << "preserve";
  }
  constexpr mode_t kSpecialUnsafeLockMode = 04600;
  constexpr mode_t kPortableUnsafeLockMode = 0610;
  mode_t unsafe_lock_mode = kSpecialUnsafeLockMode;
  ASSERT_EQ(::chmod(unsafe_lock_path.c_str(), unsafe_lock_mode), 0);
  // Some systems discard special bits on non-executable regular files even
  // when chmod succeeds; the fallback still exercises exact-mode rejection.
  if (permissions_of(unsafe_lock_path) != unsafe_lock_mode) {
    unsafe_lock_mode = kPortableUnsafeLockMode;
    ASSERT_EQ(::chmod(unsafe_lock_path.c_str(), unsafe_lock_mode), 0);
  }
  ASSERT_EQ(permissions_of(unsafe_lock_path), unsafe_lock_mode);
  DaemonProcess special_mode_lock_daemon;
  special_mode_lock_daemon.start(unsafe_lock_socket);
  EXPECT_TRUE(special_mode_lock_daemon.wait_for_exit(std::chrono::seconds(5)));
  EXPECT_TRUE(special_mode_lock_daemon.exited_with_failure());
  EXPECT_EQ(permissions_of(unsafe_lock_path), unsafe_lock_mode);
  std::filesystem::remove(unsafe_lock_path);

  const std::filesystem::path lock_target =
      unsafe_lock_temp.path() / "lock-target";
  {
    std::ofstream file(lock_target);
    file << "preserve";
  }
  std::filesystem::create_symlink(lock_target, unsafe_lock_path);
  DaemonProcess symlink_lock_daemon;
  symlink_lock_daemon.start(unsafe_lock_socket);
  EXPECT_TRUE(symlink_lock_daemon.wait_for_exit(std::chrono::seconds(5)));
  EXPECT_TRUE(symlink_lock_daemon.exited_with_failure());
  EXPECT_TRUE(std::filesystem::is_symlink(unsafe_lock_path));
  std::filesystem::remove(unsafe_lock_path);

  std::filesystem::create_directory(unsafe_lock_path);
  DaemonProcess directory_lock_daemon;
  directory_lock_daemon.start(unsafe_lock_socket);
  EXPECT_TRUE(directory_lock_daemon.wait_for_exit(std::chrono::seconds(5)));
  EXPECT_TRUE(directory_lock_daemon.exited_with_failure());
  EXPECT_TRUE(std::filesystem::is_directory(unsafe_lock_path));

  const std::string long_socket =
      (temp.path() / std::string(160, 'x')).string();
  DaemonProcess too_long;
  too_long.start(long_socket);
  EXPECT_TRUE(too_long.wait_for_exit(std::chrono::seconds(5)));
  EXPECT_TRUE(too_long.exited_with_failure());
  EXPECT_FALSE(std::filesystem::exists(long_socket));

  ScopedDaemonDirectory xdg("psix", true);
  const std::string default_path =
      (xdg.path() / "photospider" / "photospiderd-v2.sock").string();
  DaemonProcess default_daemon;
  default_daemon.start(default_path, false, xdg.path().string());
  ASSERT_TRUE(default_daemon.wait_ready(std::chrono::seconds(5)));
  EXPECT_EQ(permissions_of(xdg.path() / "photospider"), 0700);
  EXPECT_EQ(permissions_of(default_path), 0600);
  default_daemon.stop();
  EXPECT_TRUE(default_daemon.exited_successfully());
  EXPECT_FALSE(std::filesystem::exists(default_path));

  const std::filesystem::path long_xdg = temp.path() / std::string(55, 'r');
  std::filesystem::create_directories(long_xdg);
  ASSERT_EQ(::chmod(long_xdg.c_str(), 0700), 0);
  const std::string fallback_path = "/tmp/photospider-" +
                                    std::to_string(::geteuid()) +
                                    "/photospiderd-v2.sock";
  DaemonProcess fallback_daemon;
  fallback_daemon.start(fallback_path, false, long_xdg.string());
  ASSERT_TRUE(fallback_daemon.wait_ready(std::chrono::seconds(5)));
  fallback_daemon.stop();
  EXPECT_TRUE(fallback_daemon.exited_successfully());
  EXPECT_FALSE(std::filesystem::exists(fallback_path));
}

TEST(IpcDaemonLifecycle, PreservesRegularFileReplacementAfterBind) {
  ScopedDaemonDirectory temp("ps-listener-regular", true);
  const std::filesystem::path socket_path = temp.path() / "listener.sock";
  ListenerReplacementState state;
  internal::ServerLifecycleTestDependencies dependencies;
  dependencies.context = &state;
  dependencies.stage_hook = replace_listener_path;

  const OperationStatus status =
      internal::test_create_listener(socket_path.string(), dependencies);

  EXPECT_FALSE(status.ok);
  EXPECT_EQ(state.error, 0) << std::strerror(state.error);
  EXPECT_TRUE(std::filesystem::is_regular_file(socket_path));
}

TEST(IpcDaemonLifecycle, PreservesSymlinkReplacementAfterBind) {
  ScopedDaemonDirectory temp("ps-listener-symlink", true);
  const std::filesystem::path socket_path = temp.path() / "listener.sock";
  const std::filesystem::path target_path = temp.path() / "target";
  {
    std::ofstream target(target_path);
    ASSERT_TRUE(target.is_open());
    target << "preserve";
  }
  ASSERT_EQ(::chmod(target_path.c_str(), 0640), 0);
  ListenerReplacementState state;
  state.kind = ListenerReplacementState::Kind::Symlink;
  state.symlink_target = target_path.string();
  internal::ServerLifecycleTestDependencies dependencies;
  dependencies.context = &state;
  dependencies.stage_hook = replace_listener_path;

  const OperationStatus status =
      internal::test_create_listener(socket_path.string(), dependencies);

  EXPECT_FALSE(status.ok);
  EXPECT_EQ(state.error, 0) << std::strerror(state.error);
  EXPECT_TRUE(std::filesystem::is_symlink(socket_path));
  EXPECT_EQ(permissions_of(target_path), 0640);
}

TEST(IpcDaemonLifecycle, PreservesSocketReplacementBeforeListen) {
  ScopedDaemonDirectory temp("ps-listener-socket", true);
  const std::filesystem::path socket_path = temp.path() / "listener.sock";
  ListenerReplacementState state;
  state.kind = ListenerReplacementState::Kind::Socket;
  state.stage = internal::ServerLifecycleTestStage::AfterCandidateBeforeListen;
  internal::ServerLifecycleTestDependencies dependencies;
  dependencies.context = &state;
  dependencies.stage_hook = replace_listener_path;

  const OperationStatus status =
      internal::test_create_listener(socket_path.string(), dependencies);

  EXPECT_FALSE(status.ok);
  EXPECT_EQ(state.error, 0) << std::strerror(state.error);
  struct stat replacement{};
  EXPECT_EQ(::lstat(socket_path.c_str(), &replacement), 0);
  EXPECT_TRUE(S_ISSOCK(replacement.st_mode));
  close_replacement_socket(&state);
}

TEST(IpcDaemonLifecycle, PreservesReplacementAfterListenBeforeProof) {
  ScopedDaemonDirectory temp("ps-listener-after-listen", true);
  const std::filesystem::path socket_path = temp.path() / "listener.sock";
  ListenerReplacementState state;
  state.stage = internal::ServerLifecycleTestStage::AfterListenBeforeProof;
  internal::ServerLifecycleTestDependencies dependencies;
  dependencies.context = &state;
  dependencies.stage_hook = replace_listener_path;

  const OperationStatus status =
      internal::test_create_listener(socket_path.string(), dependencies);

  EXPECT_FALSE(status.ok);
  EXPECT_EQ(state.error, 0) << std::strerror(state.error);
  EXPECT_TRUE(std::filesystem::is_regular_file(socket_path));
}

TEST(IpcDaemonLifecycle, PreservesReplacementAfterProofBeforeRevalidation) {
  ScopedDaemonDirectory temp("ps-listener-after-proof", true);
  const std::filesystem::path socket_path = temp.path() / "listener.sock";
  ListenerReplacementState state;
  state.stage =
      internal::ServerLifecycleTestStage::AfterProofBeforeFinalRevalidate;
  internal::ServerLifecycleTestDependencies dependencies;
  dependencies.context = &state;
  dependencies.stage_hook = replace_listener_path;

  const OperationStatus status =
      internal::test_create_listener(socket_path.string(), dependencies);

  EXPECT_FALSE(status.ok);
  EXPECT_EQ(state.error, 0) << std::strerror(state.error);
  EXPECT_TRUE(std::filesystem::is_regular_file(socket_path));
}

TEST(IpcDaemonLifecycle,
     RejectsExactModeUnlistenedSocketReplacementBeforeCandidateCapture) {
  ScopedDaemonDirectory temp("ps-listener-unlistened", true);
  const std::filesystem::path socket_path = temp.path() / "listener.sock";
  ListenerReplacementState state;
  state.kind = ListenerReplacementState::Kind::Socket;
  internal::ServerLifecycleTestDependencies dependencies;
  dependencies.context = &state;
  dependencies.stage_hook = replace_listener_path;

  const OperationStatus status =
      internal::test_create_listener(socket_path.string(), dependencies);

  EXPECT_FALSE(status.ok);
  EXPECT_EQ(state.error, 0) << std::strerror(state.error);
  struct stat replacement{};
  ASSERT_EQ(::lstat(socket_path.c_str(), &replacement), 0);
  EXPECT_TRUE(S_ISSOCK(replacement.st_mode));
  EXPECT_EQ(replacement.st_mode & 07777, 0600);
  EXPECT_EQ(replacement.st_dev, state.replacement_device);
  EXPECT_EQ(replacement.st_ino, state.replacement_inode);
  close_replacement_socket(&state);
}

TEST(IpcDaemonLifecycle,
     RejectsExactModeListeningSocketReplacementBeforeCandidateCapture) {
  ScopedDaemonDirectory temp("ps-listener-listening", true);
  const std::filesystem::path socket_path = temp.path() / "listener.sock";
  ListenerReplacementState state;
  state.kind = ListenerReplacementState::Kind::Socket;
  state.listen_on_replacement = true;
  internal::ServerLifecycleTestDependencies dependencies;
  dependencies.context = &state;
  dependencies.stage_hook = replace_listener_path;

  const OperationStatus status =
      internal::test_create_listener(socket_path.string(), dependencies);

  EXPECT_FALSE(status.ok);
  EXPECT_EQ(state.error, 0) << std::strerror(state.error);
  struct stat replacement{};
  ASSERT_EQ(::lstat(socket_path.c_str(), &replacement), 0);
  EXPECT_TRUE(S_ISSOCK(replacement.st_mode));
  EXPECT_EQ(replacement.st_mode & 07777, 0600);
  EXPECT_EQ(replacement.st_dev, state.replacement_device);
  EXPECT_EQ(replacement.st_ino, state.replacement_inode);
  close_replacement_socket(&state);
}

TEST(IpcDaemonLifecycle, ListenFailurePreservesStaleSocketForNextStartup) {
  ScopedDaemonDirectory matching_temp("ps-listener-match", true);
  const std::filesystem::path matching_path =
      matching_temp.path() / "listener.sock";
  internal::ServerLifecycleTestDependencies matching_dependencies;
  matching_dependencies.listen_call = fail_listener;

  const OperationStatus matching_status = internal::test_create_listener(
      matching_path.string(), matching_dependencies);

  EXPECT_FALSE(matching_status.ok);
  EXPECT_NE(matching_status.message.find(std::strerror(EIO)),
            std::string::npos);
  EXPECT_TRUE(std::filesystem::is_socket(matching_path));

  const OperationStatus successor_status = internal::test_create_listener(
      matching_path.string(), internal::ServerLifecycleTestDependencies{});
  EXPECT_TRUE(successor_status.ok) << successor_status.message;
  EXPECT_FALSE(std::filesystem::exists(matching_path));

  ScopedDaemonDirectory replaced_temp("ps-listener-replaced", true);
  const std::filesystem::path replaced_path =
      replaced_temp.path() / "listener.sock";
  ListenerReplacementState state;
  state.kind = ListenerReplacementState::Kind::Socket;
  state.stage = internal::ServerLifecycleTestStage::AfterCandidateBeforeListen;
  internal::ServerLifecycleTestDependencies replaced_dependencies;
  replaced_dependencies.context = &state;
  replaced_dependencies.stage_hook = replace_listener_path;
  replaced_dependencies.listen_call = fail_listener;

  const OperationStatus replaced_status = internal::test_create_listener(
      replaced_path.string(), replaced_dependencies);

  EXPECT_FALSE(replaced_status.ok);
  EXPECT_EQ(state.error, 0) << std::strerror(state.error);
  struct stat replacement{};
  EXPECT_EQ(::lstat(replaced_path.c_str(), &replacement), 0);
  EXPECT_TRUE(S_ISSOCK(replacement.st_mode));
  close_replacement_socket(&state);
}

TEST(IpcDaemonLifecycle, CreatesSocketWithExactModeAtBind) {
  ScopedDaemonDirectory temp("ps-listener-mode", true);
  const std::filesystem::path socket_path = temp.path() / "listener.sock";
  ListenerReplacementState state;
  state.kind = ListenerReplacementState::Kind::None;
  internal::ServerLifecycleTestDependencies dependencies;
  dependencies.context = &state;
  dependencies.stage_hook = replace_listener_path;

  const OperationStatus status =
      internal::test_create_listener(socket_path.string(), dependencies);

  EXPECT_TRUE(status.ok) << status.message;
  EXPECT_EQ(state.error, 0) << std::strerror(state.error);
  EXPECT_EQ(state.initial_mode, 0600);
  EXPECT_FALSE(std::filesystem::exists(socket_path));
}

class IpcDaemonFragmentedProofTest
    : public ::testing::TestWithParam<std::size_t> {};  // NOLINT

TEST_P(IpcDaemonFragmentedProofTest,
       RetainsMatchingPrefixUntilCompleteProofFrameArrives) {
  ScopedDaemonDirectory temp("ps-listener-fragmented-proof", true);
  const std::filesystem::path socket_path = temp.path() / "listener.sock";
  ProofPrefixState state;
  internal::ServerLifecycleTestDependencies dependencies;
  dependencies.context = &state;
  dependencies.proof_write_chunk_bytes = GetParam();
  dependencies.proof_prefix_observer = observe_proof_prefix;

  const OperationStatus status =
      internal::test_create_listener(socket_path.string(), dependencies);

  EXPECT_TRUE(status.ok) << status.message;
  EXPECT_GE(state.largest_observed.load(std::memory_order_acquire), GetParam());
  EXPECT_LT(state.largest_observed.load(std::memory_order_acquire), 36U);
}

INSTANTIATE_TEST_SUITE_P(FragmentBoundaries, IpcDaemonFragmentedProofTest,
                         ::testing::Values(1U, 4U, 35U));

class IpcDaemonActiveReplacementTest
    : public ::testing::TestWithParam<std::tuple<int, bool>> {};  // NOLINT

TEST_P(IpcDaemonActiveReplacementTest,
       PreservesReplacementAndCleansBeforeClosingListener) {
  ScopedDaemonDirectory temp("ps-listener-active-replacement", true);
  const std::filesystem::path parent = temp.path() / "daemon";
  ASSERT_TRUE(std::filesystem::create_directory(parent));
  ASSERT_EQ(::chmod(parent.c_str(), 0700), 0);
  const std::filesystem::path socket_path = parent / "listener.sock";
  const std::filesystem::path target_path = temp.path() / "target";
  {
    std::ofstream target(target_path);
    ASSERT_TRUE(target.is_open());
    target << "preserve";
  }
  ListenerReplacementState state;
  state.stage =
      internal::ServerLifecycleTestStage::AfterActivationBeforeRuntimeStart;
  state.kind =
      static_cast<ListenerReplacementState::Kind>(std::get<0>(GetParam()));
  state.symlink_target = target_path.string();
  internal::ServerLifecycleTestDependencies dependencies;
  dependencies.context = &state;
  dependencies.stage_hook = replace_listener_path;
  dependencies.fail_runtime_start = std::get<1>(GetParam());
  dependencies.before_active_cleanup = observe_active_cleanup;
  int stop_descriptors[2] = {-1, -1};
  ASSERT_EQ(::pipe(stop_descriptors), 0);
  internal::UniqueFd stop_reader(stop_descriptors[0]);
  internal::UniqueFd stop_writer(stop_descriptors[1]);
  if (!dependencies.fail_runtime_start) {
    state.stop_writer = stop_writer.get();
  }
  std::unique_ptr<Host> host = create_embedded_host();
  internal::Server server(*host, "active-replacement-test");

  const OperationStatus status = internal::test_run_server(
      server, internal::ServerOptions{socket_path.string()}, stop_reader.get(),
      dependencies);

  EXPECT_EQ(status.ok, !dependencies.fail_runtime_start) << status.message;
  EXPECT_EQ(state.error, 0) << std::strerror(state.error);
  EXPECT_TRUE(state.listener_open_during_cleanup);
  if (state.kind == ListenerReplacementState::Kind::RegularFile) {
    EXPECT_TRUE(std::filesystem::is_regular_file(socket_path));
  } else if (state.kind == ListenerReplacementState::Kind::Symlink) {
    ASSERT_TRUE(std::filesystem::is_symlink(socket_path));
    EXPECT_EQ(std::filesystem::read_symlink(socket_path), target_path);
  } else {
    struct stat replacement{};
    ASSERT_EQ(::lstat(socket_path.c_str(), &replacement), 0);
    EXPECT_TRUE(S_ISSOCK(replacement.st_mode));
    EXPECT_EQ(replacement.st_dev, state.replacement_device);
    EXPECT_EQ(replacement.st_ino, state.replacement_inode);
  }
  close_replacement_socket(&state);
}

INSTANTIATE_TEST_SUITE_P(
    NormalAndRouterFailure, IpcDaemonActiveReplacementTest,
    ::testing::Values(
        std::make_tuple(
            static_cast<int>(ListenerReplacementState::Kind::RegularFile),
            false),
        std::make_tuple(
            static_cast<int>(ListenerReplacementState::Kind::Symlink), false),
        std::make_tuple(
            static_cast<int>(ListenerReplacementState::Kind::Socket), false),
        std::make_tuple(
            static_cast<int>(ListenerReplacementState::Kind::RegularFile),
            true),
        std::make_tuple(
            static_cast<int>(ListenerReplacementState::Kind::Symlink), true),
        std::make_tuple(
            static_cast<int>(ListenerReplacementState::Kind::Socket), true)));

TEST(IpcDaemonLifecycle, ParentRenameRecreateFailsClosedAtActiveCleanup) {
  ScopedDaemonDirectory temp("ps-listener-parent-replacement", true);
  const std::filesystem::path parent = temp.path() / "daemon";
  const std::filesystem::path moved_parent = temp.path() / "daemon-moved";
  ASSERT_TRUE(std::filesystem::create_directory(parent));
  ASSERT_EQ(::chmod(parent.c_str(), 0700), 0);
  ParentReplacementState state;
  state.parent = parent.string();
  state.moved_parent = moved_parent.string();
  internal::ServerLifecycleTestDependencies dependencies;
  dependencies.context = &state;
  dependencies.stage_hook = replace_parent_path;

  const OperationStatus status = internal::test_create_listener(
      (parent / "listener.sock").string(), dependencies);

  EXPECT_TRUE(status.ok) << status.message;
  EXPECT_EQ(state.error, 0) << std::strerror(state.error);
  EXPECT_TRUE(std::filesystem::is_directory(parent));
  EXPECT_TRUE(std::filesystem::is_socket(moved_parent / "listener.sock"));
}

TEST(IpcDaemonLifecycle, MatchingPrefixThatNeverCompletesFailsAtProofDeadline) {
  ScopedDaemonDirectory temp("ps-listener-stalled-prefix", true);
  const std::filesystem::path socket_path = temp.path() / "listener.sock";
  internal::ServerLifecycleTestDependencies dependencies;
  dependencies.proof_write_chunk_bytes = 1;
  dependencies.proof_write_limit_bytes = 1;
  const auto started = std::chrono::steady_clock::now();

  const OperationStatus status =
      internal::test_create_listener(socket_path.string(), dependencies);

  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_FALSE(status.ok);
  EXPECT_NE(status.message.find("deadline"), std::string::npos);
  EXPECT_LT(elapsed, std::chrono::seconds(3));
}

TEST(IpcDaemonLifecycle, RejectsProofWriteLimitBeyondFixedFrame) {
  ScopedDaemonDirectory temp("ps-listener-proof-overrun", true);
  const std::filesystem::path socket_path = temp.path() / "listener.sock";
  internal::ServerLifecycleTestDependencies dependencies;
  dependencies.proof_write_limit_bytes =
      std::numeric_limits<std::size_t>::max();

  const OperationStatus status =
      internal::test_create_listener(socket_path.string(), dependencies);

  EXPECT_FALSE(status.ok);
  EXPECT_NE(status.message.find("write limit exceeds fixed frame"),
            std::string::npos);
  EXPECT_TRUE(std::filesystem::is_socket(socket_path));
}

TEST(IpcDaemonLifecycle,
     ClearsBorrowedDependenciesAfterPathSetupExceptionBeforeReuse) {
  ScopedDaemonDirectory temp("ps-listener-dependency-borrow", true);
  const std::string socket_path = (temp.path() / "listener.sock").string();
  std::unique_ptr<Host> host = create_embedded_host();
  auto server = std::make_unique<internal::Server>(*host, "borrow-test");
  std::atomic<std::size_t> cleanup_calls{0};
  std::optional<internal::ServerLifecycleTestDependencies> dependencies(
      std::in_place);
  dependencies->context = &cleanup_calls;
  dependencies->before_active_cleanup = count_active_cleanup;
  dependencies->fail_path_setup = true;

  EXPECT_THROW(
      internal::test_run_server(*server, internal::ServerOptions{socket_path},
                                -1, *dependencies),
      std::filesystem::filesystem_error);
  dependencies.reset();

  const OperationStatus reused =
      server->run(internal::ServerOptions{"relative.sock"}, -1);
  EXPECT_FALSE(reused.ok);
  EXPECT_EQ(cleanup_calls.load(std::memory_order_relaxed), 0U);
  server.reset();
  EXPECT_EQ(cleanup_calls.load(std::memory_order_relaxed), 0U);
}

TEST(IpcDaemonLifecycle, StartupStopInterruptsStalledListenerProof) {
  ScopedDaemonDirectory temp("ps-listener-cancelled-proof", true);
  const std::string socket_path = (temp.path() / "listener.sock").string();
  std::unique_ptr<Host> host = create_embedded_host();
  internal::Server server(*host, "cancelled-proof-test");
  internal::ServerLifecycleTestDependencies dependencies;
  dependencies.proof_write_chunk_bytes = 1;
  dependencies.proof_write_limit_bytes = 1;
  int stop_descriptors[2] = {-1, -1};
  ASSERT_EQ(::pipe(stop_descriptors), 0);
  internal::UniqueFd stop_reader(stop_descriptors[0]);
  internal::UniqueFd stop_writer(stop_descriptors[1]);
  OperationStatus status;
  const auto started = std::chrono::steady_clock::now();
  std::thread runner([&] {
    status =
        internal::test_run_server(server, internal::ServerOptions{socket_path},
                                  stop_reader.get(), dependencies);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const char token = 's';
  ASSERT_EQ(::write(stop_writer.get(), &token, 1), 1);
  runner.join();

  EXPECT_FALSE(status.ok);
  EXPECT_NE(status.message.find("cancelled"), std::string::npos);
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::seconds(1));
}

TEST(IpcDaemonLifecycle,
     PreservesQueuedClientFrameAndCountsItAgainstWorkerLimit) {
  ScopedDaemonDirectory temp("ps-listener-pending", true);
  const std::string socket_path = (temp.path() / "listener.sock").string();
  std::unique_ptr<Host> host = create_embedded_host();
  internal::Server server(*host, "lifecycle-test");
  PendingClientState state;
  state.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  internal::ServerLifecycleTestDependencies dependencies;
  dependencies.context = &state;
  dependencies.stage_hook = queue_pending_clients;
  int stop_descriptors[2] = {-1, -1};
  ASSERT_EQ(::pipe(stop_descriptors), 0);
  internal::UniqueFd stop_reader(stop_descriptors[0]);
  internal::UniqueFd stop_writer(stop_descriptors[1]);
  OperationStatus run_status;
  std::thread server_thread([&] {
    run_status =
        internal::test_run_server(server, internal::ServerOptions{socket_path},
                                  stop_reader.get(), dependencies);
  });

  const auto hook_deadline = state.deadline;
  while (!state.hook_complete.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < hook_deadline) {
    std::this_thread::yield();
  }
  bool all_responses_complete =
      state.hook_complete.load(std::memory_order_acquire) && state.error == 0 &&
      state.count == state.clients.size();
  std::string response_error;
  for (internal::UniqueFd& client : state.clients) {
    if (!all_responses_complete) {
      break;
    }
    const internal::FrameReadResult response = read_frame_before_deadline(
        client.get(),
        std::chrono::steady_clock::now() + std::chrono::seconds(5));
    if (response.state != internal::FrameReadState::Complete ||
        response.payload.find("\"id\":\"pending\"") == std::string::npos ||
        response.payload.find("\"pong\":true") == std::string::npos) {
      all_responses_complete = false;
      response_error =
          response.message.empty() ? response.payload : response.message;
      break;
    }
  }

  bool excess_rejected = false;
  if (state.hook_complete.load(std::memory_order_acquire)) {
    Client excess;
    const OperationStatus excess_connect = excess.connect(socket_path);
    excess_rejected = excess_connect.ok && !excess.ping().status.ok;
    excess.disconnect();
  } else {
    state.cancel.store(true, std::memory_order_release);
  }
  const char stop_token = 's';
  const ssize_t stop_written = ::write(stop_writer.get(), &stop_token, 1);
  if (stop_written != 1) {
    stop_writer.reset();
  }
  server_thread.join();
  for (internal::UniqueFd& client : state.clients) {
    client.reset();
  }
  EXPECT_EQ(stop_written, 1);
  EXPECT_TRUE(state.hook_complete.load(std::memory_order_acquire));
  EXPECT_EQ(state.error, 0) << std::strerror(state.error);
  EXPECT_EQ(state.count, state.clients.size());
  EXPECT_TRUE(all_responses_complete) << response_error;
  EXPECT_TRUE(excess_rejected);
  EXPECT_TRUE(run_status.ok) << run_status.message;
  EXPECT_FALSE(std::filesystem::exists(socket_path));
}

TEST(IpcDaemonConnectionDeadlines,
     RecoversCapacityAfterThirtyTwoIdleClientsExpire) {
  ScopedDaemonDirectory temp("ps-ordinary-idle-capacity", true);
  const std::string socket_path = (temp.path() / "listener.sock").string();
  std::unique_ptr<Host> host = create_embedded_host();
  internal::Server server(*host, "ordinary-deadline-test");
  PendingClientState state;
  state.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  internal::ServerLifecycleTestDependencies dependencies;
  dependencies.context = &state;
  dependencies.stage_hook = queue_pending_clients;
  int stop_descriptors[2] = {-1, -1};
  ASSERT_EQ(::pipe(stop_descriptors), 0);
  internal::UniqueFd stop_reader(stop_descriptors[0]);
  internal::UniqueFd stop_writer(stop_descriptors[1]);
  OperationStatus run_status;
  std::thread server_thread([&] {
    run_status =
        internal::test_run_server(server, internal::ServerOptions{socket_path},
                                  stop_reader.get(), dependencies);
  });

  while (!state.hook_complete.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < state.deadline) {
    std::this_thread::yield();
  }
  bool all_responses_complete =
      state.hook_complete.load(std::memory_order_acquire) && state.error == 0 &&
      state.count == state.clients.size();
  std::string response_error;
  for (internal::UniqueFd& client : state.clients) {
    if (!all_responses_complete) {
      break;
    }
    const internal::FrameReadResult response =
        read_frame_before_deadline(client.get(), state.deadline);
    if (response.state != internal::FrameReadState::Complete ||
        response.payload.find("\"id\":\"pending\"") == std::string::npos ||
        response.payload.find("\"pong\":true") == std::string::npos) {
      all_responses_complete = false;
      response_error =
          response.message.empty() ? response.payload : response.message;
      break;
    }
  }

  std::string excess_message;
  const bool excess_rejected = !ping_before_deadline(
      socket_path,
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500),
      &excess_message);
  const bool capacity_recovered =
      excess_rejected &&
      wait_for_connection_capacity(socket_path, std::chrono::seconds(3));

  const char stop_token = 's';
  const ssize_t stop_written = ::write(stop_writer.get(), &stop_token, 1);
  if (stop_written != 1) {
    stop_writer.reset();
  }
  server_thread.join();
  for (internal::UniqueFd& client : state.clients) {
    client.reset();
  }

  EXPECT_EQ(stop_written, 1);
  EXPECT_TRUE(state.hook_complete.load(std::memory_order_acquire));
  EXPECT_EQ(state.error, 0) << std::strerror(state.error);
  EXPECT_EQ(state.count, state.clients.size());
  EXPECT_TRUE(all_responses_complete) << response_error;
  EXPECT_TRUE(excess_rejected) << excess_message;
  EXPECT_TRUE(capacity_recovered);
  EXPECT_TRUE(run_status.ok) << run_status.message;
  EXPECT_FALSE(std::filesystem::exists(socket_path));
}

TEST(IpcDaemonConnectionDeadlines, ClosesClientThatStallsInFrameHeader) {
  std::unique_ptr<Host> host = create_embedded_host();
  ScopedOrdinaryDeadlineServer server(*host, kShortOrdinaryStageTimeout);
  const auto test_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  std::string message;
  internal::UniqueFd client =
      connect_before_deadline(server.socket_path(), test_deadline, &message);
  ASSERT_TRUE(client) << message;
  const unsigned char partial_header = 0;
  ASSERT_TRUE(send_before_deadline(client.get(), &partial_header,
                                   sizeof(partial_header), test_deadline,
                                   &message))
      << message;

  const bool peer_closed =
      wait_for_peer_hangup(client.get(), test_deadline, &message);
  const bool later_client_served =
      peer_closed && wait_for_connection_capacity(server.socket_path(),
                                                  std::chrono::seconds(1));
  const OperationStatus run_status = server.finish();

  EXPECT_TRUE(peer_closed) << message;
  EXPECT_TRUE(later_client_served);
  EXPECT_TRUE(run_status.ok) << run_status.message;
}

TEST(IpcDaemonConnectionDeadlines, ClosesClientThatStallsInFramePayload) {
  std::unique_ptr<Host> host = create_embedded_host();
  ScopedOrdinaryDeadlineServer server(*host, kShortOrdinaryStageTimeout);
  const auto test_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  std::string message;
  internal::UniqueFd client =
      connect_before_deadline(server.socket_path(), test_deadline, &message);
  ASSERT_TRUE(client) << message;
  const std::uint32_t network_length = htonl(64);
  const unsigned char partial_payload = '{';
  ASSERT_TRUE(send_before_deadline(client.get(), &network_length,
                                   sizeof(network_length), test_deadline,
                                   &message))
      << message;
  ASSERT_TRUE(send_before_deadline(client.get(), &partial_payload,
                                   sizeof(partial_payload), test_deadline,
                                   &message))
      << message;

  const bool peer_closed =
      wait_for_peer_hangup(client.get(), test_deadline, &message);
  const bool later_client_served =
      peer_closed && wait_for_connection_capacity(server.socket_path(),
                                                  std::chrono::seconds(1));
  const OperationStatus run_status = server.finish();

  EXPECT_TRUE(peer_closed) << message;
  EXPECT_TRUE(later_client_served);
  EXPECT_TRUE(run_status.ok) << run_status.message;
}

TEST(IpcDaemonConnectionDeadlines, ClosesClientThatBackpressuresFrameWrite) {
  ps::testing::IpcHostSpy host;
  constexpr std::size_t kLargeResponseBytes = 8U * 1024U * 1024U;
  host.set_ops_combined_sources(
      {{"large", std::string(kLargeResponseBytes, 'x')}});
  ScopedOrdinaryDeadlineServer server(host, kShortOrdinaryStageTimeout);
  const auto test_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  std::string message;
  internal::UniqueFd client =
      connect_before_deadline(server.socket_path(), test_deadline, &message);
  ASSERT_TRUE(client) << message;
  const int receive_buffer_bytes = 1024;
  ASSERT_EQ(::setsockopt(client.get(), SOL_SOCKET, SO_RCVBUF,
                         &receive_buffer_bytes, sizeof(receive_buffer_bytes)),
            0)
      << std::strerror(errno);
  const std::string request = internal::Json{
      {"protocol_version", kProtocolVersion},
      {"id", "write-deadline"},
      {"method", "plugins.ops_combined_sources"},
      {"params",
       internal::Json::object()}}.dump();
  const std::uint32_t request_network_length =
      htonl(static_cast<std::uint32_t>(request.size()));
  ASSERT_TRUE(send_before_deadline(client.get(), &request_network_length,
                                   sizeof(request_network_length),
                                   test_deadline, &message))
      << message;
  ASSERT_TRUE(send_before_deadline(client.get(), request.data(), request.size(),
                                   test_deadline, &message))
      << message;

  std::string close_message;
  const bool peer_closed =
      wait_for_peer_hangup(client.get(), test_deadline, &close_message);
  const bool later_client_served =
      peer_closed && wait_for_connection_capacity(server.socket_path(),
                                                  std::chrono::seconds(1));
  const OperationStatus run_status = server.finish();
  std::string drain_message;
  const std::vector<unsigned char> response_bytes = drain_closed_peer_bytes(
      client.get(), kMaximumFramePayloadBytes + sizeof(std::uint32_t),
      &drain_message);

  EXPECT_TRUE(peer_closed) << close_message;
  EXPECT_TRUE(later_client_served);
  EXPECT_TRUE(run_status.ok) << run_status.message;
  EXPECT_EQ(host.call_count("plugins.ops_combined_sources"), 1U);
  ASSERT_TRUE(drain_message.empty()) << drain_message;
  EXPECT_LT(response_bytes.size(), kLargeResponseBytes);
}

TEST(IpcDaemonLifecycle,
     PreservesMatchingPartialOrdinaryFrameAfterListenerProof) {
  ScopedDaemonDirectory temp("ps-listener-partial-pending", true);
  const std::string socket_path = (temp.path() / "listener.sock").string();
  std::unique_ptr<Host> host = create_embedded_host();
  internal::Server server(*host, "partial-pending-test");
  PendingClientState state;
  state.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  state.target_count = 1;
  state.initial_frame_bytes = 3;
  internal::ServerLifecycleTestDependencies dependencies;
  dependencies.context = &state;
  dependencies.stage_hook = queue_pending_clients;
  int stop_descriptors[2] = {-1, -1};
  ASSERT_EQ(::pipe(stop_descriptors), 0);
  internal::UniqueFd stop_reader(stop_descriptors[0]);
  internal::UniqueFd stop_writer(stop_descriptors[1]);
  OperationStatus run_status;
  std::thread server_thread([&] {
    run_status =
        internal::test_run_server(server, internal::ServerOptions{socket_path},
                                  stop_reader.get(), dependencies);
  });

  while (!state.activated.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < state.deadline) {
    std::this_thread::yield();
  }
  const std::vector<unsigned char> frame = pending_ping_frame();
  std::size_t written = state.initial_frame_bytes;
  while (state.activated.load(std::memory_order_acquire) && state.error == 0 &&
         written < frame.size() &&
         std::chrono::steady_clock::now() < state.deadline) {
    int send_flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
    send_flags |= MSG_NOSIGNAL;
#endif
    const ssize_t result =
        ::send(state.clients[0].get(), frame.data() + written,
               frame.size() - written, send_flags);
    if (result > 0) {
      written += static_cast<std::size_t>(result);
    } else if (result < 0 && errno != EINTR && errno != EAGAIN &&
               errno != EWOULDBLOCK) {
      state.error = errno;
    }
  }
  const internal::FrameReadResult response =
      written == frame.size()
          ? read_frame_before_deadline(state.clients[0].get(), state.deadline)
          : internal::FrameReadResult{};
  const bool response_ready =
      response.state == internal::FrameReadState::Complete;
  const char stop_token = 's';
  const ssize_t stop_written = ::write(stop_writer.get(), &stop_token, 1);
  if (stop_written != 1) {
    stop_writer.reset();
  }
  server_thread.join();

  EXPECT_TRUE(state.hook_complete.load(std::memory_order_acquire));
  EXPECT_TRUE(state.activated.load(std::memory_order_acquire));
  EXPECT_EQ(state.error, 0) << std::strerror(state.error);
  EXPECT_EQ(state.count, 1U);
  EXPECT_EQ(written, frame.size());
  EXPECT_TRUE(response_ready) << response.message;
  EXPECT_EQ(response.state, internal::FrameReadState::Complete)
      << response.message;
  EXPECT_NE(response.payload.find("\"id\":\"pending\""), std::string::npos)
      << response.payload;
  EXPECT_NE(response.payload.find("\"pong\":true"), std::string::npos)
      << response.payload;
  EXPECT_EQ(stop_written, 1);
  EXPECT_TRUE(run_status.ok) << run_status.message;
  EXPECT_FALSE(std::filesystem::exists(socket_path));
}

TEST(IpcDaemonLifecycle, SerializesConcurrentStaleReclaimWithPersistentLock) {
  ScopedDaemonDirectory temp("photospider-ipc-daemon-race");
  const std::string socket_path = (temp.path() / "race.sock").string();
  const std::filesystem::path lock_path = socket_path + ".lock";

  internal::UniqueFd stale_socket(::socket(AF_UNIX, SOCK_STREAM, 0));
  ASSERT_TRUE(stale_socket);
  sockaddr_un stale_address{};
  stale_address.sun_family = AF_UNIX;
  std::memcpy(stale_address.sun_path, socket_path.c_str(),
              socket_path.size() + 1);
  const socklen_t stale_length = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + socket_path.size() + 1);
  ASSERT_EQ(::bind(stale_socket.get(),
                   reinterpret_cast<sockaddr*>(&stale_address), stale_length),
            0);
  stale_socket.reset();

  struct stat stale_before{};
  ASSERT_EQ(::lstat(socket_path.c_str(), &stale_before), 0);
  internal::UniqueFd parent_lock(::open(
      lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
  ASSERT_TRUE(parent_lock);
  ASSERT_EQ(::flock(parent_lock.get(), LOCK_EX | LOCK_NB), 0);
  DaemonProcess blocked;
  blocked.start(socket_path);
  ASSERT_TRUE(blocked.wait_for_exit(std::chrono::seconds(5)));
  EXPECT_TRUE(blocked.exited_with_code(1));
  struct stat stale_after{};
  ASSERT_EQ(::lstat(socket_path.c_str(), &stale_after), 0);
  EXPECT_EQ(stale_after.st_dev, stale_before.st_dev);
  EXPECT_EQ(stale_after.st_ino, stale_before.st_ino);
  parent_lock.reset();

  ConcurrentStartGate start_gate;
  DaemonProcess first;
  DaemonProcess second;
  first.start(socket_path, true, {}, start_gate.read_descriptor());
  second.start(socket_path, true, {}, start_gate.read_descriptor());
  start_gate.release(2);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline && !first.has_exited() &&
         !second.has_exited()) {
    (void)first.wait_for_exit(std::chrono::milliseconds(10));
    (void)second.wait_for_exit(std::chrono::milliseconds(10));
  }
  ASSERT_NE(first.has_exited(), second.has_exited());
  DaemonProcess* loser = first.has_exited() ? &first : &second;
  DaemonProcess* winner = first.has_exited() ? &second : &first;
  EXPECT_TRUE(loser->exited_with_code(1));
  ASSERT_TRUE(winner->wait_ready(std::chrono::seconds(5)));
  EXPECT_TRUE(std::filesystem::is_regular_file(lock_path));
  EXPECT_EQ(permissions_of(lock_path), 0600);
  struct stat persistent_identity{};
  ASSERT_EQ(::lstat(lock_path.c_str(), &persistent_identity), 0);
  EXPECT_EQ(persistent_identity.st_uid, ::geteuid());
  EXPECT_EQ(persistent_identity.st_nlink, 1);

  internal::UniqueFd lock_probe(
      ::open(lock_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW));
  ASSERT_TRUE(lock_probe);
  ASSERT_EQ(::flock(lock_probe.get(), LOCK_EX | LOCK_NB), -1);
  const int contention_error = errno;
  EXPECT_TRUE(contention_error == EWOULDBLOCK || contention_error == EAGAIN);

  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  EXPECT_TRUE(client.ping().status.ok);
  client.disconnect();
  winner->stop();
  EXPECT_TRUE(winner->exited_successfully());
  EXPECT_FALSE(std::filesystem::exists(socket_path));
  EXPECT_TRUE(std::filesystem::is_regular_file(lock_path));
  ASSERT_EQ(::flock(lock_probe.get(), LOCK_EX | LOCK_NB), 0);
  EXPECT_FALSE(std::filesystem::exists(socket_path));
  ASSERT_EQ(::flock(lock_probe.get(), LOCK_UN), 0);
  lock_probe.reset();

  DaemonProcess successor;
  successor.start(socket_path);
  ASSERT_TRUE(successor.wait_ready(std::chrono::seconds(5)));
  successor.stop();
  EXPECT_TRUE(successor.exited_successfully());
  EXPECT_FALSE(std::filesystem::exists(socket_path));
  EXPECT_TRUE(std::filesystem::is_regular_file(lock_path));
  struct stat successor_identity{};
  ASSERT_EQ(::lstat(lock_path.c_str(), &successor_identity), 0);
  EXPECT_EQ(successor_identity.st_dev, persistent_identity.st_dev);
  EXPECT_EQ(successor_identity.st_ino, persistent_identity.st_ino);
}

/**
 * @brief Persists graph state across clients and preserves exact Host status
 *        categories through the real daemon transport.
 *
 * @return Nothing; GoogleTest assertions report lifecycle, snapshot, protocol,
 *         or status mismatches.
 * @throws std::bad_alloc, std::runtime_error, filesystem exceptions, or system
 *         errors when fixture setup, process control, or IPC cannot complete.
 * @note Missing sessions and node mutation/projection targets remain NotFound
 *       over IPC; existing-target YAML, ROI, and destination failures retain
 *       their distinct InvalidYaml, InvalidParameter, and Io categories. A
 *       failed save leaves the remotely owned graph inspectable and retryable.
 */
TEST(IpcDaemonGraphLifecycle, PersistsAcrossClientsAndInspectsCopiedSnapshots) {
  ScopedDaemonDirectory temp("photospider-ipc-daemon-graph");
  const std::string socket_path = (temp.path() / "daemon.sock").string();
  const std::filesystem::path yaml_path = temp.path() / "source.yaml";
  write_ipc_graph(yaml_path);
  DaemonProcess daemon;
  daemon.start(socket_path);
  ASSERT_TRUE(daemon.wait_ready(std::chrono::seconds(5)));

  Client first;
  ASSERT_TRUE(first.connect(socket_path).ok);
  GraphLoadRequest request;
  request.session = GraphSessionId{"ipc_graph"};
  request.root_dir = (temp.path() / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (temp.path() / "cache").string();
  const auto loaded = first.load_graph(request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;
  EXPECT_EQ(loaded.value.session_name, "ipc_graph");
  EXPECT_EQ(loaded.value.session_id.value.size(), 32U);
  EXPECT_NE(loaded.value.session_id.value, loaded.value.session_name);

  const auto listed = first.list_graphs();
  ASSERT_TRUE(listed.status.ok);
  ASSERT_EQ(listed.value.size(), 1U);
  EXPECT_EQ(listed.value[0].session_id.value, loaded.value.session_id.value);
  const auto graph = first.inspect_graph(loaded.value.session_id);
  ASSERT_TRUE(graph.status.ok) << graph.status.message;
  ASSERT_EQ(graph.value.nodes.size(), 1U);
  EXPECT_EQ(graph.value.session.value, loaded.value.session_id.value);
  EXPECT_EQ(graph.value.nodes[0].id.value, 1);
  EXPECT_EQ(graph.value.nodes[0].name, "ipc_source");
  EXPECT_FALSE(graph.value.nodes[0].debug.has_value());
  EXPECT_FALSE(graph.value.nodes[0].space.has_value());

  const auto node = first.inspect_node(loaded.value.session_id, NodeId{1});
  ASSERT_TRUE(node.status.ok);
  EXPECT_EQ(node.value.name, "ipc_source");
  const auto endings = first.inspect_dependency_tree(loaded.value.session_id,
                                                     std::nullopt, false);
  ASSERT_TRUE(endings.status.ok);
  EXPECT_EQ(endings.value.scope, HostDependencyTreeScope::EndingNodes);
  ASSERT_EQ(endings.value.entries.size(), 1U);
  const auto start =
      first.inspect_dependency_tree(loaded.value.session_id, NodeId{1}, true);
  ASSERT_TRUE(start.status.ok);
  EXPECT_EQ(start.value.scope, HostDependencyTreeScope::StartNode);
  EXPECT_EQ(start.value.start_node->value, 1);

  first.disconnect();
  Client second;
  ASSERT_TRUE(second.connect(socket_path).ok);
  const auto after_disconnect = second.list_graphs();
  ASSERT_TRUE(after_disconnect.status.ok);
  ASSERT_EQ(after_disconnect.value.size(), 1U);
  EXPECT_TRUE(second.inspect_graph(loaded.value.session_id).status.ok);

  Client third;
  ASSERT_TRUE(third.connect(socket_path).ok);
  std::atomic<bool> second_ping{false};
  std::atomic<bool> third_ping{false};
  std::thread second_thread([&] { second_ping = second.ping().status.ok; });
  std::thread third_thread([&] { third_ping = third.ping().status.ok; });
  second_thread.join();
  third_thread.join();
  EXPECT_TRUE(second_ping.load());
  EXPECT_TRUE(third_ping.load());

  std::vector<std::unique_ptr<Client>> bounded_clients;
  for (int index = 0; index < 30; ++index) {
    auto bounded = std::make_unique<Client>();
    ASSERT_TRUE(bounded->connect(socket_path).ok);
    ASSERT_TRUE(bounded->ping().status.ok);
    bounded_clients.push_back(std::move(bounded));
  }
  Client excess;
  ASSERT_TRUE(excess.connect(socket_path).ok);
  EXPECT_FALSE(excess.ping().status.ok);
  EXPECT_TRUE(second.ping().status.ok);
  for (auto& bounded : bounded_clients) {
    bounded->disconnect();
  }
  ASSERT_TRUE(
      wait_for_connection_capacity(socket_path, std::chrono::seconds(2)));

  std::string malformed_error;
  internal::UniqueFd missing_id =
      internal::connect_unix_socket(socket_path, &malformed_error);
  ASSERT_TRUE(missing_id) << malformed_error;
  const std::string missing_id_request =
      R"({"protocol_version":2,"method":"daemon.ping","params":{}})";
  ASSERT_TRUE(internal::write_frame(missing_id.get(), missing_id_request).ok);
  const internal::FrameReadResult missing_id_response =
      internal::read_frame(missing_id.get());
  ASSERT_EQ(missing_id_response.state, internal::FrameReadState::Complete);
  EXPECT_NE(missing_id_response.payload.find("\"id\":null"), std::string::npos);
  EXPECT_NE(missing_id_response.payload.find("\"invalid_request\""),
            std::string::npos);
  missing_id.reset();

  std::string raw_error;
  internal::UniqueFd partial =
      internal::connect_unix_socket(socket_path, &raw_error);
  ASSERT_TRUE(partial) << raw_error;
  const unsigned char partial_header[] = {0, 0};
  ASSERT_EQ(::send(partial.get(), partial_header, sizeof(partial_header), 0),
            static_cast<ssize_t>(sizeof(partial_header)));
  EXPECT_TRUE(third.ping().status.ok);
  partial.reset();
  EXPECT_TRUE(second.ping().status.ok);

  internal::UniqueFd partial_body =
      internal::connect_unix_socket(socket_path, &raw_error);
  ASSERT_TRUE(partial_body) << raw_error;
  const std::uint32_t partial_body_length = htonl(12U);
  ASSERT_EQ(::send(partial_body.get(), &partial_body_length,
                   sizeof(partial_body_length), 0),
            static_cast<ssize_t>(sizeof(partial_body_length)));
  const char body_prefix[] = "abc";
  ASSERT_EQ(::send(partial_body.get(), body_prefix, sizeof(body_prefix) - 1, 0),
            static_cast<ssize_t>(sizeof(body_prefix) - 1));
  partial_body.reset();
  EXPECT_TRUE(second.ping().status.ok);

  const IpcSessionId missing{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
  const auto missing_graph = second.inspect_graph(missing);
  EXPECT_FALSE(missing_graph.status.ok);
  EXPECT_EQ(missing_graph.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(missing_graph.status.code,
            static_cast<std::int32_t>(GraphErrc::NotFound));
  const std::filesystem::path missing_session_save_path =
      temp.path() / "missing_session_save.yaml";
  const auto missing_session_save =
      second.save_graph(missing, missing_session_save_path.string());
  EXPECT_FALSE(missing_session_save.status.ok);
  EXPECT_EQ(missing_session_save.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(missing_session_save.status.code,
            static_cast<std::int32_t>(GraphErrc::NotFound));
  EXPECT_FALSE(std::filesystem::exists(missing_session_save_path));
  const auto missing_node =
      second.inspect_node(loaded.value.session_id, NodeId{999});
  EXPECT_FALSE(missing_node.status.ok);
  EXPECT_EQ(missing_node.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(missing_node.status.code,
            static_cast<std::int32_t>(GraphErrc::NotFound));

  const auto missing_node_yaml = second.set_node_yaml(
      loaded.value.session_id, NodeId{999},
      "id: 999\nname: missing\ntype: ipc_fixture\nsubtype: source\n");
  EXPECT_FALSE(missing_node_yaml.status.ok);
  EXPECT_EQ(missing_node_yaml.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(missing_node_yaml.status.code,
            static_cast<std::int32_t>(GraphErrc::NotFound));

  const PixelRect roi{0, 0, 1, 1};
  const auto missing_roi_target =
      second.project_roi(loaded.value.session_id, NodeId{1}, roi, NodeId{999});
  EXPECT_FALSE(missing_roi_target.status.ok);
  EXPECT_EQ(missing_roi_target.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(missing_roi_target.status.code,
            static_cast<std::int32_t>(GraphErrc::NotFound));
  const auto missing_roi_source = second.project_roi_backward(
      loaded.value.session_id, NodeId{1}, roi, NodeId{999});
  EXPECT_FALSE(missing_roi_source.status.ok);
  EXPECT_EQ(missing_roi_source.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(missing_roi_source.status.code,
            static_cast<std::int32_t>(GraphErrc::NotFound));

  const auto invalid_node_yaml =
      second.set_node_yaml(loaded.value.session_id, NodeId{1}, "[");
  EXPECT_FALSE(invalid_node_yaml.status.ok);
  EXPECT_EQ(invalid_node_yaml.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(invalid_node_yaml.status.code,
            static_cast<std::int32_t>(GraphErrc::InvalidYaml));
  const auto invalid_roi = second.project_roi(
      loaded.value.session_id, NodeId{1}, PixelRect{0, 0, 0, 1}, NodeId{1});
  EXPECT_FALSE(invalid_roi.status.ok);
  EXPECT_EQ(invalid_roi.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(invalid_roi.status.code,
            static_cast<std::int32_t>(GraphErrc::InvalidParameter));

  const auto save_io = second.save_graph(
      loaded.value.session_id,
      (temp.path() / "missing_save_parent" / "graph.yaml").string());
  EXPECT_FALSE(save_io.status.ok);
  EXPECT_EQ(save_io.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(save_io.status.code, static_cast<std::int32_t>(GraphErrc::Io));

  const auto graph_after_failed_save =
      second.inspect_graph(loaded.value.session_id);
  ASSERT_TRUE(graph_after_failed_save.status.ok)
      << graph_after_failed_save.status.message;
  ASSERT_EQ(graph_after_failed_save.value.nodes.size(), 1U);
  EXPECT_EQ(graph_after_failed_save.value.nodes[0].name, "ipc_source");
  const auto node_after_failed_save =
      second.inspect_node(loaded.value.session_id, NodeId{1});
  ASSERT_TRUE(node_after_failed_save.status.ok)
      << node_after_failed_save.status.message;
  EXPECT_EQ(node_after_failed_save.value.name, "ipc_source");

  const std::filesystem::path retry_save_path = temp.path() / "retry_save.yaml";
  const auto retry_save =
      second.save_graph(loaded.value.session_id, retry_save_path.string());
  ASSERT_TRUE(retry_save.status.ok) << retry_save.status.message;
  EXPECT_TRUE(std::filesystem::is_regular_file(retry_save_path));
  EXPECT_GT(std::filesystem::file_size(retry_save_path), 0U);

  ASSERT_TRUE(second.close_graph(loaded.value.session_id).status.ok);
  const auto empty = second.list_graphs();
  ASSERT_TRUE(empty.status.ok);
  EXPECT_TRUE(empty.value.empty());

  const auto reloaded = second.load_graph(request);
  ASSERT_TRUE(reloaded.status.ok) << reloaded.status.message;
  second.disconnect();
  daemon.stop();
  EXPECT_TRUE(daemon.exited_successfully());
  EXPECT_FALSE(std::filesystem::exists(socket_path));
}

TEST(IpcDaemonOperationPlugins,
     RealFixtureIsProcessGlobalAcrossShortLivedClientsUntilExplicitUnload) {
  ScopedDaemonDirectory temp("ps-plugin-daemon", true);
  const std::string socket_path = (temp.path() / "plugin.sock").string();
  const std::filesystem::path plugin_dir = lifecycle_operation_plugin_dir();
  ASSERT_TRUE(std::filesystem::exists(plugin_dir))
      << "lifecycle operation plugin directory was not built: " << plugin_dir;

  DaemonProcess daemon;
  daemon.start(socket_path);
  ASSERT_TRUE(daemon.wait_ready(std::chrono::seconds(5)));

  const internal::Json baseline_unload =
      raw_daemon_call(socket_path, "plugins.unload_all",
                      internal::Json::object(), "plugin-baseline-unload");
  ASSERT_TRUE(baseline_unload.contains("result")) << baseline_unload.dump();
  EXPECT_GE(baseline_unload["result"]["unloaded"].get<int>(), 0);

  for (const char* id : {"plugin-seed-first", "plugin-seed-second"}) {
    const internal::Json seeded =
        raw_daemon_call(socket_path, "plugins.seed_builtins",
                        internal::Json{{"future", true}}, id);
    ASSERT_TRUE(seeded.contains("result")) << seeded.dump();
    EXPECT_EQ(seeded["result"], internal::Json::object());
  }

  const internal::Json loaded = raw_daemon_call(
      socket_path, "plugins.load_report",
      internal::Json{
          {"directories", internal::Json::array({plugin_dir.string()})},
          {"session_id", "not-a-plugin-session"}},
      "plugin-real-load");
  ASSERT_TRUE(loaded.contains("result")) << loaded.dump();
  EXPECT_EQ(loaded["result"]["attempted"], 1);
  EXPECT_EQ(loaded["result"]["loaded"], 1);
  EXPECT_TRUE(loaded["result"]["errors"].empty());
  EXPECT_NE(std::find(loaded["result"]["new_op_keys"].begin(),
                      loaded["result"]["new_op_keys"].end(),
                      internal::Json("plugin_lifecycle:op")),
            loaded["result"]["new_op_keys"].end());

  std::vector<internal::Json> source_rows;
  std::optional<std::string> cursor;
  std::size_t offset = 0;
  constexpr std::size_t kMaxSourcePages = 128;
  std::size_t source_page_count = 0;
  const auto source_page_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    ASSERT_LT(source_page_count, kMaxSourcePages)
        << "plugin source pagination exceeded its page budget";
    const std::chrono::milliseconds rpc_timeout =
        timeout_before_deadline(source_page_deadline, std::chrono::seconds(1));
    ASSERT_NE(rpc_timeout, std::chrono::milliseconds::zero())
        << "plugin source pagination exceeded its aggregate deadline";
    ++source_page_count;

    const std::size_t requested_offset = offset;
    internal::Json params{{"limit", 1}};
    if (cursor) {
      params["cursor"] = *cursor;
      params["offset"] = requested_offset;
    }
    const internal::Json page = raw_daemon_call(
        socket_path, "plugins.ops_sources", std::move(params),
        "plugin-source-page-" + std::to_string(requested_offset), rpc_timeout);
    ASSERT_TRUE(page.contains("result")) << page.dump();
    const internal::Json& result = page["result"];
    ASSERT_TRUE(result.is_object()) << page.dump();
    ASSERT_TRUE(result.contains("offset"));
    ASSERT_TRUE(result["offset"].is_number_unsigned());
    const std::size_t server_offset = result["offset"].get<std::size_t>();
    ASSERT_EQ(server_offset, requested_offset);
    ASSERT_TRUE(result.contains("sources"));
    ASSERT_TRUE(result["sources"].is_array());
    ASSERT_LE(result["sources"].size(), 1U);
    ASSERT_TRUE(result.contains("has_more"));
    ASSERT_TRUE(result["has_more"].is_boolean());
    ASSERT_TRUE(result.contains("cursor"));
    ASSERT_TRUE(result["cursor"].is_null() || result["cursor"].is_string());
    const bool has_more = result["has_more"].get<bool>();
    ASSERT_EQ(has_more, result["cursor"].is_string());

    for (const internal::Json& row : result["sources"]) {
      source_rows.push_back(row);
    }
    ASSERT_LE(result["sources"].size(),
              std::numeric_limits<std::size_t>::max() - server_offset);
    const std::size_t next_offset = server_offset + result["sources"].size();
    if (has_more) {
      ASSERT_FALSE(result["sources"].empty());
      ASSERT_GT(next_offset, server_offset);
      cursor = result["cursor"].get<std::string>();
    } else {
      cursor.reset();
    }
    offset = next_offset;
  } while (cursor);

  ASSERT_FALSE(source_rows.empty());
  EXPECT_TRUE(std::is_sorted(
      source_rows.begin(), source_rows.end(),
      [](const internal::Json& left, const internal::Json& right) {
        return left["key"].get<std::string>() < right["key"].get<std::string>();
      }));
  const auto loaded_source = std::find_if(
      source_rows.begin(), source_rows.end(), [](const internal::Json& row) {
        return row.value("key", "") == "plugin_lifecycle:op";
      });
  ASSERT_NE(loaded_source, source_rows.end());
  const std::filesystem::path source_path =
      (*loaded_source)["source"].get<std::string>();
  EXPECT_TRUE(source_path.is_absolute());
  EXPECT_EQ(source_path.parent_path(), std::filesystem::absolute(plugin_dir));

  const internal::Json combined_keys = raw_daemon_call(
      socket_path, "plugins.ops_combined_keys", internal::Json{{"limit", 4096}},
      "plugin-combined-keys-other-client");
  ASSERT_TRUE(combined_keys.contains("result")) << combined_keys.dump();
  EXPECT_TRUE(std::is_sorted(combined_keys["result"]["keys"].begin(),
                             combined_keys["result"]["keys"].end()));
  EXPECT_NE(std::find(combined_keys["result"]["keys"].begin(),
                      combined_keys["result"]["keys"].end(),
                      internal::Json("plugin_lifecycle:op")),
            combined_keys["result"]["keys"].end());

  const internal::Json combined_sources = raw_daemon_call(
      socket_path, "plugins.ops_combined_sources",
      internal::Json{{"limit", 4096}}, "plugin-combined-sources-other-client");
  ASSERT_TRUE(combined_sources.contains("result")) << combined_sources.dump();
  EXPECT_NE(std::find_if(combined_sources["result"]["sources"].begin(),
                         combined_sources["result"]["sources"].end(),
                         [](const internal::Json& row) {
                           return row.value("key", "") == "plugin_lifecycle:op";
                         }),
            combined_sources["result"]["sources"].end());

  const internal::Json repeated_load = raw_daemon_call(
      socket_path, "plugins.load_report",
      internal::Json{
          {"directories", internal::Json::array({plugin_dir.string()})}},
      "plugin-repeated-load-other-client");
  ASSERT_TRUE(repeated_load.contains("result")) << repeated_load.dump();
  EXPECT_EQ(repeated_load["result"]["attempted"], 0);
  EXPECT_EQ(repeated_load["result"]["loaded"], 0);
  EXPECT_TRUE(repeated_load["result"]["errors"].empty());

  const internal::Json unloaded =
      raw_daemon_call(socket_path, "plugins.unload_all",
                      internal::Json::object(), "plugin-explicit-unload");
  ASSERT_TRUE(unloaded.contains("result")) << unloaded.dump();
  EXPECT_GE(unloaded["result"]["unloaded"].get<int>(), 1);

  const internal::Json after_unload = raw_daemon_call(
      socket_path, "plugins.ops_sources", internal::Json{{"limit", 4096}},
      "plugin-sources-after-explicit-unload");
  ASSERT_TRUE(after_unload.contains("result")) << after_unload.dump();
  EXPECT_EQ(std::count_if(after_unload["result"]["sources"].begin(),
                          after_unload["result"]["sources"].end(),
                          [](const internal::Json& row) {
                            return row.value("key", "") ==
                                   "plugin_lifecycle:op";
                          }),
            0);

  daemon.stop();
  EXPECT_TRUE(daemon.exited_successfully());
  EXPECT_FALSE(std::filesystem::exists(socket_path));
}

/**
 * @brief Preserves execution defaults and one fixed pool across Graphs.
 *
 * @return Nothing; GoogleTest records protocol, Host, lifecycle, or status
 * propagation mismatches.
 * @throws Standard daemon, socket, filesystem, allocation, or synchronization
 * exceptions from the real product path.
 * @note The accepted HP and RT routes are deliberately distinct. A rejected
 * count-nine update must leave both future-Graph defaults unchanged, while
 * every loaded Graph reuses the Host-owned fixed ExecutionService.
 */
TEST(IpcDaemonExecution,
     RealDaemonPreservesDefaultsAndSharesFixedPoolAcrossGraphs) {
  constexpr unsigned int kServiceWorkers = 4U;
  constexpr std::size_t kInitialGraphCount = 4U;

  ScopedDaemonDirectory temp("ps-execution-budget", true);
  const std::string socket_path = (temp.path() / "budget.sock").string();
  DaemonProcess daemon;
  daemon.start(socket_path);
  ASSERT_TRUE(daemon.wait_ready(std::chrono::seconds(5)));

  internal::Json response =
      raw_daemon_call(socket_path, "execution.types",
                      internal::Json{{"limit", 4096}}, "execution-types");
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["types"],
            internal::Json::array({"cpu", "gpu_pipeline", "serial_debug"}));

  response =
      raw_daemon_call(socket_path, "execution.description",
                      internal::Json{{"type", "cpu"}}, "execution-description");
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["type"], "cpu");
  EXPECT_FALSE(response["result"]["description"].get<std::string>().empty());

  response = raw_daemon_call(socket_path, "execution.configure_defaults",
                             internal::Json{{"hp_type", "cpu"},
                                            {"rt_type", "serial_debug"},
                                            {"worker_count", kServiceWorkers}},
                             "execution-budget-accepted-defaults");
  ASSERT_TRUE(response.contains("result")) << response.dump();

  response = raw_daemon_call(
      socket_path, "execution.configure_defaults",
      internal::Json{{"hp_type", "serial_debug"},
                     {"rt_type", "cpu"},
                     {"worker_count", kExecutionWorkerRequestMax + 1U}},
      "execution-budget-rejected-defaults");
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["code"], internal::kInvalidParamsCode);
  EXPECT_EQ(response["error"]["name"], "invalid_params");

  const std::filesystem::path yaml_path = temp.path() / "budget.yaml";
  write_ipc_graph(yaml_path);
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  std::vector<GraphSessionSummary> loaded;
  loaded.reserve(kInitialGraphCount);
  for (std::size_t index = 0; index < kInitialGraphCount; ++index) {
    GraphLoadRequest request;
    request.session =
        GraphSessionId{"execution_budget_" + std::to_string(index)};
    request.root_dir =
        (temp.path() / ("sessions-" + std::to_string(index))).string();
    request.yaml_path = yaml_path.string();
    request.cache_root_dir =
        (temp.path() / ("cache-" + std::to_string(index))).string();
    IpcResult<GraphSessionSummary> result = client.load_graph(request);
    ASSERT_TRUE(result.status.ok)
        << "graph " << index << ": " << result.status.message;
    loaded.push_back(std::move(result.value));
  }

  ASSERT_FALSE(loaded.empty());
  const IpcResult<ExecutionInfoSnapshot> hp = client.execution_info(
      loaded.front().session_id, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(hp.status.ok) << hp.status.message;
  EXPECT_EQ(hp.value.execution_type, "cpu");
  const IpcResult<ExecutionInfoSnapshot> rt = client.execution_info(
      loaded.front().session_id, ComputeIntent::RealTimeUpdate);
  ASSERT_TRUE(rt.status.ok) << rt.status.message;
  EXPECT_EQ(rt.value.execution_type, "serial_debug");

  GraphLoadRequest additional;
  additional.session = GraphSessionId{"execution_budget_additional"};
  additional.root_dir = (temp.path() / "sessions-additional").string();
  additional.yaml_path = yaml_path.string();
  additional.cache_root_dir = (temp.path() / "cache-additional").string();
  const IpcResult<GraphSessionSummary> shared = client.load_graph(additional);
  ASSERT_TRUE(shared.status.ok) << shared.status.message;
  const IpcResult<ExecutionInfoSnapshot> shared_hp = client.execution_info(
      shared.value.session_id, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(shared_hp.status.ok) << shared_hp.status.message;
  EXPECT_EQ(shared_hp.value.execution_type, "cpu");

  for (const GraphSessionSummary& graph : loaded) {
    const VoidResult cleanup = client.close_graph(graph.session_id);
    EXPECT_TRUE(cleanup.status.ok) << cleanup.status.message;
  }
  const VoidResult shared_cleanup = client.close_graph(shared.value.session_id);
  EXPECT_TRUE(shared_cleanup.status.ok) << shared_cleanup.status.message;
  client.disconnect();
  daemon.stop();
  EXPECT_TRUE(daemon.exited_successfully());
}

/**
 * @brief Routes real policy discovery, mutation, and scan through the daemon.
 *
 * @return Nothing; GoogleTest records protocol, Host, loader, binding, or
 * lifecycle mismatches.
 * @throws Standard daemon, socket, filesystem, allocation, loader, or
 * synchronization exceptions from the real product path.
 * @note Every raw call uses a fresh client connection, proving policy registry
 * and binding state are process-owned rather than connection-owned. A second
 * daemon process exercises directory scan from a clean registry.
 */
TEST(IpcDaemonPolicy,
     RealFixtureRoutesDiscoveryConfigurationReplacementAndScan) {
  ScopedDaemonDirectory temp("ps-policy-daemon", true);
  const std::filesystem::path plugin_path = policy_fixture_plugin_path();
  ASSERT_TRUE(std::filesystem::is_regular_file(plugin_path))
      << "policy fixture was not built at " << plugin_path;

  const std::string load_socket = (temp.path() / "load.sock").string();
  DaemonProcess daemon;
  daemon.start(load_socket);
  ASSERT_TRUE(daemon.wait_ready(std::chrono::seconds(5)));

  internal::Json response = raw_daemon_call(
      load_socket, "policy.load",
      internal::Json{{"path", (temp.path() / "missing.dylib").string()}},
      "policy-missing-load");
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "graph");
  EXPECT_EQ(response["error"]["name"], "io");

  response = raw_daemon_call(
      load_socket, "policy.load",
      internal::Json{{"path", plugin_path.string()}, {"future", true}},
      "policy-real-load");
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"], internal::Json::object());

  response =
      raw_daemon_call(load_socket, "policy.types",
                      internal::Json{{"limit", 4096}}, "policy-real-types");
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_TRUE(response["result"]["types"].is_array());
  EXPECT_TRUE(std::is_sorted(response["result"]["types"].begin(),
                             response["result"]["types"].end()));
  EXPECT_NE(std::find(response["result"]["types"].begin(),
                      response["result"]["types"].end(),
                      internal::Json(kFixturePolicyType)),
            response["result"]["types"].end());

  response = raw_daemon_call(load_socket, "policy.description",
                             internal::Json{{"type", kFixturePolicyType}},
                             "policy-real-description");
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["type"], kFixturePolicyType);
  EXPECT_EQ(response["result"]["description"],
            "Deterministic policy test fixture.");

  const internal::Json missing_description =
      raw_daemon_call(load_socket, "policy.description",
                      internal::Json{{"type", "missing_policy_type"}},
                      "policy-missing-description");
  ASSERT_TRUE(missing_description.contains("error"))
      << missing_description.dump();
  EXPECT_EQ(missing_description["error"]["domain"], "graph");
  EXPECT_EQ(missing_description["error"]["name"], "not_found");

  response = raw_daemon_call(load_socket, "policy.loaded_plugins",
                             internal::Json{{"limit", 4096}},
                             "policy-real-loaded-plugins");
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_TRUE(response["result"]["plugins"].is_array());
  ASSERT_EQ(response["result"]["plugins"].size(), 1U);
  EXPECT_EQ(response["result"]["plugins"][0],
            std::filesystem::absolute(plugin_path).lexically_normal().string());

  response =
      raw_daemon_call(load_socket, "policy.configure_defaults",
                      internal::Json{{"interactive_type", kFixturePolicyType},
                                     {"throughput_type", kFixturePolicyType}},
                      "policy-real-defaults");
  ASSERT_TRUE(response.contains("result")) << response.dump();

  response = raw_daemon_call(load_socket, "policy.info",
                             internal::Json{{"policy_class", "interactive"}},
                             "policy-real-info");
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["policy_class"], "interactive");
  EXPECT_EQ(response["result"]["policy_type"], kFixturePolicyType);
  EXPECT_TRUE(response["result"]["fault"].is_null());
  const std::uint64_t fixture_generation =
      response["result"]["binding_generation"].get<std::uint64_t>();
  EXPECT_GT(fixture_generation, 0U);

  response = raw_daemon_call(load_socket, "policy.replace",
                             internal::Json{{"policy_class", "interactive"},
                                            {"type", "missing_policy_type"}},
                             "policy-real-invalid-replace");
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "graph");
  EXPECT_EQ(response["error"]["name"], "invalid_parameter");

  response = raw_daemon_call(
      load_socket, "policy.replace",
      internal::Json{{"policy_class", "interactive"}, {"type", "interactive"}},
      "policy-real-replace");
  ASSERT_TRUE(response.contains("result")) << response.dump();

  response = raw_daemon_call(load_socket, "policy.info",
                             internal::Json{{"policy_class", "interactive"}},
                             "policy-info-after-replace");
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["policy_type"], "interactive");
  EXPECT_GT(response["result"]["binding_generation"].get<std::uint64_t>(),
            fixture_generation);
  EXPECT_TRUE(response["result"]["fault"].is_null());

  response = raw_daemon_call(load_socket, "policy.types",
                             internal::Json{{"limit", 4096}},
                             "policy-types-after-client-close");
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_NE(std::find(response["result"]["types"].begin(),
                      response["result"]["types"].end(),
                      internal::Json(kFixturePolicyType)),
            response["result"]["types"].end());

  daemon.stop();
  EXPECT_TRUE(daemon.exited_successfully());

  const std::string scan_socket = (temp.path() / "scan.sock").string();
  daemon.start(scan_socket);
  ASSERT_TRUE(daemon.wait_ready(std::chrono::seconds(5)));
  response = raw_daemon_call(
      scan_socket, "policy.scan",
      internal::Json{
          {"directories",
           internal::Json::array({plugin_path.parent_path().string()})}},
      "policy-real-scan");
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_GE(response["result"]["loaded"].get<std::size_t>(), 1U);

  response = raw_daemon_call(scan_socket, "policy.types",
                             internal::Json{{"limit", 4096}},
                             "policy-types-after-scan");
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_NE(std::find(response["result"]["types"].begin(),
                      response["result"]["types"].end(),
                      internal::Json(kFixturePolicyType)),
            response["result"]["types"].end());

  daemon.stop();
  EXPECT_TRUE(daemon.exited_successfully());
}

TEST(IpcOutputFixtureDaemon,
     EmptyImageSucceedsWithoutArtifactOrDeliveryMetadata) {
  ScopedDaemonDirectory temp("ps-output-empty", true);
  const std::string socket_path = (temp.path() / "empty.sock").string();
  ManualProcessClock clock(temp.path() / "clock.bin");
  DaemonProcess daemon;
  daemon.start(socket_path, true, {}, -1, "empty", clock.path().string());
  ASSERT_TRUE(daemon.wait_ready(std::chrono::seconds(5)));
  const IpcResult<GraphSessionSummary> loaded = load_fixture_session(
      socket_path, temp.path() / "sessions", "empty_output");
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  const internal::Json submitted = raw_daemon_call(
      socket_path, "compute.submit",
      image_compute_submit_params(loaded.value.session_id), "empty-submit");
  ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
  ASSERT_EQ(submitted["result"].size(), 6U);
  EXPECT_TRUE(submitted["result"]["output"].is_null());
  const std::string compute_id =
      submitted["result"]["compute_id"].get<std::string>();
  ScopedFixtureJobRelease release_guard(socket_path, compute_id);
  const internal::Json terminal = wait_for_real_compute_terminal(
      socket_path, compute_id, std::chrono::seconds(3));
  ASSERT_TRUE(terminal.contains("result")) << terminal.dump();
  EXPECT_EQ(terminal["result"]["state"], "succeeded");
  EXPECT_TRUE(terminal["result"]["status"]["ok"].get<bool>());
  EXPECT_TRUE(terminal["result"]["output"].is_null());
  const internal::Json result = raw_daemon_call(
      socket_path, "compute.result", internal::Json{{"compute_id", compute_id}},
      "empty-result");
  ASSERT_TRUE(result.contains("result")) << result.dump();
  EXPECT_TRUE(result["result"]["output"].is_null());
  const internal::Json released = raw_daemon_call(
      socket_path, "compute.release",
      internal::Json{{"compute_id", compute_id}}, "empty-release");
  ASSERT_TRUE(released.contains("result")) << released.dump();
  release_guard.dismiss();

  daemon.stop();
  EXPECT_TRUE(daemon.exited_successfully());
  EXPECT_FALSE(std::filesystem::exists(socket_path));
  const std::filesystem::path output_base(socket_path + ".outputs");
  EXPECT_TRUE(std::filesystem::exists(output_base));
  EXPECT_TRUE(std::filesystem::is_empty(output_base));
}

TEST(IpcObservationFixtureDaemon,
     IndependentClientsShareEventDrainAndRetainTracePages) {
  ScopedDaemonDirectory temp("ps-observation-clients", true);
  const std::string socket_path = (temp.path() / "observe.sock").string();
  ManualProcessClock clock(temp.path() / "clock.bin");
  DaemonProcess daemon;
  daemon.start(socket_path, true, {}, -1, "empty", clock.path().string());
  ASSERT_TRUE(daemon.wait_ready(std::chrono::seconds(5)));
  const IpcResult<GraphSessionSummary> loaded = load_fixture_session(
      socket_path, temp.path() / "sessions", "observation_clients");
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  RawDaemonCallTask first_drain(
      socket_path, "events.drain",
      internal::Json{{"session_id", loaded.value.session_id.value},
                     {"limit", 1}},
      "event-client-left", std::chrono::seconds(3));
  RawDaemonCallTask second_drain(
      socket_path, "events.drain",
      internal::Json{{"session_id", loaded.value.session_id.value},
                     {"limit", 1}},
      "event-client-right", std::chrono::seconds(3));
  const internal::Json first_events = first_drain.join();
  const internal::Json second_events = second_drain.join();
  ASSERT_TRUE(first_events.contains("result")) << first_events.dump();
  ASSERT_TRUE(second_events.contains("result")) << second_events.dump();
  ASSERT_EQ(first_events["result"]["events"].size(), 1u);
  ASSERT_EQ(second_events["result"]["events"].size(), 1u);
  std::array<std::uint64_t, 2> event_sequences = {
      first_events["result"]["events"][0]["sequence"].get<std::uint64_t>(),
      second_events["result"]["events"][0]["sequence"].get<std::uint64_t>()};
  std::sort(event_sequences.begin(), event_sequences.end());
  EXPECT_EQ(event_sequences, (std::array<std::uint64_t, 2>{1, 2}));
  EXPECT_EQ(first_events["result"]["dropped_count"].get<std::uint64_t>() +
                second_events["result"]["dropped_count"].get<std::uint64_t>(),
            1u);
  EXPECT_NE(first_events["result"]["has_more"],
            second_events["result"]["has_more"]);

  const internal::Json trace_params{
      {"session_id", loaded.value.session_id.value},
      {"after_sequence", 0},
      {"limit", 2}};
  RawDaemonCallTask first_trace(socket_path, "execution.trace", trace_params,
                                "trace-client-left", std::chrono::seconds(3));
  RawDaemonCallTask second_trace(socket_path, "execution.trace", trace_params,
                                 "trace-client-right", std::chrono::seconds(3));
  const internal::Json left_trace = first_trace.join();
  const internal::Json right_trace = second_trace.join();
  ASSERT_TRUE(left_trace.contains("result")) << left_trace.dump();
  ASSERT_TRUE(right_trace.contains("result")) << right_trace.dump();
  EXPECT_EQ(left_trace["result"], right_trace["result"]);
  ASSERT_EQ(left_trace["result"]["events"].size(), 2u);
  EXPECT_EQ(left_trace["result"]["events"][0]["sequence"], 1);
  EXPECT_EQ(left_trace["result"]["events"][1]["sequence"], 2);
  EXPECT_EQ(left_trace["result"]["next_sequence"], 2);
  EXPECT_FALSE(left_trace["result"]["has_more"].get<bool>());

  const internal::Json closed = raw_daemon_call(
      socket_path, "graph.close",
      internal::Json{{"session_id", loaded.value.session_id.value}},
      "observation-close");
  ASSERT_TRUE(closed.contains("result")) << closed.dump();
  daemon.stop();
  EXPECT_TRUE(daemon.exited_successfully());
  EXPECT_FALSE(std::filesystem::exists(socket_path));
}

TEST(IpcOutputFixtureDaemon,
     NonemptyImageReturnsExactProtectedMetadataAndStableLease) {
  ScopedDaemonDirectory temp("ps-output-value", true);
  const std::string socket_path = (temp.path() / "value.sock").string();
  ManualProcessClock clock(temp.path() / "clock.bin");
  DaemonProcess daemon;
  daemon.start(socket_path, true, {}, -1, "nonempty", clock.path().string());
  ASSERT_TRUE(daemon.wait_ready(std::chrono::seconds(5)));
  const IpcResult<GraphSessionSummary> loaded = load_fixture_session(
      socket_path, temp.path() / "sessions", "nonempty_output");
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  const internal::Json submitted = raw_daemon_call(
      socket_path, "compute.submit",
      image_compute_submit_params(loaded.value.session_id), "image-submit");
  ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
  EXPECT_TRUE(submitted["result"]["output"].is_null());
  const std::string compute_id =
      submitted["result"]["compute_id"].get<std::string>();
  ScopedFixtureJobRelease release_guard(socket_path, compute_id);
  const internal::Json terminal = wait_for_real_compute_terminal(
      socket_path, compute_id, std::chrono::seconds(3));
  ASSERT_TRUE(terminal.contains("result")) << terminal.dump();
  EXPECT_EQ(terminal["result"]["state"], "succeeded");
  EXPECT_TRUE(terminal["result"]["output"].is_null());

  const internal::Json result = raw_daemon_call(
      socket_path, "compute.result", internal::Json{{"compute_id", compute_id}},
      "image-result");
  ASSERT_TRUE(result.contains("result")) << result.dump();
  ASSERT_EQ(result["result"].size(), 6U);
  const internal::Json output = result["result"]["output"];
  if (output.is_object() &&
      output.value("delivery_id", internal::Json()).is_string()) {
    release_guard.protect_delivery(output["delivery_id"].get<std::string>());
  }
  ASSERT_TRUE(output.is_object()) << result.dump();
  ASSERT_EQ(output.size(), 12U);
  EXPECT_TRUE(
      internal::valid_opaque_id(output["output_id"].get<std::string>()));
  EXPECT_TRUE(
      internal::valid_opaque_id(output["delivery_id"].get<std::string>()));
  EXPECT_FALSE(output.contains("output_reference"));
  EXPECT_EQ(output["width"], 2);
  EXPECT_EQ(output["height"], 2);
  EXPECT_EQ(output["channels"], 1);
  EXPECT_EQ(output["data_type"], "uint8");
  EXPECT_EQ(output["device"], "cpu");
  EXPECT_EQ(output["row_step"], 2);
  EXPECT_EQ(output["byte_size"], 4);
  const std::string artifact_path = output["path"].get<std::string>();
  EXPECT_TRUE(std::filesystem::path(artifact_path).is_absolute());
  EXPECT_EQ(read_artifact_bytes(artifact_path),
            (std::vector<std::uint8_t>{1, 2, 3, 4}));
  struct stat artifact{};
  ASSERT_EQ(::lstat(artifact_path.c_str(), &artifact), 0);
  EXPECT_TRUE(S_ISREG(artifact.st_mode));
  EXPECT_EQ(artifact.st_mode & 07777, 0600);
  EXPECT_EQ(artifact.st_uid, ::geteuid());
  EXPECT_EQ(artifact.st_nlink, 1);
  EXPECT_EQ(static_cast<std::uint64_t>(artifact.st_dev),
            output["filesystem_device"].get<std::uint64_t>());
  EXPECT_EQ(static_cast<std::uint64_t>(artifact.st_ino),
            output["inode"].get<std::uint64_t>());

  const internal::Json repeated = raw_daemon_call(
      socket_path, "compute.result", internal::Json{{"compute_id", compute_id}},
      "image-result-repeated");
  ASSERT_TRUE(repeated.contains("result")) << repeated.dump();
  EXPECT_EQ(repeated["result"]["output"], output);
  const internal::Json released =
      raw_daemon_call(socket_path, "compute.release",
                      internal::Json{{"compute_id", compute_id},
                                     {"delivery_id", output["delivery_id"]}},
                      "image-release");
  ASSERT_TRUE(released.contains("result")) << released.dump();
  release_guard.dismiss();
  EXPECT_FALSE(std::filesystem::exists(artifact_path));

  daemon.stop();
  EXPECT_TRUE(daemon.exited_successfully());
  EXPECT_FALSE(std::filesystem::exists(socket_path));
  EXPECT_TRUE(std::filesystem::is_empty(socket_path + ".outputs"));
}

TEST(IpcOutputFixtureDaemon,
     ManualClockRefreshesOldLeaseAndReactivatesStableIdentity) {
  ScopedDaemonDirectory temp("ps-output-clock", true);
  const std::string socket_path = (temp.path() / "clock.sock").string();
  ManualProcessClock clock(temp.path() / "clock.bin");
  DaemonProcess daemon;
  daemon.start(socket_path, true, {}, -1, "nonempty", clock.path().string());
  ASSERT_TRUE(daemon.wait_ready(std::chrono::seconds(5)));
  const IpcResult<GraphSessionSummary> loaded = load_fixture_session(
      socket_path, temp.path() / "sessions", "manual_clock_output");
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  FixtureImageDelivery refreshed = deliver_fixture_image(
      socket_path, loaded.value.session_id, "near-expiry");
  ScopedFixtureJobRelease refreshed_guard(socket_path, refreshed.compute_id);
  refreshed_guard.protect_delivery(
      refreshed.output["delivery_id"].get<std::string>());
  const std::string refreshed_path =
      refreshed.output["path"].get<std::string>();
  clock.advance(std::chrono::milliseconds(900));
  const internal::Json repeated =
      raw_daemon_call(socket_path, "compute.result",
                      internal::Json{{"compute_id", refreshed.compute_id}},
                      "near-expiry-refresh");
  ASSERT_TRUE(repeated.contains("result")) << repeated.dump();
  ASSERT_TRUE(repeated["result"]["output"].is_object()) << repeated.dump();
  EXPECT_EQ(repeated["result"]["output"]["delivery_id"],
            refreshed.output["delivery_id"]);
  EXPECT_EQ(repeated["result"]["output"]["output_id"],
            refreshed.output["output_id"]);

  const internal::Json owner_removed =
      raw_daemon_call(socket_path, "compute.release",
                      internal::Json{{"compute_id", refreshed.compute_id}},
                      "near-expiry-remove-owner");
  ASSERT_TRUE(owner_removed.contains("result")) << owner_removed.dump();
  clock.advance(std::chrono::milliseconds(200));
  trigger_fixture_cleanup(socket_path, "near-expiry-cleanup");
  EXPECT_TRUE(std::filesystem::exists(refreshed_path));
  EXPECT_EQ(read_artifact_bytes(refreshed_path),
            (std::vector<std::uint8_t>{1, 2, 3, 4}));
  const internal::Json orphan_released = raw_daemon_call(
      socket_path, "compute.release",
      internal::Json{{"compute_id", refreshed.compute_id},
                     {"delivery_id", refreshed.output["delivery_id"]}},
      "near-expiry-release-orphan");
  ASSERT_TRUE(orphan_released.contains("result")) << orphan_released.dump();
  refreshed_guard.dismiss();
  EXPECT_FALSE(std::filesystem::exists(refreshed_path));

  FixtureImageDelivery reactivated =
      deliver_fixture_image(socket_path, loaded.value.session_id, "reactivate");
  ScopedFixtureJobRelease reactivated_guard(socket_path,
                                            reactivated.compute_id);
  reactivated_guard.protect_delivery(
      reactivated.output["delivery_id"].get<std::string>());
  const std::string stable_delivery =
      reactivated.output["delivery_id"].get<std::string>();
  clock.advance(std::chrono::milliseconds(1100));
  const internal::Json after_expiry =
      raw_daemon_call(socket_path, "compute.result",
                      internal::Json{{"compute_id", reactivated.compute_id}},
                      "reactivate-after-expiry");
  ASSERT_TRUE(after_expiry.contains("result")) << after_expiry.dump();
  ASSERT_TRUE(after_expiry["result"]["output"].is_object())
      << after_expiry.dump();
  EXPECT_EQ(after_expiry["result"]["output"]["delivery_id"], stable_delivery);
  EXPECT_EQ(after_expiry["result"]["output"]["output_id"],
            reactivated.output["output_id"]);
  const internal::Json reactivated_released =
      raw_daemon_call(socket_path, "compute.release",
                      internal::Json{{"compute_id", reactivated.compute_id},
                                     {"delivery_id", stable_delivery}},
                      "reactivate-release");
  ASSERT_TRUE(reactivated_released.contains("result"))
      << reactivated_released.dump();
  reactivated_guard.dismiss();

  daemon.stop();
  EXPECT_TRUE(daemon.exited_successfully());
  EXPECT_TRUE(std::filesystem::is_empty(socket_path + ".outputs"));
}

TEST(IpcOutputFixtureDaemon,
     OpenDescriptorsSurviveReleaseEvictionAndRegistryExpiry) {
  ScopedDaemonDirectory temp("ps-output-retention", true);
  const std::string socket_path = (temp.path() / "retention.sock").string();
  ManualProcessClock clock(temp.path() / "clock.bin");
  DaemonProcess daemon;
  daemon.start(socket_path, true, {}, -1, "nonempty", clock.path().string());
  ASSERT_TRUE(daemon.wait_ready(std::chrono::seconds(5)));
  const IpcResult<GraphSessionSummary> loaded = load_fixture_session(
      socket_path, temp.path() / "sessions", "retention_output");
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;
  const std::vector<std::uint8_t> expected{1, 2, 3, 4};

  FixtureImageDelivery normal =
      deliver_fixture_image(socket_path, loaded.value.session_id, "fd-normal");
  internal::UniqueFd normal_fd =
      open_fixture_artifact(normal.output["path"].get<std::string>());
  const internal::Json normal_released = raw_daemon_call(
      socket_path, "compute.release",
      internal::Json{{"compute_id", normal.compute_id},
                     {"delivery_id", normal.output["delivery_id"]}},
      "fd-normal-release");
  ASSERT_TRUE(normal_released.contains("result")) << normal_released.dump();
  EXPECT_FALSE(
      std::filesystem::exists(normal.output["path"].get<std::string>()));
  EXPECT_EQ(read_open_fixture_artifact(normal_fd.get()), expected);

  FixtureImageDelivery evicted = deliver_fixture_image(
      socket_path, loaded.value.session_id, "fd-eviction");
  ScopedFixtureJobRelease eviction_guard(socket_path, evicted.compute_id);
  eviction_guard.protect_delivery(
      evicted.output["delivery_id"].get<std::string>());
  internal::UniqueFd eviction_fd =
      open_fixture_artifact(evicted.output["path"].get<std::string>());
  clock.advance(std::chrono::milliseconds(1100));
  trigger_fixture_cleanup(socket_path, "expire-eviction-lease");
  for (std::size_t index = 0; index < 3; ++index) {
    const internal::Json submitted =
        raw_daemon_call(socket_path, "compute.submit",
                        minimal_compute_submit_params(loaded.value.session_id),
                        "capacity-submit-" + std::to_string(index));
    ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
    const std::string compute_id =
        submitted["result"]["compute_id"].get<std::string>();
    const internal::Json terminal = wait_for_real_compute_terminal(
        socket_path, compute_id, std::chrono::seconds(3));
    ASSERT_TRUE(terminal.contains("result")) << terminal.dump();
  }
  const internal::Json evicted_status =
      raw_daemon_call(socket_path, "compute.status",
                      internal::Json{{"compute_id", evicted.compute_id}},
                      "capacity-evicted-status");
  ASSERT_TRUE(evicted_status.contains("error")) << evicted_status.dump();
  EXPECT_EQ(evicted_status["error"]["name"], "job_not_found");
  EXPECT_FALSE(
      std::filesystem::exists(evicted.output["path"].get<std::string>()));
  EXPECT_EQ(read_open_fixture_artifact(eviction_fd.get()), expected);
  eviction_guard.dismiss();

  FixtureImageDelivery expired =
      deliver_fixture_image(socket_path, loaded.value.session_id, "fd-expiry");
  ScopedFixtureJobRelease expiry_guard(socket_path, expired.compute_id);
  expiry_guard.protect_delivery(
      expired.output["delivery_id"].get<std::string>());
  internal::UniqueFd expiry_fd =
      open_fixture_artifact(expired.output["path"].get<std::string>());
  clock.advance(std::chrono::milliseconds(1100));
  trigger_fixture_cleanup(socket_path, "expire-ttl-lease");
  clock.advance(std::chrono::seconds(9));
  const internal::Json expired_status =
      raw_daemon_call(socket_path, "compute.status",
                      internal::Json{{"compute_id", expired.compute_id}},
                      "registry-ttl-status");
  ASSERT_TRUE(expired_status.contains("error")) << expired_status.dump();
  EXPECT_EQ(expired_status["error"]["name"], "job_not_found");
  EXPECT_FALSE(
      std::filesystem::exists(expired.output["path"].get<std::string>()));
  EXPECT_EQ(read_open_fixture_artifact(expiry_fd.get()), expected);
  expiry_guard.dismiss();

  daemon.stop();
  EXPECT_TRUE(daemon.exited_successfully());
  EXPECT_TRUE(std::filesystem::is_empty(socket_path + ".outputs"));
}

TEST(IpcOutputFixtureDaemon,
     ActiveLeaseBlocksShutdownAfterListenerPathIsRemoved) {
  ScopedDaemonDirectory temp("ps-output-shutdown-lease", true);
  const std::string socket_path = (temp.path() / "shutdown.sock").string();
  ManualProcessClock clock(temp.path() / "clock.bin");
  DaemonProcess daemon;
  daemon.start(socket_path, true, {}, -1, "nonempty", clock.path().string());
  ASSERT_TRUE(daemon.wait_ready(std::chrono::seconds(5)));
  const IpcResult<GraphSessionSummary> loaded = load_fixture_session(
      socket_path, temp.path() / "sessions", "shutdown_lease_output");
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;
  FixtureImageDelivery delivery = deliver_fixture_image(
      socket_path, loaded.value.session_id, "shutdown-lease");
  const std::string path = delivery.output["path"].get<std::string>();
  internal::UniqueFd descriptor = open_fixture_artifact(path);

  clock.advance(std::chrono::milliseconds(900));
  ASSERT_TRUE(daemon.request_stop());
  ASSERT_TRUE(wait_for_listener_shutdown(socket_path, std::chrono::seconds(1)));
  ASSERT_FALSE(daemon.poll_exit());
  EXPECT_FALSE(std::filesystem::exists(socket_path));
  internal::UniqueFd late_connector(::socket(AF_UNIX, SOCK_STREAM, 0));
  ASSERT_TRUE(late_connector);
  sockaddr_un late_address{};
  late_address.sun_family = AF_UNIX;
  std::memcpy(late_address.sun_path, socket_path.c_str(),
              socket_path.size() + 1);
  const socklen_t late_address_length = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + socket_path.size() + 1);
  errno = 0;
  const int late_connect = ::connect(late_connector.get(),
                                     reinterpret_cast<sockaddr*>(&late_address),
                                     late_address_length);
  const int late_connect_error = errno;
  EXPECT_EQ(late_connect, -1);
  EXPECT_EQ(late_connect_error, ENOENT);
  EXPECT_FALSE(daemon.wait_for_exit(std::chrono::milliseconds(40)));
  clock.advance(std::chrono::milliseconds(200));
  ASSERT_TRUE(daemon.wait_for_exit(std::chrono::seconds(2)));
  EXPECT_TRUE(daemon.exited_successfully());
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_EQ(read_open_fixture_artifact(descriptor.get()),
            (std::vector<std::uint8_t>{1, 2, 3, 4}));
  EXPECT_TRUE(std::filesystem::is_empty(socket_path + ".outputs"));
}

TEST(IpcOutputFixtureDaemon,
     InvalidImageFilesystemPublicationAndArtifactCountQuotaStayNested) {
  ScopedDaemonDirectory temp("ps-output-fail", true);
  ManualProcessClock clock(temp.path() / "clock.bin");
  const std::string invalid_socket = (temp.path() / "invalid.sock").string();
  DaemonProcess invalid_daemon;
  invalid_daemon.start(invalid_socket, true, {}, -1, "invalid",
                       clock.path().string());
  ASSERT_TRUE(invalid_daemon.wait_ready(std::chrono::seconds(5)));
  const IpcResult<GraphSessionSummary> invalid_session = load_fixture_session(
      invalid_socket, temp.path() / "invalid-sessions", "invalid_output");
  ASSERT_TRUE(invalid_session.status.ok) << invalid_session.status.message;
  internal::Json submitted = raw_daemon_call(
      invalid_socket, "compute.submit",
      image_compute_submit_params(invalid_session.value.session_id),
      "invalid-image-submit");
  ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
  const std::string invalid_id =
      submitted["result"]["compute_id"].get<std::string>();
  internal::Json terminal = wait_for_real_compute_terminal(
      invalid_socket, invalid_id, std::chrono::seconds(3));
  ASSERT_TRUE(terminal.contains("result")) << terminal.dump();
  EXPECT_EQ(terminal["result"]["state"], "failed");
  EXPECT_EQ(terminal["result"]["status"]["domain"], "daemon");
  EXPECT_EQ(terminal["result"]["status"]["name"], "internal_error");
  EXPECT_TRUE(terminal["result"]["output"].is_null());
  EXPECT_TRUE(raw_daemon_call(invalid_socket, "compute.release",
                              internal::Json{{"compute_id", invalid_id}},
                              "invalid-image-release")
                  .contains("result"));
  invalid_daemon.stop();
  ASSERT_TRUE(invalid_daemon.exited_successfully());

  const std::string quota_socket = (temp.path() / "quota.sock").string();
  DaemonProcess quota_daemon;
  quota_daemon.start(quota_socket, true, {}, -1, "nonempty",
                     clock.path().string());
  ASSERT_TRUE(quota_daemon.wait_ready(std::chrono::seconds(5)));
  const IpcResult<GraphSessionSummary> quota_session = load_fixture_session(
      quota_socket, temp.path() / "quota-sessions", "quota_output");
  ASSERT_TRUE(quota_session.status.ok) << quota_session.status.message;

  const std::filesystem::path quota_instance =
      find_output_instance_directory(quota_socket);
  {
    ScopedDirectoryMode block_publication(quota_instance, 0500);
    submitted = raw_daemon_call(
        quota_socket, "compute.submit",
        image_compute_submit_params(quota_session.value.session_id),
        "filesystem-publication-submit");
    ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
    const std::string publication_failure_id =
        submitted["result"]["compute_id"].get<std::string>();
    terminal = wait_for_real_compute_terminal(
        quota_socket, publication_failure_id, std::chrono::seconds(3));
    ASSERT_TRUE(terminal.contains("result")) << terminal.dump();
    EXPECT_EQ(terminal["result"]["state"], "failed");
    EXPECT_EQ(terminal["result"]["status"]["domain"], "daemon");
    EXPECT_EQ(terminal["result"]["status"]["name"], "internal_error");
    EXPECT_TRUE(terminal["result"]["output"].is_null());
    const internal::Json publication_result =
        raw_daemon_call(quota_socket, "compute.result",
                        internal::Json{{"compute_id", publication_failure_id}},
                        "filesystem-publication-result");
    ASSERT_TRUE(publication_result.contains("result"))
        << publication_result.dump();
    EXPECT_EQ(publication_result["result"]["state"], "failed");
    EXPECT_EQ(publication_result["result"]["status"]["domain"], "daemon");
    EXPECT_EQ(publication_result["result"]["status"]["name"], "internal_error");
    EXPECT_TRUE(publication_result["result"]["output"].is_null());
    EXPECT_TRUE(
        raw_daemon_call(quota_socket, "compute.release",
                        internal::Json{{"compute_id", publication_failure_id}},
                        "filesystem-publication-release")
            .contains("result"));
    EXPECT_TRUE(std::filesystem::is_empty(quota_instance));
    block_publication.restore();
  }

  std::vector<std::string> compute_ids;
  compute_ids.reserve(3);
  for (std::size_t index = 0; index < 3; ++index) {
    submitted = raw_daemon_call(
        quota_socket, "compute.submit",
        image_compute_submit_params(quota_session.value.session_id),
        "quota-submit-" + std::to_string(index));
    ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
    const std::string compute_id =
        submitted["result"]["compute_id"].get<std::string>();
    compute_ids.push_back(compute_id);
    terminal = wait_for_real_compute_terminal(quota_socket, compute_id,
                                              std::chrono::seconds(3));
    ASSERT_TRUE(terminal.contains("result")) << terminal.dump();
    if (index < 2) {
      EXPECT_EQ(terminal["result"]["state"], "succeeded") << index;
    } else {
      EXPECT_EQ(terminal["result"]["state"], "failed");
      EXPECT_EQ(terminal["result"]["status"]["domain"], "daemon");
      EXPECT_EQ(terminal["result"]["status"]["name"],
                "artifact_limit_exceeded");
      EXPECT_TRUE(terminal["result"]["output"].is_null());
    }
  }
  for (std::size_t index = 0; index < compute_ids.size(); ++index) {
    const internal::Json released =
        raw_daemon_call(quota_socket, "compute.release",
                        internal::Json{{"compute_id", compute_ids[index]}},
                        "quota-release-" + std::to_string(index));
    EXPECT_TRUE(released.contains("result")) << released.dump();
  }
  quota_daemon.stop();
  EXPECT_TRUE(quota_daemon.exited_successfully());
  EXPECT_TRUE(std::filesystem::is_empty(quota_socket + ".outputs"));
}

TEST(IpcOutputFixtureDaemon,
     TamperIsTopLevelAndOrphanLeaseSurvivesNormalJobRemoval) {
  ScopedDaemonDirectory temp("ps-output-race", true);
  const std::string socket_path = (temp.path() / "race.sock").string();
  ManualProcessClock clock(temp.path() / "clock.bin");
  DaemonProcess daemon;
  daemon.start(socket_path, true, {}, -1, "nonempty", clock.path().string());
  ASSERT_TRUE(daemon.wait_ready(std::chrono::seconds(5)));
  const IpcResult<GraphSessionSummary> loaded = load_fixture_session(
      socket_path, temp.path() / "sessions", "output_races");
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  internal::Json submitted = raw_daemon_call(
      socket_path, "compute.submit",
      image_compute_submit_params(loaded.value.session_id), "tamper-submit");
  ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
  const std::string tamper_id =
      submitted["result"]["compute_id"].get<std::string>();
  ScopedFixtureJobRelease tamper_guard(socket_path, tamper_id);
  ASSERT_TRUE(wait_for_real_compute_terminal(socket_path, tamper_id,
                                             std::chrono::seconds(3))
                  .contains("result"));
  internal::Json result = raw_daemon_call(
      socket_path, "compute.result", internal::Json{{"compute_id", tamper_id}},
      "tamper-result");
  ASSERT_TRUE(result.contains("result")) << result.dump();
  internal::Json tamper_output = result["result"]["output"];
  if (tamper_output.is_object() &&
      tamper_output.value("delivery_id", internal::Json()).is_string()) {
    tamper_guard.protect_delivery(
        tamper_output["delivery_id"].get<std::string>());
  }
  ASSERT_TRUE(tamper_output.is_object()) << result.dump();
  const std::string tamper_path = tamper_output["path"].get<std::string>();
  const std::filesystem::path victim = temp.path() / "victim.bin";
  {
    std::ofstream stream(victim, std::ios::binary);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.put('v');
  }
  ASSERT_EQ(::chmod(victim.c_str(), 0600), 0);
  ASSERT_EQ(::unlink(tamper_path.c_str()), 0);
  ASSERT_EQ(::symlink(victim.c_str(), tamper_path.c_str()), 0);
  const internal::Json missing = raw_daemon_call(
      socket_path, "compute.result", internal::Json{{"compute_id", tamper_id}},
      "tamper-result-missing");
  ASSERT_TRUE(missing.contains("error")) << missing.dump();
  EXPECT_EQ(missing["error"]["domain"], "daemon");
  EXPECT_EQ(missing["error"]["name"], "artifact_not_found");
  const internal::Json tamper_released = raw_daemon_call(
      socket_path, "compute.release",
      internal::Json{{"compute_id", tamper_id},
                     {"delivery_id", tamper_output["delivery_id"]}},
      "tamper-release");
  ASSERT_TRUE(tamper_released.contains("result")) << tamper_released.dump();
  tamper_guard.dismiss();
  struct stat replacement{};
  ASSERT_EQ(::lstat(tamper_path.c_str(), &replacement), 0);
  EXPECT_TRUE(S_ISLNK(replacement.st_mode));
  EXPECT_TRUE(std::filesystem::exists(victim));

  submitted = raw_daemon_call(
      socket_path, "compute.submit",
      image_compute_submit_params(loaded.value.session_id), "orphan-submit");
  ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
  const std::string orphan_id =
      submitted["result"]["compute_id"].get<std::string>();
  ScopedFixtureJobRelease orphan_guard(socket_path, orphan_id);
  ASSERT_TRUE(wait_for_real_compute_terminal(socket_path, orphan_id,
                                             std::chrono::seconds(3))
                  .contains("result"));
  result = raw_daemon_call(socket_path, "compute.result",
                           internal::Json{{"compute_id", orphan_id}},
                           "orphan-result");
  ASSERT_TRUE(result.contains("result")) << result.dump();
  const internal::Json orphan_output = result["result"]["output"];
  if (orphan_output.is_object() &&
      orphan_output.value("delivery_id", internal::Json()).is_string()) {
    orphan_guard.protect_delivery(
        orphan_output["delivery_id"].get<std::string>());
  }
  ASSERT_TRUE(orphan_output.is_object()) << result.dump();
  const std::string orphan_path = orphan_output["path"].get<std::string>();
  auto release_gate = std::make_shared<ConcurrentCallGate>();
  RawDaemonCallTask normal_release(
      socket_path, "compute.release", internal::Json{{"compute_id", orphan_id}},
      "orphan-remove-job", std::chrono::seconds(3), true, release_gate);
  RawDaemonCallTask paired_release(
      socket_path, "compute.release",
      internal::Json{{"compute_id", orphan_id},
                     {"delivery_id", orphan_output["delivery_id"]}},
      "orphan-release-lease", std::chrono::seconds(3), true, release_gate);
  const bool release_calls_ready =
      release_gate->wait_ready(2, std::chrono::seconds(2));
  release_gate->release();
  const bool normal_release_complete =
      normal_release.wait_for(std::chrono::seconds(3));
  const bool paired_release_complete =
      paired_release.wait_for(std::chrono::seconds(3));
  const internal::Json job_removed = normal_release.join();
  const internal::Json lease_released = paired_release.join();
  ASSERT_TRUE(release_calls_ready);
  ASSERT_TRUE(normal_release_complete) << job_removed.dump();
  ASSERT_TRUE(paired_release_complete) << lease_released.dump();
  ASSERT_TRUE(lease_released.contains("result")) << lease_released.dump();
  if (job_removed.contains("error")) {
    EXPECT_EQ(job_removed["error"]["domain"], "daemon");
    EXPECT_EQ(job_removed["error"]["name"], "job_not_found");
  } else {
    EXPECT_TRUE(job_removed.contains("result")) << job_removed.dump();
  }
  orphan_guard.dismiss();
  EXPECT_FALSE(std::filesystem::exists(orphan_path));

  daemon.stop();
  EXPECT_TRUE(daemon.exited_successfully());
}

TEST(IpcOutputFixtureDaemon,
     AbruptDeathLeavesSafeArtifactForRestartAndFinalCleanup) {
  ScopedDaemonDirectory temp("ps-output-restart", true);
  const std::string socket_path = (temp.path() / "restart.sock").string();
  ManualProcessClock clock(temp.path() / "clock.bin");
  DaemonProcess daemon;
  daemon.start(socket_path, true, {}, -1, "nonempty", clock.path().string());
  ASSERT_TRUE(daemon.wait_ready(std::chrono::seconds(5)));
  const IpcResult<GraphSessionSummary> loaded = load_fixture_session(
      socket_path, temp.path() / "sessions", "restart_output");
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;
  const internal::Json submitted = raw_daemon_call(
      socket_path, "compute.submit",
      image_compute_submit_params(loaded.value.session_id), "restart-submit");
  ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
  const std::string compute_id =
      submitted["result"]["compute_id"].get<std::string>();
  ASSERT_TRUE(wait_for_real_compute_terminal(socket_path, compute_id,
                                             std::chrono::seconds(3))
                  .contains("result"));
  const internal::Json result = raw_daemon_call(
      socket_path, "compute.result", internal::Json{{"compute_id", compute_id}},
      "restart-result");
  ASSERT_TRUE(result.contains("result")) << result.dump();
  ASSERT_TRUE(result["result"]["output"].is_object()) << result.dump();
  const std::string stale_path =
      result["result"]["output"]["path"].get<std::string>();
  ASSERT_TRUE(std::filesystem::exists(stale_path));
  ASSERT_TRUE(daemon.crash_and_wait(std::chrono::seconds(3)));
  EXPECT_TRUE(std::filesystem::exists(stale_path));

  daemon.start(socket_path, true, {}, -1, "empty", clock.path().string());
  ASSERT_TRUE(daemon.wait_ready(std::chrono::seconds(5)));
  EXPECT_FALSE(std::filesystem::exists(stale_path));
  daemon.stop();
  EXPECT_TRUE(daemon.exited_successfully());
  EXPECT_FALSE(std::filesystem::exists(socket_path));
  EXPECT_TRUE(std::filesystem::exists(socket_path + ".lock"));
  EXPECT_TRUE(std::filesystem::is_empty(socket_path + ".outputs"));
}

TEST(IpcDaemonComputeLifecycle,
     DisconnectAndIndependentClientsPreserveOneTerminalJob) {
  ScopedDaemonDirectory temp("photospider-ipc-daemon-compute");
  const std::string socket_path = (temp.path() / "compute.sock").string();
  const std::filesystem::path yaml_path = temp.path() / "source.yaml";
  write_ipc_graph(yaml_path);
  DaemonProcess daemon;
  daemon.start(socket_path);
  ASSERT_TRUE(daemon.wait_ready(std::chrono::seconds(5)));

  Client loader;
  ASSERT_TRUE(loader.connect(socket_path).ok);
  GraphLoadRequest load;
  load.session = GraphSessionId{"compute_lifecycle"};
  load.root_dir = (temp.path() / "sessions").string();
  load.yaml_path = yaml_path.string();
  load.cache_root_dir = (temp.path() / "cache").string();
  const IpcResult<GraphSessionSummary> loaded = loader.load_graph(load);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  internal::Json submitted =
      raw_daemon_call(socket_path, "compute.submit",
                      minimal_compute_submit_params(loaded.value.session_id),
                      "disconnect-submit");
  ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
  ASSERT_EQ(submitted["result"].size(), 6U);
  EXPECT_EQ(submitted["result"]["state"], "queued");
  EXPECT_FALSE(submitted["result"]["cancellable"].get<bool>());
  EXPECT_TRUE(submitted["result"]["status"].is_null());
  EXPECT_TRUE(submitted["result"]["output"].is_null());
  const std::string compute_id =
      submitted["result"]["compute_id"].get<std::string>();

  std::string partial_error;
  internal::UniqueFd partial =
      internal::connect_unix_socket(socket_path, &partial_error);
  ASSERT_TRUE(partial) << partial_error;
  const unsigned char partial_header[] = {0, 0};
  ASSERT_EQ(::send(partial.get(), partial_header, sizeof(partial_header), 0),
            static_cast<ssize_t>(sizeof(partial_header)));
  partial.reset();

  RawDaemonCallTask first_poll(socket_path, "compute.status",
                               internal::Json{{"compute_id", compute_id}},
                               "multiclient-left", std::chrono::seconds(3));
  RawDaemonCallTask second_poll(socket_path, "compute.status",
                                internal::Json{{"compute_id", compute_id}},
                                "multiclient-right", std::chrono::seconds(3));
  const internal::Json first_status = first_poll.join();
  const internal::Json second_status = second_poll.join();
  ASSERT_TRUE(first_status.contains("result")) << first_status.dump();
  ASSERT_TRUE(second_status.contains("result")) << second_status.dump();
  EXPECT_EQ(first_status["result"]["compute_id"], compute_id);
  EXPECT_EQ(second_status["result"]["compute_id"], compute_id);

  internal::Json terminal = wait_for_real_compute_terminal(
      socket_path, compute_id, std::chrono::seconds(3));
  ASSERT_TRUE(terminal.contains("result")) << terminal.dump();
  EXPECT_EQ(terminal["result"]["state"], "failed");
  EXPECT_EQ(terminal["result"]["status"]["domain"], "graph");
  EXPECT_FALSE(terminal["result"]["status"]["ok"].get<bool>());
  EXPECT_TRUE(terminal["result"]["output"].is_null());
  const internal::Json result = raw_daemon_call(
      socket_path, "compute.result", internal::Json{{"compute_id", compute_id}},
      "terminal-result");
  ASSERT_TRUE(result.contains("result")) << result.dump();
  EXPECT_EQ(result["result"], terminal["result"]);
  const internal::Json released = raw_daemon_call(
      socket_path, "compute.release",
      internal::Json{{"compute_id", compute_id}}, "terminal-release");
  EXPECT_EQ(released["result"],
            (internal::Json{{"compute_id", compute_id}, {"released", true}}));

  ASSERT_TRUE(loader.close_graph(loaded.value.session_id).status.ok);
  loader.disconnect();
  daemon.stop();
  EXPECT_TRUE(daemon.exited_successfully());
  EXPECT_FALSE(std::filesystem::exists(socket_path));
}

TEST(IpcDaemonComputeLifecycle,
     ActiveReloadAndQueuedJobsHoldGraphCloseUntilFifoRelease) {
  ScopedDaemonDirectory temp("psac", true);
  const std::string socket_path = (temp.path() / "active-close.sock").string();
  const std::filesystem::path yaml_path = temp.path() / "source.yaml";
  write_ipc_graph(yaml_path);
  BlockingHostFifo reload_fifo(temp.path() / "reload.yaml.fifo");
  DaemonProcess daemon;
  daemon.start(socket_path);
  ASSERT_TRUE(daemon.wait_ready(std::chrono::seconds(5)));

  Client loader;
  ASSERT_TRUE(loader.connect(socket_path).ok);
  GraphLoadRequest load;
  load.session = GraphSessionId{"active_close"};
  load.root_dir = (temp.path() / "sessions").string();
  load.yaml_path = yaml_path.string();
  load.cache_root_dir = (temp.path() / "cache").string();
  const IpcResult<GraphSessionSummary> loaded = loader.load_graph(load);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  RawDaemonCallTask reloading(
      socket_path, "graph.reload",
      internal::Json{
          {"session_id", loaded.value.session_id.value},
          {"yaml_path", (temp.path() / "reload.yaml.fifo").string()}},
      "active-close-reload", std::chrono::seconds(12));
  if (!reload_fifo.wait_for_reader(std::chrono::seconds(3))) {
    daemon.stop();
    (void)reloading.join();
    FAIL() << reload_fifo.message();
    return;
  }

  std::vector<std::string> compute_ids;
  for (std::int64_t node : {1, 2}) {
    const internal::Json submitted = raw_daemon_call(
        socket_path, "compute.submit",
        minimal_compute_submit_params(loaded.value.session_id, node),
        "active-close-submit-" + std::to_string(node));
    if (!submitted.contains("result")) {
      (void)reload_fifo.release();
      daemon.stop();
      (void)reloading.join();
      FAIL() << "active close compute submission failed: " << submitted.dump();
      return;
    }
    compute_ids.push_back(submitted["result"]["compute_id"].get<std::string>());
  }
  const internal::Json queued = wait_for_real_compute_state(
      socket_path, compute_ids.back(), "queued", std::chrono::seconds(3));
  if (!queued.contains("result") || queued["result"]["state"] != "queued") {
    (void)reload_fifo.release();
    daemon.stop();
    (void)reloading.join();
    FAIL() << "second active close job was not queued: " << queued.dump();
    return;
  }

  std::string abandoned_close_message;
  internal::UniqueFd abandoned_close = send_raw_daemon_request(
      socket_path, "graph.close",
      internal::Json{{"session_id", loaded.value.session_id.value}},
      "active-close-lost-response",
      std::chrono::steady_clock::now() + std::chrono::seconds(3),
      &abandoned_close_message);
  if (!abandoned_close) {
    (void)reload_fifo.release();
    daemon.stop();
    (void)reloading.join();
    FAIL() << abandoned_close_message;
    return;
  }
  bool closing_observed = false;
  const auto closing_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  std::size_t probe_sequence = 0;
  while (std::chrono::steady_clock::now() < closing_deadline) {
    const internal::Json probe = raw_daemon_call(
        socket_path, "compute.submit",
        minimal_compute_submit_params(loaded.value.session_id),
        "active-close-admission-" + std::to_string(probe_sequence++),
        std::chrono::milliseconds(500));
    if (probe.contains("error") && probe["error"]["domain"] == "graph" &&
        probe["error"]["name"] == "not_found") {
      closing_observed = true;
      break;
    }
    if (probe.contains("result")) {
      compute_ids.push_back(probe["result"]["compute_id"].get<std::string>());
    }
    std::this_thread::yield();
  }
  if (!closing_observed) {
    (void)reload_fifo.release();
    daemon.stop();
    (void)reloading.join();
    FAIL() << "graph close did not enter its blocked closing state";
    return;
  }

  RawDaemonCallTask closing(
      socket_path, "graph.close",
      internal::Json{{"session_id", loaded.value.session_id.value}},
      "active-close-joined", std::chrono::seconds(12));
  abandoned_close.reset();
  if (!reload_fifo.release()) {
    daemon.stop();
    (void)closing.join();
    (void)reloading.join();
    FAIL() << reload_fifo.message();
    return;
  }
  if (!reloading.wait_for(std::chrono::seconds(5)) ||
      !closing.wait_for(std::chrono::seconds(8))) {
    daemon.stop();
    (void)closing.join();
    (void)reloading.join();
    FAIL() << "reload or close did not finish after FIFO release";
    return;
  }
  const internal::Json reloaded = reloading.join();
  const internal::Json closed = closing.join();
  ASSERT_TRUE(reloaded.contains("result")) << reloaded.dump();
  ASSERT_TRUE(closed.contains("result")) << closed.dump();
  EXPECT_TRUE(closed["result"]["closed"].get<bool>());
  const internal::Json late_close = raw_daemon_call(
      socket_path, "graph.close",
      internal::Json{{"session_id", loaded.value.session_id.value}},
      "active-close-late");
  ASSERT_TRUE(late_close.contains("error")) << late_close.dump();
  EXPECT_EQ(late_close["error"]["domain"], "graph");
  EXPECT_EQ(late_close["error"]["name"], "not_found");

  for (const std::string& compute_id : compute_ids) {
    const internal::Json terminal = wait_for_real_compute_terminal(
        socket_path, compute_id, std::chrono::seconds(3));
    ASSERT_TRUE(terminal.contains("result")) << terminal.dump();
    EXPECT_TRUE(terminal["result"]["state"] == "succeeded" ||
                terminal["result"]["state"] == "failed");
    ASSERT_TRUE(raw_daemon_call(socket_path, "compute.release",
                                internal::Json{{"compute_id", compute_id}},
                                "active-close-release")
                    .contains("result"));
  }
  loader.disconnect();
  daemon.stop();
  EXPECT_TRUE(daemon.exited_successfully());
  EXPECT_FALSE(std::filesystem::exists(socket_path));
}

TEST(IpcDaemonComputeLifecycle,
     ActiveReloadAndQueuedJobHoldSignalShutdownUntilFifoRelease) {
  ScopedDaemonDirectory temp("psas", true);
  const std::string socket_path =
      (temp.path() / "active-shutdown.sock").string();
  const std::filesystem::path yaml_path = temp.path() / "source.yaml";
  write_ipc_graph(yaml_path);
  BlockingHostFifo reload_fifo(temp.path() / "reload.yaml.fifo");
  DaemonProcess daemon;
  daemon.start(socket_path);
  ASSERT_TRUE(daemon.wait_ready(std::chrono::seconds(5)));

  Client loader;
  ASSERT_TRUE(loader.connect(socket_path).ok);
  GraphLoadRequest load;
  load.session = GraphSessionId{"active_shutdown"};
  load.root_dir = (temp.path() / "sessions").string();
  load.yaml_path = yaml_path.string();
  load.cache_root_dir = (temp.path() / "cache").string();
  const IpcResult<GraphSessionSummary> loaded = loader.load_graph(load);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  RawDaemonCallTask reloading(
      socket_path, "graph.reload",
      internal::Json{
          {"session_id", loaded.value.session_id.value},
          {"yaml_path", (temp.path() / "reload.yaml.fifo").string()}},
      "active-shutdown-reload", std::chrono::seconds(12), false);
  if (!reload_fifo.wait_for_reader(std::chrono::seconds(3))) {
    daemon.stop();
    (void)reloading.join();
    FAIL() << reload_fifo.message();
    return;
  }

  std::vector<std::string> compute_ids;
  for (std::int64_t node : {1, 2}) {
    const internal::Json submitted = raw_daemon_call(
        socket_path, "compute.submit",
        minimal_compute_submit_params(loaded.value.session_id, node),
        "active-shutdown-submit-" + std::to_string(node));
    if (!submitted.contains("result")) {
      (void)reload_fifo.release();
      daemon.stop();
      (void)reloading.join();
      FAIL() << "active shutdown compute submission failed: "
             << submitted.dump();
      return;
    }
    compute_ids.push_back(submitted["result"]["compute_id"].get<std::string>());
  }
  const internal::Json queued = wait_for_real_compute_state(
      socket_path, compute_ids.back(), "queued", std::chrono::seconds(3));
  if (!queued.contains("result") || queued["result"]["state"] != "queued") {
    (void)reload_fifo.release();
    daemon.stop();
    (void)reloading.join();
    FAIL() << "second active shutdown job was not queued: " << queued.dump();
    return;
  }

  if (!daemon.request_stop() ||
      !wait_for_listener_shutdown(socket_path, std::chrono::seconds(3))) {
    (void)reload_fifo.release();
    daemon.stop();
    (void)reloading.join();
    FAIL() << "signal shutdown did not stop listener admission";
    return;
  }
  const bool exited_before_release = daemon.poll_exit();
  if (!reload_fifo.release()) {
    daemon.stop();
    (void)reloading.join();
    FAIL() << reload_fifo.message();
    return;
  }
  const bool exited_after_release =
      daemon.wait_for_exit(std::chrono::seconds(8));
  (void)reloading.join();
  EXPECT_FALSE(exited_before_release);
  ASSERT_TRUE(exited_after_release);
  EXPECT_TRUE(daemon.exited_successfully());
  EXPECT_FALSE(std::filesystem::exists(socket_path));
}

}  // namespace
}  // namespace ps::ipc
