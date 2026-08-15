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
 * The Boolean fields are transient test state rather than counters. No field
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
  /**
   * @brief Reports whether a post-shutdown acceptance delay remains armed.
   * @note False is the default and the post-successful-`SHUT_WR` consumer
   * clears the one-shot state before delaying. This observation grants no
   * channel, deadline, or process authority.
   */
  bool request_shutdown_acceptance_delay_armed = false;
  /**
   * @brief Reports whether a post-transfer-acceptance delay remains armed.
   * @note False is the default and the successful transfer consumer clears
   * the one-shot state before delaying. This observation grants no channel,
   * deadline, lifecycle, or process authority.
   */
  bool request_transfer_post_acceptance_delay_armed = false;
  /**
   * @brief Reports whether a response-channel observation overflow is armed.
   * @note False is the default. The next supervised invocation consumes the
   * one-shot state before any fallible Host preparation, so an early failure
   * cannot perturb a later invocation. This observation grants no channel,
   * clock, deadline, PID, or cleanup authority.
   */
  bool response_channel_observation_overflow_armed = false;
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
   * @brief Exercises the production checked supervisor-deadline derivation.
   * @param base Exact synthetic monotonic base captured by the maintained test.
   * @param duration Candidate positive bounded supervisor duration.
   * @return Exact `base + duration` deadline when representable.
   * @throws std::invalid_argument when `duration` is outside the construction
   * domain.
   * @throws std::overflow_error when the exact sum exceeds the monotonic clock
   * range.
   * @throws std::bad_alloc when constructing a rejection diagnostic exhausts
   * memory.
   * @note This source-private seam samples no real clock and delegates to the
   * same helper used by every production supervisor deadline derivation. It
   * grants no clock, deadline, child, descriptor, or lifecycle authority.
   */
  static std::chrono::steady_clock::time_point
  checked_supervisor_deadline_for_test(
      std::chrono::steady_clock::time_point base,
      std::chrono::milliseconds duration);

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
   * @brief Delays the next successful request-shutdown acceptance once.
   * @param delay Nonnegative process-local delay applied after successful
   * `shutdown(SHUT_WR)` and before request-transfer deadline acceptance.
   * @return Nothing after publishing the next-shutdown perturbation.
   * @throws std::invalid_argument when `delay` is negative.
   * @note This maintained deadline-test seam is disabled by default, affects
   * no already-accepted shutdown, and grants no PID, descriptor, mapping,
   * lifecycle, or clock authority. The production helper consumes it only
   * after successful write-half shutdown; callers must clear an unconsumed
   * delay after an earlier exceptional path before starting another call.
   */
  static void delay_next_supervised_request_shutdown_acceptance(
      std::chrono::milliseconds delay);

  /**
   * @brief Delays Host continuation after the next accepted request transfer.
   * @param delay Nonnegative process-local delay consumed exactly once after
   * successful `SHUT_WR` and its same-deadline acceptance observation, but
   * before the send helper returns to the supervisor caller.
   * @return Nothing after publishing the next post-acceptance perturbation.
   * @throws std::invalid_argument when `delay` is negative.
   * @note This maintained deadline-test seam is disabled by default and
   * deterministically models supervisor descheduling after the transfer
   * acceptance linearization point. It grants no PID, descriptor, mapping,
   * lifecycle, deadline, or clock authority. Callers must clear an unconsumed
   * delay after an earlier exceptional path before starting another call.
   */
  static void delay_next_supervised_request_transfer_post_acceptance(
      std::chrono::milliseconds delay);

  /**
   * @brief Forces one owned response-channel observation deadline to overflow.
   * @param enabled True to arm the next invocation, false to clear the seam.
   * @return Nothing after publishing the process-local one-shot state.
   * @throws Nothing.
   * @note When armed, the next invocation consumes the state before Host
   * preparation. If it reaches the response phase, the production owner
   * replaces its control socket with an owned `/dev/null` descriptor so the
   * ordinary receiver observes a real `recvmsg` `ENOTSOCK`, then supplies
   * `time_point::max()` as the captured base for that channel error's short
   * exact-status observation. This source-private seam is disabled by default,
   * is not installed, and grants no channel, clock, deadline, PID, wait, or
   * cleanup authority.
   */
  static void force_next_response_channel_observation_overflow(
      bool enabled) noexcept;

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
