/**
 * @file isolated_cpu_invocation_test_probe.hpp
 * @brief Declares read-only observations for maintained invocation tests.
 */
#pragma once

#include <chrono>
#include <cstdint>

namespace ps::execution {

/**
 * @brief Closed lifecycle acceptance points available to maintained tests.
 * @note Values identify only source-private timing perturbations and carry no
 * runtime protocol, session, or process authority.
 */
enum class SupervisedLifecycleTestEvent : std::uint8_t {
  /** @brief Authenticated startup-event acceptance. */
  RuntimeStarted = 0,
  /** @brief Authenticated callback-heartbeat acceptance. */
  Heartbeat = 1,
  /** @brief Authenticated callback-completion acceptance. */
  InvocationCompleted = 2,
};

/**
 * @brief Process-local observations of invocation side effects.
 * @throws Nothing for ordinary value operations.
 * @note Counters are monotonic; the PID field is only the latest observation.
 * The hold field is transient test state rather than a counter. No field
 * grants descriptor, PID, mapping, or cleanup authority.
 */
struct IsolatedCpuInvocationTestSnapshot final {
  /**
   * @brief Host capability materialization attempts before any shm creation.
   */
  std::uint64_t host_capability_materialization_attempts = 0U;
  /** @brief Fresh child processes successfully created by the Host. */
  std::uint64_t spawned_children = 0U;
  /** @brief Exact child PIDs successfully returned by blocking `waitpid`. */
  std::uint64_t reaped_children = 0U;
  /** @brief Most recent exactly reaped PID, or -1 before the first reap. */
  std::int64_t last_reaped_child = -1;
  /** @brief Frames whose exact declared byte length has been received. */
  std::uint64_t exact_frames_received = 0U;
  /** @brief True only while the one-shot post-request exit hold is armed. */
  bool invocation_monitor_exit_hold_armed = false;
};

/**
 * @brief Source-private observation, timing, and framing seam for tests.
 * @throws Nothing for construction because the type has no instances.
 * @note This header is not installed. Snapshot access exposes no authority;
 * timing perturbations are one-shot and process-local, and framing borrows only
 * the caller-supplied test socket for one receive.
 */
class IsolatedCpuInvocationTestProbe final {
 public:
  /**
   * @brief Prevents construction of the static observation seam.
   * @throws Nothing because the operation is deleted.
   */
  IsolatedCpuInvocationTestProbe() = delete;

  /**
   * @brief Reads process-local observations from atomic fields.
   * @return Monotonic counters and the latest exactly reaped child PID.
   * @throws Nothing.
   * @note The snapshot is diagnostic evidence only and cannot reap a child.
   * Fields are not a transactionally coherent view of concurrent invocations;
   * maintained callers compare them around one synchronous invocation.
   */
  static IsolatedCpuInvocationTestSnapshot snapshot() noexcept;

  /**
   * @brief Delays the next supervised request send exactly once.
   * @param delay Nonnegative process-local delay applied before send polling.
   * @return Nothing after publishing the next-send perturbation.
   * @throws std::invalid_argument when `delay` is negative.
   * @note This maintained deadline-test seam is not installed, affects no
   * already-started send, and grants no PID, descriptor, or mapping authority.
   */
  static void delay_next_supervised_request_send(
      std::chrono::milliseconds delay);

  /**
   * @brief Delays one selected lifecycle event immediately before acceptance.
   * @param event Closed authenticated event whose next acceptance is delayed.
   * @param delay Nonnegative process-local delay consumed exactly once.
   * @return Nothing after publishing the selected one-shot perturbation.
   * @throws std::invalid_argument when `delay` is negative or `event` is not a
   * closed enumerator.
   * @note The event has already been received and session-validated when the
   * delay is consumed, so this seam deterministically models supervisor
   * descheduling between receive and the acceptance linearization point. It is
   * not installed and grants no PID, descriptor, mapping, or clock authority.
   */
  static void delay_next_lifecycle_event_acceptance(
      SupervisedLifecycleTestEvent event, std::chrono::milliseconds delay);

  /**
   * @brief Holds the next post-request invocation monitor until child exit.
   * @param enabled True to arm the one-shot hold, false to clear it.
   * @return Nothing after publishing the process-local test state.
   * @throws Nothing.
   * @note When armed, the production owner uses `waitid(WNOWAIT)` after the
   * complete request transfer and deadline construction. The hold ends only
   * after the exact child has exited normally without consuming its wait
   * status, so lifecycle and response bytes remain queued for the production
   * monitor. This source-private seam is disabled by default and grants no
   * PID, wait, descriptor, mapping, lifecycle, or response authority to test
   * code.
   */
  static void hold_next_invocation_monitor_until_child_exit(
      bool enabled) noexcept;

  /**
   * @brief Runs the production one-frame receiver and discards its payload.
   * @param socket Connected blocking Unix stream descriptor borrowed for the
   * call.
   * @return Nothing after one exact frame, its control-free terminating EOF,
   * and retirement of all received FDs.
   * @throws IsolatedCpuProtocolError for malformed framing or ancillary data.
   * @throws IsolatedCpuInvocationError for channel-system failure or early EOF.
   * @throws std::bad_alloc when bounded receive storage cannot allocate.
   * @note This test seam invokes the same receiver used before the runtime
   * callback and does not decode, map, or retain the frame. Control records
   * installed by a zero-payload `recvmsg` are validated before EOF handling.
   */
  static void receive_one_packet(int socket);
};

}  // namespace ps::execution
