#include <arpa/inet.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ipc/frame.hpp"
#include "ipc/unix_socket.hpp"
#include "photospider/ipc/client.hpp"

#ifndef PS_PHOTOSPIDERD_PATH
#error "PS_PHOTOSPIDERD_PATH must name the real daemon executable"
#endif

namespace ps::ipc {
namespace {

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

std::uint64_t ScopedDaemonDirectory::sequence_ = 0;

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
 * @brief RAII owner for one real photospiderd child process.
 *
 * The destructor enforces a bounded SIGTERM-to-SIGKILL-to-waitpid sequence, so
 * an assertion or daemon bug cannot leak a child or hang CTest indefinitely.
 *
 * @throws std::runtime_error if fork fails.
 * @note Readiness is established by typed `daemon.ping`, never fixed sleep.
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
   * @brief Terminates and reaps a remaining child within a hard deadline.
   *
   * @throws Nothing.
   */
  ~DaemonProcess() { stop(); }

  /**
   * @brief Starts the real foreground daemon.
   *
   * @param socket_path Expected socket path; passed through `--socket` when
   *        `explicit_socket` is true.
   * @param explicit_socket Whether to use the explicit option.
   * @param xdg_runtime Optional protected runtime root installed in the child
   *        environment for default-path tests.
   * @param start_gate_fd Optional descriptor from which the child must consume
   *        one byte before exec.
   * @throws std::bad_alloc if copied path/environment storage cannot be
   *         allocated.
   * @throws std::runtime_error if fork fails.
   * @note The child uses `execl` and never initializes Host before exec.
   */
  void start(std::string socket_path, bool explicit_socket = true,
             std::string xdg_runtime = {}, int start_gate_fd = -1) {
    stop();
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
      int status = 0;
      const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
      if (waited == pid_) {
        record_exit(status);
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
   * @brief Waits for natural child exit within a hard deadline.
   *
   * @param timeout Maximum wait duration.
   * @return True if the child was reaped.
   * @throws Nothing.
   */
  bool wait_for_exit(std::chrono::milliseconds timeout) noexcept {
    if (pid_ < 0) {
      return true;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
      if (waited == pid_) {
        record_exit(status);
        return true;
      }
      if (waited < 0 && errno != EINTR) {
        pid_ = -1;
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  }

  /**
   * @brief Sends SIGTERM and enforces bounded reap, escalating to SIGKILL.
   *
   * @throws Nothing.
   * @note Normal daemon shutdown is allowed five seconds before escalation.
   */
  void stop() noexcept {
    if (pid_ < 0) {
      return;
    }
    (void)::kill(pid_, SIGTERM);
    if (wait_for_exit(std::chrono::seconds(5))) {
      return;
    }
    (void)::kill(pid_, SIGKILL);
    int status = 0;
    while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
    }
    record_exit(status);
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
   * @brief Reports whether a reaped child had a nonzero/abnormal exit.
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
   * @brief Reports whether this process has already been reaped.
   *
   * @return True after any terminal status has been recorded.
   * @throws Nothing.
   */
  bool has_exited() const noexcept { return exited_; }

 private:
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

  /** @brief Owned child pid, or -1 once absent/reaped. */
  pid_t pid_ = -1;

  /** @brief Socket path used for readiness probes. */
  std::string socket_path_;

  /** @brief Whether a terminal child state has been recorded. */
  bool exited_ = false;

  /** @brief Whether the terminal state was a normal process exit. */
  bool normal_exit_ = false;

  /** @brief Normal exit code, or -1 for a signal termination. */
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
    Client probe;
    if (probe.connect(socket_path).ok && probe.ping().status.ok) {
      probe.disconnect();
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
  EXPECT_EQ(version.value.protocol_version, 1);
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
  EXPECT_EQ(version.value.methods,
            (std::vector<std::string>{"daemon.ping", "daemon.version",
                                      "graph.close", "graph.list", "graph.load",
                                      "inspect.dependency_tree",
                                      "inspect.graph", "inspect.node"}));

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
  ASSERT_EQ(::chmod(unsafe_lock_path.c_str(), 04600), 0);
  DaemonProcess special_mode_lock_daemon;
  special_mode_lock_daemon.start(unsafe_lock_socket);
  EXPECT_TRUE(special_mode_lock_daemon.wait_for_exit(std::chrono::seconds(5)));
  EXPECT_TRUE(special_mode_lock_daemon.exited_with_failure());
  EXPECT_EQ(permissions_of(unsafe_lock_path), 04600);
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
      (xdg.path() / "photospider" / "photospiderd-v1.sock").string();
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
                                    "/photospiderd-v1.sock";
  DaemonProcess fallback_daemon;
  fallback_daemon.start(fallback_path, false, long_xdg.string());
  ASSERT_TRUE(fallback_daemon.wait_ready(std::chrono::seconds(5)));
  fallback_daemon.stop();
  EXPECT_TRUE(fallback_daemon.exited_successfully());
  EXPECT_FALSE(std::filesystem::exists(fallback_path));
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
      R"({"protocol_version":1,"method":"daemon.ping","params":{}})";
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
  const auto missing_node =
      second.inspect_node(loaded.value.session_id, NodeId{999});
  EXPECT_FALSE(missing_node.status.ok);
  EXPECT_EQ(missing_node.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(missing_node.status.code,
            static_cast<std::int32_t>(GraphErrc::NotFound));

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

}  // namespace
}  // namespace ps::ipc
