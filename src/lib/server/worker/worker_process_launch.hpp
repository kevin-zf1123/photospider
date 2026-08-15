/**
 * @file worker_process_launch.hpp
 * @brief Defines the closed exec-bootstrap contract for one Issue #105 worker.
 */
#pragma once

#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace ps::server {

/** @brief Exact control-descriptor argument prefix shared across exec. */
constexpr std::string_view kWorkerControlFdPrefix{"--control-fd="};
/** @brief Exact read-only checkpoint data descriptor argument prefix. */
constexpr std::string_view kWorkerCheckpointDataFdPrefix{
    "--checkpoint-data-fd="};  // NOLINT(whitespace/indent_namespace)
/** @brief Exact write-only output-stage descriptor argument prefix. */
constexpr std::string_view kWorkerOutputDataFdPrefix{"--output-data-fd="};
/** @brief Exact startup-deadline argument prefix shared across exec. */
constexpr std::string_view kWorkerStartupTimeoutPrefix{"--startup-timeout-ms="};
/** @brief Exact per-message I/O-deadline argument prefix shared across exec. */
constexpr std::string_view kWorkerIoTimeoutPrefix{"--io-timeout-ms="};
/**
 * @brief Inclusive upper bound for every configured worker duration.
 * @note The bound matches the protocol's unsigned 32-bit heartbeat-cadence
 * field and is exactly representable by the supported Darwin/Linux monotonic
 * clocks. It is a lifecycle-policy limit, not a wire-version change.
 */
constexpr std::chrono::milliseconds kMaximumWorkerDuration{
    static_cast<std::chrono::milliseconds::rep>(
        std::numeric_limits<std::uint32_t>::max())};
/**
 * @brief Inclusive heartbeat-interval bound with room for a larger timeout.
 * @note Manager configuration additionally requires the interval to be
 * strictly less than `heartbeat_timeout`.
 */
// NOLINTBEGIN(whitespace/indent_namespace)
constexpr std::chrono::milliseconds kMaximumWorkerHeartbeatInterval =
    kMaximumWorkerDuration - std::chrono::milliseconds{1};

/** @brief Exact monotonic-clock ticks in one millisecond. */
constexpr auto kWorkerClockTicksPerMillisecond =
    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::milliseconds{1})
        .count();
static_assert(
    kWorkerClockTicksPerMillisecond > 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::duration{
                kWorkerClockTicksPerMillisecond}) ==
            std::chrono::milliseconds{1},
    "supported worker supervision clocks must exactly represent milliseconds");
static_assert(
    std::numeric_limits<std::chrono::steady_clock::duration::rep>::is_integer,
    "supported worker supervision clocks must use integer ticks");
static_assert(
    kMaximumWorkerDuration.count() <=
        std::numeric_limits<std::chrono::steady_clock::duration::rep>::max() /
            kWorkerClockTicksPerMillisecond,
    "the shared worker duration bound must fit the monotonic clock");
// NOLINTEND

/**
 * @brief Validates and exactly converts one worker lifecycle duration.
 * @param duration Candidate positive millisecond duration.
 * @param maximum Inclusive field-specific maximum no greater than the shared
 * worker-duration bound.
 * @param field_name Nonempty trusted configuration field name for diagnostics.
 * @return Exact monotonic-clock duration for safe deadline arithmetic.
 * @throws std::invalid_argument when the field name, maximum, or candidate is
 * outside the closed supported domain.
 * @throws std::bad_alloc when constructing a rejection diagnostic exhausts
 * memory.
 * @note Validation precedes `duration_cast`; compile-time scale checks prove
 * that every admitted millisecond value converts without integer overflow or
 * truncation on supported platforms.
 */
inline std::chrono::steady_clock::duration validate_and_convert_worker_duration(
    std::chrono::milliseconds duration, std::chrono::milliseconds maximum,
    std::string_view field_name) {
  if (field_name.empty() || maximum.count() <= 0 ||
      maximum > kMaximumWorkerDuration) {
    throw std::invalid_argument("worker duration validation bound is invalid");
  }
  if (duration.count() <= 0 || duration > maximum) {
    throw std::invalid_argument(
        std::string(field_name) + " must be between 1 and " +
        std::to_string(maximum.count()) + " milliseconds");
  }
  return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      duration);
}

/**
 * @brief Adds one validated worker duration to one captured monotonic base.
 * @param base Exact base captured once by the caller.
 * @param duration Candidate positive worker duration within the shared bound.
 * @return Exact absolute monotonic deadline.
 * @throws std::invalid_argument when `duration` is outside the shared bound.
 * @throws std::overflow_error when the exact sum exceeds the clock range.
 * @throws std::bad_alloc when constructing a rejection diagnostic exhausts
 * memory.
 * @note The range check and addition use the same caller-provided `base`, so a
 * second clock observation cannot invalidate the proof.
 */
inline std::chrono::steady_clock::time_point checked_worker_deadline(
    std::chrono::steady_clock::time_point base,
    std::chrono::milliseconds duration) {
  const std::chrono::steady_clock::duration increment =
      validate_and_convert_worker_duration(duration, kMaximumWorkerDuration,
                                           "worker deadline duration");
  const auto latest_base =
      std::chrono::steady_clock::time_point::max() - increment;
  if (base > latest_base) {
    throw std::overflow_error("worker deadline exceeds monotonic clock range");
  }
  return base + increment;
}

/**
 * @brief Immutable manager policy required before assignment-frame receipt.
 * @throws Nothing for value construction.
 * @note These values are exec-bootstrap configuration, not worker-control
 * payload fields. The data descriptors are direction-scoped OS capabilities;
 * their numbers never enter JobSpec or an Assignment/Report frame. The worker
 * needs the startup bound before it can receive the first protocol frame.
 */
struct WorkerProcessLaunchOptions final {
  /** @brief Exact private manager/worker control descriptor. */
  int control_fd = -1;
  /** @brief Exact inherited read-only checkpoint data-plane descriptor. */
  int checkpoint_data_fd = -1;
  /** @brief Exact inherited write-only output-stage data-plane descriptor. */
  int output_data_fd = -1;
  /** @brief Manager-selected bounded complete assignment-receive duration. */
  std::chrono::milliseconds startup_timeout{0};
  /** @brief Manager-selected bounded worker-to-manager frame-write duration. */
  std::chrono::milliseconds io_timeout{0};
};

/**
 * @brief Owns the five complete argument strings retained across `fork`.
 * @throws Nothing for default construction; string mutation may allocate.
 * @note WorkerManager constructs this storage before `fork`, then the child
 * only borrows the stable character arrays for `execv`.
 */
struct WorkerProcessLaunchArguments final {
  /** @brief Complete `--control-fd` argument. */
  std::string control_fd;
  /** @brief Complete `--checkpoint-data-fd` argument. */
  std::string checkpoint_data_fd;
  /** @brief Complete `--output-data-fd` argument. */
  std::string output_data_fd;
  /** @brief Complete `--startup-timeout-ms` argument. */
  std::string startup_timeout;
  /** @brief Complete `--io-timeout-ms` argument. */
  std::string io_timeout;
};

/**
 * @brief Builds the exact required worker exec argument storage.
 * @param options Valid manager-selected launch policy.
 * @return Five stable complete argument strings in parser order.
 * @throws std::invalid_argument for a standard/reserved/duplicate descriptor
 * or a duration outside the shared closed bound.
 * @throws std::bad_alloc when retaining argument strings exhausts memory.
 * @note Call only before `fork`; the returned strings must outlive `execv`.
 */
inline WorkerProcessLaunchArguments make_worker_process_launch_arguments(
    const WorkerProcessLaunchOptions& options) {
  constexpr int kMaximumLaunchDescriptor = 1024 * 1024;
  if (options.control_fd < 3 || options.control_fd > kMaximumLaunchDescriptor ||
      options.checkpoint_data_fd < 3 ||
      options.checkpoint_data_fd > kMaximumLaunchDescriptor ||
      options.output_data_fd < 3 ||
      options.output_data_fd > kMaximumLaunchDescriptor ||
      options.control_fd == options.checkpoint_data_fd ||
      options.control_fd == options.output_data_fd ||
      options.checkpoint_data_fd == options.output_data_fd) {
    throw std::invalid_argument(
        "worker launch descriptors are invalid or not distinct");
  }
  static_cast<void>(validate_and_convert_worker_duration(
      options.startup_timeout, kMaximumWorkerDuration, "startup_timeout"));
  static_cast<void>(validate_and_convert_worker_duration(
      options.io_timeout, kMaximumWorkerDuration, "io_timeout"));
  WorkerProcessLaunchArguments arguments;
  arguments.control_fd =
      std::string(kWorkerControlFdPrefix) + std::to_string(options.control_fd);
  arguments.checkpoint_data_fd = std::string(kWorkerCheckpointDataFdPrefix) +
                                 std::to_string(options.checkpoint_data_fd);
  arguments.output_data_fd = std::string(kWorkerOutputDataFdPrefix) +
                             std::to_string(options.output_data_fd);
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
 * @param argc Process argument count, which must be exactly six.
 * @param argv Process argument vector in control/checkpoint/output/startup/I/O
 * order.
 * @return Valid exact direction-scoped descriptors and manager durations.
 * @throws std::invalid_argument for a missing, extra, reordered, malformed, or
 * out-of-range value.
 * @throws std::bad_alloc when retaining a parse or duration-rejection
 * diagnostic exhausts memory.
 * @note No worker-local default exists: accepted duration values equal the
 * manager arguments exactly within the shared closed bound. The argument
 * vector and its strings are borrowed only for this call; the returned value
 * owns every parsed scalar.
 */
inline WorkerProcessLaunchOptions parse_worker_process_launch_options(
    int argc, char* const argv[]) {
  if (argc != 6 || argv == nullptr || argv[1] == nullptr ||
      argv[2] == nullptr || argv[3] == nullptr || argv[4] == nullptr ||
      argv[5] == nullptr) {
    throw std::invalid_argument(
        "worker requires control, checkpoint, output, startup, and I/O "
        "arguments");
  }
  const std::int64_t control =
      parse_positive_worker_launch_value(argv[1], kWorkerControlFdPrefix);
  const std::int64_t checkpoint = parse_positive_worker_launch_value(
      argv[2], kWorkerCheckpointDataFdPrefix);
  const std::int64_t output =
      parse_positive_worker_launch_value(argv[3], kWorkerOutputDataFdPrefix);
  const std::int64_t startup =
      parse_positive_worker_launch_value(argv[4], kWorkerStartupTimeoutPrefix);
  const std::int64_t io =
      parse_positive_worker_launch_value(argv[5], kWorkerIoTimeoutPrefix);
  constexpr std::int64_t kMaximumLaunchDescriptor = 1024 * 1024;
  if (control < 3 || control > kMaximumLaunchDescriptor || checkpoint < 3 ||
      checkpoint > kMaximumLaunchDescriptor || output < 3 ||
      output > kMaximumLaunchDescriptor || control == checkpoint ||
      control == output || checkpoint == output) {
    throw std::invalid_argument(
        "worker launch descriptors are invalid or not distinct");
  }
  const std::chrono::milliseconds startup_timeout(startup);
  const std::chrono::milliseconds io_timeout(io);
  static_cast<void>(validate_and_convert_worker_duration(
      startup_timeout, kMaximumWorkerDuration, "startup_timeout"));
  static_cast<void>(validate_and_convert_worker_duration(
      io_timeout, kMaximumWorkerDuration, "io_timeout"));
  return WorkerProcessLaunchOptions{
      static_cast<int>(control), static_cast<int>(checkpoint),
      static_cast<int>(output), startup_timeout, io_timeout};
}

}  // namespace ps::server
