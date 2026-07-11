#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "ipc/server.hpp"
#include "ipc/service_version.hpp"
#include "photospider/host/host.hpp"

namespace {

/**
 * @brief Signal-handler-visible self-pipe write descriptor, or -1.
 *
 * @throws Nothing; signal registration updates this scalar directly.
 * @note The value only borrows the live `SelfPipe` write end. It is published
 *       before handler installation and reset before handler restoration, and
 *       the handler performs no ownership transfer or descriptor close.
 */
volatile std::sig_atomic_t g_signal_write_fd = -1;

/**
 * @brief Performs the sole async-signal-safe action for SIGINT/SIGTERM.
 *
 * @param signal_number Delivered signal number, intentionally ignored.
 * @throws Nothing.
 * @note The handler writes one byte to a pre-created nonblocking pipe and does
 *       not allocate, lock, log, call Host, or unlink filesystem paths.
 */
extern "C" void notify_shutdown(int signal_number) noexcept {
  (void)signal_number;
  const int fd = static_cast<int>(g_signal_write_fd);
  if (fd < 0) {
    return;
  }
  const int saved_errno = errno;
  const unsigned char byte = 1;
  const ssize_t ignored = ::write(fd, &byte, sizeof(byte));
  (void)ignored;
  errno = saved_errno;
}

/**
 * @brief Move-disabled owner of the daemon signal self-pipe.
 *
 * @throws std::runtime_error if pipe creation or descriptor configuration
 *         fails.
 * @note Both ends are nonblocking and close-on-exec before any signal handler
 *       is installed.
 */
class SelfPipe {
 public:
  /**
   * @brief Creates and configures the signal self-pipe.
   *
   * @throws std::runtime_error on pipe/fcntl failure.
   */
  SelfPipe() {
    int descriptors[2] = {-1, -1};
    if (::pipe(descriptors) != 0) {
      throw std::runtime_error(std::string("self-pipe creation failed: ") +
                               std::strerror(errno));
    }
    read_fd_ = descriptors[0];
    write_fd_ = descriptors[1];
    try {
      configure(read_fd_);
      configure(write_fd_);
    } catch (...) {
      ::close(read_fd_);
      ::close(write_fd_);
      read_fd_ = -1;
      write_fd_ = -1;
      throw;
    }
  }

  /**
   * @brief Prevents duplicating ownership of the two pipe descriptors.
   *
   * @throws Nothing because this operation is unavailable.
   * @note Exactly one process-shell object closes each end.
   */
  SelfPipe(const SelfPipe&) = delete;

  /**
   * @brief Prevents replacing self-pipe descriptor ownership by copy.
   *
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   * @note Signal registration borrows, rather than owns, the write end.
   */
  SelfPipe& operator=(const SelfPipe&) = delete;

  /**
   * @brief Closes both pipe descriptors once.
   *
   * @throws Nothing.
   */
  ~SelfPipe() {
    if (read_fd_ >= 0) {
      ::close(read_fd_);
    }
    if (write_fd_ >= 0) {
      ::close(write_fd_);
    }
  }

  /**
   * @brief Returns the self-pipe read descriptor for the server poll loop.
   *
   * @return Owned read descriptor.
   * @throws Nothing.
   */
  int read_fd() const noexcept { return read_fd_; }

  /**
   * @brief Returns the self-pipe write descriptor for signal notification.
   *
   * @return Owned write descriptor.
   * @throws Nothing.
   */
  int write_fd() const noexcept { return write_fd_; }

 private:
  /**
   * @brief Adds nonblocking and close-on-exec flags to one pipe descriptor.
   *
   * @param fd Valid pipe descriptor.
   * @throws std::runtime_error if either `fcntl` operation fails.
   */
  static void configure(int fd) {
    const int status_flags = ::fcntl(fd, F_GETFL, 0);
    const int descriptor_flags = ::fcntl(fd, F_GETFD, 0);
    if (status_flags < 0 || descriptor_flags < 0 ||
        ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) != 0 ||
        ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
      throw std::runtime_error(std::string("self-pipe fcntl failed: ") +
                               std::strerror(errno));
    }
  }

  /** @brief Owned read end of the self-pipe. */
  int read_fd_ = -1;

  /** @brief Owned nonblocking write end used by signal handlers. */
  int write_fd_ = -1;
};

/**
 * @brief RAII installation and restoration of SIGINT/SIGTERM handlers.
 *
 * @throws std::runtime_error if either handler cannot be installed.
 * @note Destruction first disables the global write descriptor and then
 *       restores previous handlers while the self-pipe is still alive.
 */
class SignalRegistration {
 public:
  /**
   * @brief Installs self-pipe notification handlers.
   *
   * @param write_fd Pre-created nonblocking self-pipe write descriptor.
   * @throws std::runtime_error if `sigaction` fails.
   */
  explicit SignalRegistration(int write_fd) {
    struct sigaction action{};
    action.sa_handler = notify_shutdown;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    g_signal_write_fd = static_cast<std::sig_atomic_t>(write_fd);
    if (::sigaction(SIGINT, &action, &old_int_) != 0) {
      g_signal_write_fd = -1;
      throw std::runtime_error(std::string("SIGINT handler setup failed: ") +
                               std::strerror(errno));
    }
    int_installed_ = true;
    if (::sigaction(SIGTERM, &action, &old_term_) != 0) {
      (void)::sigaction(SIGINT, &old_int_, nullptr);
      int_installed_ = false;
      g_signal_write_fd = -1;
      throw std::runtime_error(std::string("SIGTERM handler setup failed: ") +
                               std::strerror(errno));
    }
    term_installed_ = true;
  }

  /**
   * @brief Prevents multiple restorers from owning the same prior actions.
   *
   * @throws Nothing because this operation is unavailable.
   * @note One registration covers both SIGINT and SIGTERM.
   */
  SignalRegistration(const SignalRegistration&) = delete;

  /**
   * @brief Prevents overwriting captured signal-restoration state.
   *
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   * @note The process shell installs only one live registration.
   */
  SignalRegistration& operator=(const SignalRegistration&) = delete;

  /**
   * @brief Restores previous process signal handlers.
   *
   * @throws Nothing.
   */
  ~SignalRegistration() {
    g_signal_write_fd = -1;
    if (term_installed_) {
      (void)::sigaction(SIGTERM, &old_term_, nullptr);
    }
    if (int_installed_) {
      (void)::sigaction(SIGINT, &old_int_, nullptr);
    }
  }

 private:
  /** @brief Previous SIGINT action restored at destruction. */
  struct sigaction old_int_{};

  /** @brief Previous SIGTERM action restored at destruction. */
  struct sigaction old_term_{};

  /** @brief Whether SIGINT installation succeeded. */
  bool int_installed_ = false;

  /** @brief Whether SIGTERM installation succeeded. */
  bool term_installed_ = false;
};

/**
 * @brief Parsed foreground daemon command-line options.
 *
 * @throws std::bad_alloc when socket path storage cannot be allocated.
 */
struct CommandLineOptions {
  /** @brief Explicit socket path, or empty for the per-user default. */
  std::string socket_path;

  /** @brief Whether help text was requested. */
  bool show_help = false;
};

/**
 * @brief Parses the intentionally small foreground daemon option surface.
 *
 * @param argc Process argument count.
 * @param argv Process argument vector.
 * @param options Receives parsed `--socket PATH` or `--help` state.
 * @param message Receives an unknown/missing-argument diagnostic.
 * @return True when arguments are valid.
 * @throws std::bad_alloc if copied strings cannot be allocated.
 * @note No background/fork, TCP, shutdown method, or graph_cli remote option is
 *       accepted by this process shell.
 */
bool parse_options(int argc, char** argv, CommandLineOptions* options,
                   std::string* message) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      options->show_help = true;
      continue;
    }
    if (argument == "--socket") {
      if (index + 1 >= argc) {
        *message = "--socket requires an absolute PATH";
        return false;
      }
      options->socket_path = argv[++index];
      continue;
    }
    *message = "unknown photospiderd option: " + argument;
    return false;
  }
  return true;
}

/**
 * @brief Prints stable foreground daemon usage text.
 *
 * @throws Nothing except standard stream failures configured by the caller.
 */
void print_usage() {
  std::cout
      << "Usage: photospiderd [--socket ABSOLUTE_PATH]\n"
         "Runs the version 1 local Unix socket daemon in the foreground.\n";
}

}  // namespace

/**
 * @brief Constructs one embedded Host and runs the foreground Unix daemon.
 *
 * @param argc Process argument count.
 * @param argv Process argument vector.
 * @return Zero after help or graceful SIGINT/SIGTERM cleanup; nonzero for
 *         invalid options, startup failures, or unexpected fatal exceptions.
 * @throws Nothing; fatal exceptions are diagnosed and converted to exit 1.
 * @note The self-pipe exists before signal handlers, and Host outlives server
 *       shutdown/session cleanup.
 */
int main(int argc, char** argv) {
  try {
    CommandLineOptions options;
    std::string message;
    if (!parse_options(argc, argv, &options, &message)) {
      std::cerr << "photospiderd: " << message << '\n';
      return 2;
    }
    if (options.show_help) {
      print_usage();
      return 0;
    }

    SelfPipe self_pipe;
    SignalRegistration signals(self_pipe.write_fd());
    std::unique_ptr<ps::Host> host = ps::create_embedded_host();
    if (!host) {
      std::cerr << "photospiderd: embedded Host creation returned null\n";
      return 1;
    }
    ps::ipc::internal::Server server(*host, ps::ipc::internal::kServiceVersion);
    const ps::ipc::IpcStatus status =
        server.run(ps::ipc::internal::ServerOptions{options.socket_path},
                   self_pipe.read_fd());
    if (!status.ok) {
      std::cerr << "photospiderd: " << status.message << '\n';
      return 1;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "photospiderd: fatal startup failure: " << error.what()
              << '\n';
    return 1;
  } catch (...) {
    std::cerr << "photospiderd: fatal unknown startup failure\n";
    return 1;
  }
}
