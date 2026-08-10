/**
 * @file worker_process_launch.hpp
 * @brief Defines the closed exec-bootstrap contract for one Issue #100 worker.
 */
#pragma once

#include <charconv>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace ps::server {

/** @brief Exact control-descriptor argument prefix shared across exec. */
constexpr std::string_view kWorkerControlFdPrefix{"--control-fd="};
/** @brief Exact startup-deadline argument prefix shared across exec. */
constexpr std::string_view kWorkerStartupTimeoutPrefix{"--startup-timeout-ms="};
/** @brief Exact per-message I/O-deadline argument prefix shared across exec. */
constexpr std::string_view kWorkerIoTimeoutPrefix{"--io-timeout-ms="};

/**
 * @brief Immutable manager policy required before assignment-frame receipt.
 * @throws Nothing for value construction.
 * @note These values are exec-bootstrap configuration, not worker-protocol
 * payload fields. The worker needs the startup bound before it can receive the
 * first protocol frame.
 */
struct WorkerProcessLaunchOptions final {
  /** @brief Exact private manager/worker control descriptor. */
  int control_fd = -1;
  /** @brief Manager-selected complete assignment-receive bound. */
  std::chrono::milliseconds startup_timeout{0};
  /** @brief Manager-selected bound for each worker-to-manager frame write. */
  std::chrono::milliseconds io_timeout{0};
};

/**
 * @brief Owns the three complete argument strings retained across `fork`.
 * @throws Nothing for default construction; string mutation may allocate.
 * @note WorkerManager constructs this storage before `fork`, then the child
 * only borrows the stable character arrays for `execv`.
 */
struct WorkerProcessLaunchArguments final {
  /** @brief Complete `--control-fd` argument. */
  std::string control_fd;
  /** @brief Complete `--startup-timeout-ms` argument. */
  std::string startup_timeout;
  /** @brief Complete `--io-timeout-ms` argument. */
  std::string io_timeout;
};

/**
 * @brief Builds the exact required worker exec argument storage.
 * @param options Valid manager-selected launch policy.
 * @return Three stable complete argument strings in parser order.
 * @throws std::invalid_argument for a standard/reserved descriptor or a
 * non-positive duration.
 * @throws std::bad_alloc when retaining argument strings exhausts memory.
 * @note Call only before `fork`; the returned strings must outlive `execv`.
 */
inline WorkerProcessLaunchArguments make_worker_process_launch_arguments(
    const WorkerProcessLaunchOptions& options) {
  if (options.control_fd < 3 || options.control_fd > 1024 * 1024) {
    throw std::invalid_argument("worker control descriptor is out of range");
  }
  if (options.startup_timeout.count() <= 0 || options.io_timeout.count() <= 0) {
    throw std::invalid_argument("worker launch timeouts must be positive");
  }
  WorkerProcessLaunchArguments arguments;
  arguments.control_fd =
      std::string(kWorkerControlFdPrefix) + std::to_string(options.control_fd);
  arguments.startup_timeout = std::string(kWorkerStartupTimeoutPrefix) +
                              std::to_string(options.startup_timeout.count());
  arguments.io_timeout = std::string(kWorkerIoTimeoutPrefix) +
                         std::to_string(options.io_timeout.count());
  return arguments;
}

/**
 * @brief Parses one exact decimal launch argument without allocating.
 * @param argument Complete candidate argument.
 * @param prefix Required exact field prefix.
 * @return Strictly positive signed 64-bit decimal payload.
 * @throws std::invalid_argument for a missing prefix, empty payload, zero,
 * negative value, overflow, or trailing byte.
 * @note A plus sign and surrounding whitespace are intentionally rejected.
 */
inline std::int64_t parse_positive_worker_launch_value(
    std::string_view argument, std::string_view prefix) {
  if (argument.compare(0U, prefix.size(), prefix) != 0) {
    throw std::invalid_argument("worker launch argument prefix is invalid");
  }
  const std::string_view number = argument.substr(prefix.size());
  std::int64_t value = 0;
  const auto parsed =
      std::from_chars(number.data(), number.data() + number.size(), value, 10);
  if (number.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != number.data() + number.size() || value <= 0) {
    throw std::invalid_argument("worker launch argument value is invalid");
  }
  return value;
}

/**
 * @brief Parses the complete closed worker exec-bootstrap argument vector.
 * @param argc Process argument count, which must be exactly four.
 * @param argv Process argument vector in control/startup/I/O order.
 * @return Valid exact control descriptor and manager-selected durations.
 * @throws std::invalid_argument for a missing, extra, reordered, malformed, or
 * out-of-range value.
 * @note No worker-local default or cap exists: accepted duration values equal
 * the manager arguments exactly.
 */
inline WorkerProcessLaunchOptions parse_worker_process_launch_options(
    int argc, char* const argv[]) {
  if (argc != 4 || argv == nullptr || argv[1] == nullptr ||
      argv[2] == nullptr || argv[3] == nullptr) {
    throw std::invalid_argument(
        "worker requires control, startup-timeout, and io-timeout arguments");
  }
  const std::int64_t control =
      parse_positive_worker_launch_value(argv[1], kWorkerControlFdPrefix);
  const std::int64_t startup =
      parse_positive_worker_launch_value(argv[2], kWorkerStartupTimeoutPrefix);
  const std::int64_t io =
      parse_positive_worker_launch_value(argv[3], kWorkerIoTimeoutPrefix);
  if (control < 3 || control > 1024 * 1024) {
    throw std::invalid_argument("worker control descriptor is out of range");
  }
  return WorkerProcessLaunchOptions{static_cast<int>(control),
                                    std::chrono::milliseconds(startup),
                                    std::chrono::milliseconds(io)};
}

}  // namespace ps::server
