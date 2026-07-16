#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ipc/server.hpp"
#include "photospider/core/image_buffer.hpp"
#include "support/ipc_host_spy.hpp"

namespace {

/** @brief Signal-visible write descriptor for the fixture stop pipe. */
volatile std::sig_atomic_t g_stop_write_fd = -1;

/**
 * @brief Selects the deterministic image result returned by the test Host.
 * @throws Nothing.
 */
enum class ImageMode {
  /** @brief Canonical successful empty CPU image. */
  Empty,
  /** @brief Successful two-by-two tight-row UINT8 CPU image. */
  Nonempty,
  /** @brief Malformed image used to exercise nested image validation failure.
   */
  Invalid,
};

/**
 * @brief Complete command-line policy for one fixture server process.
 * @throws std::bad_alloc when path storage cannot be allocated.
 */
struct Options {
  /** @brief Absolute Unix socket path owned by the real internal Server. */
  std::string socket_path;

  /** @brief Deterministic Host image behavior for every image compute. */
  ImageMode image_mode = ImageMode::Empty;

  /** @brief Existing fixed-width manual monotonic-clock control file. */
  std::string clock_control_path;
};

/**
 * @brief Reads a cross-process fixed-width monotonic test clock.
 *
 * @throws std::runtime_error if the control path is unsafe or cannot provide
 *         one complete locked 64-bit nanosecond sample.
 * @note The parent test owns updates through one locked `pwrite`. One
 *       process-local mutex serializes the complete shared-lock, exact-offset
 *       `pread`, unlock, and monotonic-clamp transaction because `flock`
 *       ownership is associated with the single retained open-file
 *       description rather than with an individual fixture thread. This
 *       class is fixture-only and never enters the product daemon.
 */
class ControlFileClock final {
 public:
  /**
   * @brief Opens and validates an existing test-owned control file.
   * @param path Absolute regular mode-0600 file created by the parent test.
   * @throws std::runtime_error for open or identity/size validation failure.
   */
  explicit ControlFileClock(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat metadata{};
    if (fd_ < 0 || ::fstat(fd_, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_uid != ::geteuid() || (metadata.st_mode & 07777) != 0600 ||
        metadata.st_nlink != 1 ||
        metadata.st_size != static_cast<off_t>(sizeof(std::uint64_t))) {
      if (fd_ >= 0) {
        (void)::close(fd_);
        fd_ = -1;
      }
      throw std::runtime_error("fixture clock control file is unsafe");
    }
  }

  /** @brief Closes the owned control descriptor. @throws Nothing. */
  ~ControlFileClock() noexcept {
    if (fd_ >= 0) {
      (void)::close(fd_);
    }
  }

  /**
   * @brief Prevents duplicate control descriptor ownership.
   * @throws Nothing because construction is unavailable.
   */
  ControlFileClock(const ControlFileClock&) = delete;

  /**
   * @brief Prevents replacing descriptor ownership by assignment.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ControlFileClock& operator=(const ControlFileClock&) = delete;

  /**
   * @brief Reads and monotonically clamps one serialized test-clock sample.
   * @return Stable `steady_clock` point derived from unsigned nanoseconds.
   * @throws std::runtime_error if locking, exact-width reading, or unlocking
   *         fails.
   * @note The process-local mutex covers the complete `LOCK_SH` through
   *       `pread`, `LOCK_UN`, and clamp transaction. The constructor's one
   *       validated descriptor is reused for every sample; this method does
   *       not duplicate or reopen it.
   */
  std::chrono::steady_clock::time_point now() const {
    std::lock_guard<std::mutex> sample_lock(sample_mutex_);
    lock(LOCK_SH);
    std::uint64_t nanoseconds = 0;
    const ssize_t count = ::pread(fd_, &nanoseconds, sizeof(nanoseconds), 0);
    const int read_error = errno;
    unlock();
    if (count != static_cast<ssize_t>(sizeof(nanoseconds))) {
      errno = read_error;
      throw std::runtime_error("fixture clock control read failed");
    }
    if (nanoseconds < last_nanoseconds_) {
      nanoseconds = last_nanoseconds_;
    } else {
      last_nanoseconds_ = nanoseconds;
    }
    return std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(nanoseconds)));
  }

 private:
  /**
   * @brief Acquires one advisory lock while retrying interruption.
   * @param operation Advisory lock operation; `now()` passes `LOCK_SH`.
   * @return Nothing.
   * @throws std::runtime_error if the lock operation fails.
   * @note The caller must hold `sample_mutex_` so the retained descriptor's
   *       process-local lock transaction cannot overlap another sample.
   */
  void lock(int operation) const {
    int result = -1;
    do {
      result = ::flock(fd_, operation);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
      throw std::runtime_error("fixture clock control lock failed");
    }
  }

  /**
   * @brief Releases the current shared advisory lock exactly once.
   * @return Nothing.
   * @throws std::runtime_error when unlocking fails for a reason other than an
   *         interrupted system call.
   * @note Interrupted unlock attempts are retried while `sample_mutex_`
   *       remains held, preserving the complete sampling transaction.
   */
  void unlock() const {
    int result = -1;
    do {
      result = ::flock(fd_, LOCK_UN);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
      throw std::runtime_error("fixture clock control unlock failed");
    }
  }

  /** @brief Owned validated control-file descriptor. */
  int fd_ = -1;

  /**
   * @brief Serializes shared lock, read, unlock, and monotonic-clamp sampling.
   * @note Required because every fixture thread reuses `fd_` and `flock`
   *       associates locks with that retained open-file description.
   */
  mutable std::mutex sample_mutex_;

  /** @brief Greatest fixed-width sample returned by this fixture process. */
  mutable std::uint64_t last_nanoseconds_ = 0;
};

/**
 * @brief Produces unique deterministic 32-lowercase-hex fixture identities.
 * @throws std::runtime_error if the bounded counter is exhausted.
 * @note Atomic allocation permits snapshot, compute, and output callbacks to
 *       share one source across server threads without data races.
 */
class DeterministicIdSource final {
 public:
  /**
   * @brief Returns the next opaque identity.
   * @return Unique fixed-width lowercase hexadecimal text.
   * @throws std::runtime_error if the counter wraps.
   */
  std::string next() {
    const std::uint64_t value = next_.fetch_add(1, std::memory_order_relaxed);
    if (value == 0) {
      throw std::runtime_error("fixture opaque-id counter exhausted");
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result(32, '0');
    std::uint64_t remaining = value;
    for (std::size_t index = result.size(); index > 0 && remaining != 0;
         --index) {
      result[index - 1] = kHex[remaining & 0x0fU];
      remaining >>= 4U;
    }
    return result;
  }

 private:
  /** @brief Next nonzero identity suffix. */
  std::atomic<std::uint64_t> next_{1};
};

/**
 * @brief Builds the fixture's coherent small runtime dependency policy.
 * @param clock Cross-process manual monotonic clock shared by all components.
 * @param ids Thread-safe deterministic opaque-id source.
 * @return Snapshot/compute/output policies with short deterministic TTLs.
 * @throws std::bad_alloc if callback storage cannot allocate.
 * @note Limits are intentionally fixture-only: three terminal jobs, two
 *       artifacts, ten-second job retention, and one-second delivery leases.
 */
ps::ipc::internal::RequestRouterRuntimeDependencies fixture_dependencies(
    const std::shared_ptr<ControlFileClock>& clock,
    const std::shared_ptr<DeterministicIdSource>& ids) {
  ps::ipc::internal::RequestRouterRuntimeDependencies dependencies;
  dependencies.snapshot_limits.records = 4;
  dependencies.snapshot_limits.total_bytes = 4 * 1024 * 1024;
  dependencies.snapshot_limits.reservation_bytes = 1024 * 1024;
  dependencies.snapshot_limits.snapshot_bytes = 1024 * 1024;
  dependencies.snapshot_limits.ttl = std::chrono::seconds(10);
  dependencies.snapshot_clock = [clock] { return clock->now(); };
  dependencies.snapshot_id_generator = [ids] { return ids->next(); };
  dependencies.compute_limits.active = 8;
  dependencies.compute_limits.terminal = 3;
  dependencies.compute_limits.terminal_ttl = std::chrono::seconds(10);
  dependencies.compute_clock = [clock] { return clock->now(); };
  dependencies.compute_id_generator = [ids] { return ids->next(); };
  dependencies.output_limits.artifacts = 2;
  dependencies.output_limits.total_bytes = 1024;
  dependencies.output_limits.artifact_bytes = 256;
  dependencies.output_limits.job_ttl = std::chrono::seconds(10);
  dependencies.output_limits.delivery_ttl = std::chrono::seconds(1);
  dependencies.output_clock = [clock] { return clock->now(); };
  dependencies.output_id_generator = [ids] { return ids->next(); };
  return dependencies;
}

/**
 * @brief Performs the fixture's sole async-signal-safe shutdown action.
 * @param signal_number Delivered signal number, intentionally ignored.
 * @return Nothing.
 * @throws Nothing.
 * @note The handler writes one byte to a pre-created nonblocking pipe and
 *       never allocates, locks, logs, calls Host, or touches filesystem state.
 */
extern "C" void notify_stop(int signal_number) noexcept {
  (void)signal_number;
  const int descriptor = static_cast<int>(g_stop_write_fd);
  if (descriptor < 0) {
    return;
  }
  const int saved_errno = errno;
  const std::uint8_t token = 1;
  const ssize_t ignored = ::write(descriptor, &token, sizeof(token));
  (void)ignored;
  errno = saved_errno;
}

/**
 * @brief Owns the nonblocking close-on-exec fixture shutdown pipe.
 * @throws std::runtime_error if pipe creation or descriptor setup fails.
 * @note The read end is passed to `Server::run`; signal handlers borrow only
 *       the write end while this object remains alive.
 */
class StopPipe final {
 public:
  /**
   * @brief Creates and protects both shutdown-pipe descriptors.
   * @throws std::runtime_error if a POSIX operation fails.
   */
  StopPipe() {
    int descriptors[2] = {-1, -1};
    if (::pipe(descriptors) != 0) {
      throw std::runtime_error("fixture stop-pipe creation failed");
    }
    read_fd_ = descriptors[0];
    write_fd_ = descriptors[1];
    try {
      configure(read_fd_);
      configure(write_fd_);
    } catch (...) {
      close_fd(read_fd_);
      close_fd(write_fd_);
      throw;
    }
  }

  /** @brief Closes both owned descriptors exactly once. @throws Nothing. */
  ~StopPipe() noexcept {
    close_fd(read_fd_);
    close_fd(write_fd_);
  }

  /**
   * @brief Prevents duplicate pipe ownership.
   * @throws Nothing because construction is unavailable.
   */
  StopPipe(const StopPipe&) = delete;

  /**
   * @brief Prevents replacing pipe ownership by copy assignment.
   * @return No value because copying is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  StopPipe& operator=(const StopPipe&) = delete;

  /**
   * @brief Returns the borrowed read descriptor.
   * @return Descriptor value.
   * @throws Nothing.
   */
  int read_fd() const noexcept { return read_fd_; }

  /**
   * @brief Returns the borrowed write descriptor.
   * @return Descriptor value.
   * @throws Nothing.
   */
  int write_fd() const noexcept { return write_fd_; }

 private:
  /**
   * @brief Adds nonblocking and close-on-exec flags to one descriptor.
   * @param descriptor Owned pipe descriptor.
   * @return Nothing.
   * @throws std::runtime_error if `fcntl` fails.
   */
  static void configure(int descriptor) {
    const int status_flags = ::fcntl(descriptor, F_GETFL, 0);
    const int descriptor_flags = ::fcntl(descriptor, F_GETFD, 0);
    if (status_flags < 0 || descriptor_flags < 0 ||
        ::fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK) != 0 ||
        ::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
      throw std::runtime_error("fixture stop-pipe fcntl failed");
    }
  }

  /**
   * @brief Closes one descriptor while containing interrupted closes.
   * @param descriptor Descriptor reset to -1 after the attempt.
   * @return Nothing.
   * @throws Nothing.
   */
  static void close_fd(int& descriptor) noexcept {
    if (descriptor >= 0) {
      while (::close(descriptor) != 0 && errno == EINTR) {
      }
      descriptor = -1;
    }
  }

  /** @brief Owned shutdown-pipe read descriptor. */
  int read_fd_ = -1;

  /** @brief Owned shutdown-pipe write descriptor. */
  int write_fd_ = -1;
};

/**
 * @brief Installs and later restores SIGINT/SIGTERM fixture handlers.
 * @throws std::runtime_error if either handler cannot be installed.
 * @note Destruction first removes the globally borrowed descriptor, then
 *       restores both exact prior actions while the StopPipe is still alive.
 */
class SignalRegistration final {
 public:
  /**
   * @brief Installs shutdown notification handlers.
   * @param write_fd Borrowed nonblocking StopPipe write descriptor.
   * @throws std::runtime_error if `sigaction` fails.
   */
  explicit SignalRegistration(int write_fd) {
    struct sigaction action{};
    action.sa_handler = notify_stop;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    g_stop_write_fd = static_cast<std::sig_atomic_t>(write_fd);
    if (::sigaction(SIGINT, &action, &old_int_) != 0) {
      g_stop_write_fd = -1;
      throw std::runtime_error("fixture SIGINT setup failed");
    }
    int_installed_ = true;
    if (::sigaction(SIGTERM, &action, &old_term_) != 0) {
      (void)::sigaction(SIGINT, &old_int_, nullptr);
      int_installed_ = false;
      g_stop_write_fd = -1;
      throw std::runtime_error("fixture SIGTERM setup failed");
    }
    term_installed_ = true;
  }

  /** @brief Restores prior handlers and clears borrowed state. @throws Nothing.
   */
  ~SignalRegistration() noexcept {
    g_stop_write_fd = -1;
    if (term_installed_) {
      (void)::sigaction(SIGTERM, &old_term_, nullptr);
    }
    if (int_installed_) {
      (void)::sigaction(SIGINT, &old_int_, nullptr);
    }
  }

  /**
   * @brief Prevents duplicate signal-restoration ownership.
   * @throws Nothing because construction is unavailable.
   */
  SignalRegistration(const SignalRegistration&) = delete;

  /**
   * @brief Prevents replacing signal-restoration ownership by assignment.
   * @return No value because copying is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  SignalRegistration& operator=(const SignalRegistration&) = delete;

 private:
  /** @brief Prior SIGINT action restored at destruction. */
  struct sigaction old_int_{};

  /** @brief Prior SIGTERM action restored at destruction. */
  struct sigaction old_term_{};

  /** @brief Whether the SIGINT action must be restored. */
  bool int_installed_ = false;

  /** @brief Whether the SIGTERM action must be restored. */
  bool term_installed_ = false;
};

/**
 * @brief Parses the small deterministic fixture command line.
 * @param argc Process argument count.
 * @param argv Process argument vector.
 * @param options Receives complete validated policy on success.
 * @param message Receives one diagnostic on failure.
 * @return True only for one absolute socket and known image mode.
 * @throws std::bad_alloc if copied argument or diagnostic storage fails.
 */
bool parse_options(int argc, char** argv, Options* options,
                   std::string* message) {
  if (options == nullptr || message == nullptr) {
    return false;
  }
  Options candidate;
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--socket" && index + 1 < argc) {
      candidate.socket_path = argv[++index];
      continue;
    }
    if (argument == "--image-mode" && index + 1 < argc) {
      const std::string value(argv[++index]);
      if (value == "empty") {
        candidate.image_mode = ImageMode::Empty;
      } else if (value == "nonempty") {
        candidate.image_mode = ImageMode::Nonempty;
      } else if (value == "invalid") {
        candidate.image_mode = ImageMode::Invalid;
      } else {
        *message = "image mode must be empty, nonempty, or invalid";
        return false;
      }
      continue;
    }
    if (argument == "--clock-control" && index + 1 < argc) {
      candidate.clock_control_path = argv[++index];
      continue;
    }
    *message = "unknown or incomplete fixture option: " + argument;
    return false;
  }
  if (candidate.socket_path.empty() || candidate.socket_path.front() != '/') {
    *message = "fixture socket path must be absolute";
    return false;
  }
  if (candidate.clock_control_path.empty() ||
      candidate.clock_control_path.front() != '/') {
    *message = "fixture clock control path must be absolute";
    return false;
  }
  *options = std::move(candidate);
  return true;
}

/**
 * @brief Builds the configured deterministic Host image value.
 * @param mode Requested successful or malformed image behavior.
 * @return Owned ImageBuffer whose payload outlives every copied Host result.
 * @throws std::bad_alloc when the four-byte payload cannot be allocated.
 */
ps::ImageBuffer make_image(ImageMode mode) {
  if (mode == ImageMode::Empty) {
    return {};
  }
  if (mode == ImageMode::Invalid) {
    ps::ImageBuffer invalid;
    invalid.width = 1;
    return invalid;
  }
  auto storage = std::make_shared<std::vector<std::uint8_t>>(
      std::initializer_list<std::uint8_t>{1, 2, 3, 4});
  ps::ImageBuffer image;
  image.width = 2;
  image.height = 2;
  image.channels = 1;
  image.type = ps::DataType::UINT8;
  image.device = ps::Device::CPU;
  image.step = 2;
  image.data = std::shared_ptr<void>(storage, storage->data());
  return image;
}

/**
 * @brief Configures deterministic bounded observation pages on the test Host.
 * @param host Fixture Host that will serve real Server client workers.
 * @return Nothing.
 * @throws std::bad_alloc if copied event text or page storage cannot allocate.
 * @throws std::invalid_argument if `host` is null.
 * @note Two destructive event batches are consumed globally across clients,
 *       while the trace page is copied unchanged for every client. This seam
 *       exercises production router/server concurrency without adding a wire
 *       method, product flag, or backend publication failpoint.
 */
void configure_observations(ps::testing::IpcHostSpy* host) {
  if (host == nullptr) {
    throw std::invalid_argument("fixture observation Host is null");
  }
  ps::ComputeEventBatch first;
  first.events = {
      ps::ComputeEventSnapshot{1, ps::NodeId{1}, "first", "fixture", 1.0}};
  first.next_sequence = 2;
  first.has_more = true;
  first.dropped_count = 1;
  ps::ComputeEventBatch second;
  second.events = {
      ps::ComputeEventSnapshot{2, ps::NodeId{2}, "second", "fixture", 2.0}};
  second.next_sequence = 3;
  host->set_compute_event_batches({std::move(first), std::move(second)});

  ps::SchedulerTracePage trace;
  trace.events = {
      ps::SchedulerTraceEventSnapshot{
          1, 11, ps::NodeId{-1}, -1,
          ps::HostSchedulerTraceAction::AssignInitial, 101},
      ps::SchedulerTraceEventSnapshot{
          2, 12, ps::NodeId{2}, 3, ps::HostSchedulerTraceAction::Execute, 102}};
  trace.next_sequence = 2;
  host->set_scheduler_trace_page(std::move(trace));
}

}  // namespace

/**
 * @brief Runs a real internal IPC server around one deterministic test Host.
 * @param argc Process argument count.
 * @param argv Process argument vector.
 * @return Zero after graceful cleanup; nonzero for invalid options or failure.
 * @throws Nothing; all exceptions become bounded stderr diagnostics and exit 1.
 * @note This non-installed test fixture exercises the production
 *       Server/RequestRouter/OutputStore/socket/worker stack. It is not the
 *       `photospiderd` product binary and does not alter product startup or
 *       plugin-seeding behavior.
 */
int main(int argc, char** argv) {
  try {
    Options options;
    std::string message;
    if (!parse_options(argc, argv, &options, &message)) {
      std::cerr << "ipc_output_fixture_daemon: " << message << '\n';
      return 2;
    }
    StopPipe stop_pipe;
    SignalRegistration signals(stop_pipe.write_fd());
    ps::testing::IpcHostSpy host;
    host.set_compute_image(make_image(options.image_mode));
    configure_observations(&host);
    auto clock = std::make_shared<ControlFileClock>(options.clock_control_path);
    auto ids = std::make_shared<DeterministicIdSource>();
    ps::ipc::internal::Server server(host, "output-fixture-v1",
                                     fixture_dependencies(clock, ids));
    const ps::OperationStatus status =
        server.run(ps::ipc::internal::ServerOptions{options.socket_path},
                   stop_pipe.read_fd());
    if (!status.ok) {
      std::cerr << "ipc_output_fixture_daemon: " << status.message << '\n';
      return 1;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ipc_output_fixture_daemon: " << error.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "ipc_output_fixture_daemon: unknown failure\n";
    return 1;
  }
}
