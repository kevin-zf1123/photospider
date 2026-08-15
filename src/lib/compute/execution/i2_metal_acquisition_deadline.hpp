/**
 * @file i2_metal_acquisition_deadline.hpp
 * @brief Defines source-private I2 Metal deadline and timeout containment.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <thread>

#include "execution/device/device_completion.hpp"
#include "execution/device/residency_manager.hpp"
#include "photospider/data/value.hpp"
#include "photospider/memory/ready_fence.hpp"

namespace ps::compute {

/** @brief Fixed polling cadence inside one caller-owned absolute deadline. */
inline constexpr std::chrono::microseconds kI2MetalCompletionPollInterval{50};

/**
 * @brief Classifies exact ownership containment after an I2 Metal timeout.
 * @throws Nothing for construction and comparison.
 * @note Every value denies further publication through the timed-out
 * acquisition. Native command completion remains owned by its sole callback
 * and releases callback-retained leases only when that command terminates.
 */
enum class I2TimedOutTransferContainment : std::uint8_t {
  /** @brief The exact pending manager admission was removed. */
  PendingAdmissionDiscarded,
  /** @brief A Ready publication won the race and its resident was released. */
  ReadyResidentReleased,
  /** @brief Completion was already terminal and retained no exact resident. */
  TerminalWithoutResident,
  /** @brief Completion removed admission and is settling its sole fence. */
  CompletionOwnerSettling,
};

namespace detail {

/**
 * @brief Polls one already-published Metal resident with a deterministic hook.
 * @tparam BeforePoll Nullary verification hook invoked after the open-deadline
 * precheck and immediately before the single fence observation.
 * @tparam Clock Nullary callable returning `steady_clock::time_point`.
 * @param fence Exact Ready resident fence borrowed from process residency.
 * @param capture_deadline Exclusive absolute I2 capture deadline.
 * @param before_poll Source-private precheck-to-poll interleave hook.
 * @param clock Monotonic source sampled around the fence observation.
 * @return The observed snapshot only when its fresh post-poll sample is
 * strictly before the deadline; otherwise `std::nullopt` at a tie or later.
 * @throws std::logic_error for an invalid fence.
 * @throws BeforePoll or Clock failures unchanged.
 * @note The helper performs exactly one poll and owns no Value, resident,
 * executor submission, transfer, release, ledger, or native authority. A
 * timeout therefore leaves the existing resident untouched for its row-scoped
 * owner. Production delegates with a no-op hook; deterministic tests place
 * deadline crossing exactly between the precheck and poll.
 */
template <typename BeforePoll, typename Clock>
std::optional<ReadyFenceSnapshot>
poll_i2_metal_resident_reuse_with_pre_poll_hook(
    const ReadyFence& fence,
    std::chrono::steady_clock::time_point capture_deadline,
    BeforePoll&& before_poll, Clock&& clock) {
  if (clock() >= capture_deadline) {
    return std::nullopt;
  }
  before_poll();
  const ReadyFenceSnapshot observed = fence.poll();
  if (clock() >= capture_deadline) {
    return std::nullopt;
  }
  return observed;
}

/**
 * @brief Waits for one Metal fence with a deterministic pre-poll test hook.
 * @tparam BeforePoll Nullary verification hook invoked only after an open
 * deadline precheck and immediately before polling.
 * @tparam Clock Nullary callable returning `steady_clock::time_point`.
 * @tparam SleepUntil Callable accepting one `steady_clock::time_point`.
 * @param fence Exact pending or terminal fence to observe.
 * @param capture_deadline Exclusive absolute I2 capture deadline.
 * @param before_poll Source-private interleave hook.
 * @param clock Monotonic source sampled immediately before and after each
 * fence observation.
 * @param sleep_until Bounded sleeper used only between polls.
 * @return Terminal snapshot whose fresh post-poll sample is strictly before
 * the deadline, otherwise `std::nullopt` even for an exact terminal tie.
 * @throws std::logic_error for an invalid fence.
 * @throws BeforePoll, Clock, or sleeper failures unchanged.
 * @note The function owns no producer, Value, transfer, residency, ledger, or
 * native cancellation authority. It checks the exclusive deadline before
 * every fence observation, rejects every terminal when the immediate
 * post-poll sample reaches the deadline, and never sleeps past it. Production
 * delegates with a no-op hook; only deterministic source-private tests use the
 * hook to place a real terminal transition in the precheck-to-poll interleave.
 */
template <typename BeforePoll, typename Clock, typename SleepUntil>
std::optional<ReadyFenceSnapshot>
wait_for_i2_metal_completion_until_with_pre_poll_hook(
    const ReadyFence& fence,
    std::chrono::steady_clock::time_point capture_deadline,
    BeforePoll&& before_poll, Clock&& clock, SleepUntil&& sleep_until) {
  while (true) {
    const std::chrono::steady_clock::time_point pre_poll_at = clock();
    if (pre_poll_at >= capture_deadline) {
      return std::nullopt;
    }
    before_poll();
    const ReadyFenceSnapshot observed = fence.poll();
    const std::chrono::steady_clock::time_point post_poll_at = clock();
    if (post_poll_at >= capture_deadline) {
      return std::nullopt;
    }
    if (observed.terminal()) {
      return observed;
    }
    const auto remaining = capture_deadline - post_poll_at;
    const std::chrono::steady_clock::time_point wake_at =
        remaining <= kI2MetalCompletionPollInterval
            ? capture_deadline
            : post_poll_at + kI2MetalCompletionPollInterval;
    sleep_until(wake_at);
  }
}

}  // namespace detail

/**
 * @brief Polls one resident fence under an injected monotonic clock.
 * @tparam Clock Nullary callable returning `steady_clock::time_point`.
 * @param fence Exact already-published resident fence.
 * @param capture_deadline Exclusive absolute I2 capture deadline.
 * @param clock Monotonic source sampled around the single fence observation.
 * @return Snapshot accepted only when its fresh post-poll sample is strictly
 * before the deadline, otherwise `std::nullopt`.
 * @throws std::logic_error for an invalid fence.
 * @throws Clock failures unchanged.
 * @note This overload delegates with a no-op test hook and grants no resident
 * release, transfer, executor, or ownership authority.
 */
template <typename Clock>
std::optional<ReadyFenceSnapshot> poll_i2_metal_resident_reuse(
    const ReadyFence& fence,
    std::chrono::steady_clock::time_point capture_deadline, Clock&& clock) {
  return detail::poll_i2_metal_resident_reuse_with_pre_poll_hook(
      fence, capture_deadline, [] {}, clock);
}

/**
 * @brief Polls one resident fence with the process monotonic clock.
 * @param fence Exact already-published resident fence.
 * @param capture_deadline Exclusive absolute I2 capture deadline.
 * @return Snapshot accepted only when its fresh post-poll sample is strictly
 * before the deadline, otherwise `std::nullopt`.
 * @throws std::logic_error for an invalid fence.
 * @note The helper performs no wait, retry, transfer, release, or budget
 * refresh; equality with the unchanged deadline is late.
 */
inline std::optional<ReadyFenceSnapshot> poll_i2_metal_resident_reuse(
    const ReadyFence& fence,
    std::chrono::steady_clock::time_point capture_deadline) {
  return poll_i2_metal_resident_reuse(
      fence, capture_deadline, [] { return std::chrono::steady_clock::now(); });
}

/**
 * @brief Waits for one Metal fence only while an absolute deadline is open.
 * @tparam Clock Nullary callable returning `steady_clock::time_point`.
 * @tparam SleepUntil Callable accepting one `steady_clock::time_point`.
 * @param fence Exact pending or terminal fence to observe.
 * @param capture_deadline Exclusive absolute I2 capture deadline.
 * @param clock Monotonic source sampled immediately before and after each
 * fence observation.
 * @param sleep_until Bounded sleeper used only between polls.
 * @return Terminal snapshot whose fresh post-poll sample is strictly before
 * the deadline, otherwise `std::nullopt` even for an exact terminal tie.
 * @throws std::logic_error for an invalid fence.
 * @throws Clock or sleeper failures unchanged.
 * @note This production form delegates with a no-op pre-poll hook and grants
 * no producer, Value, transfer, residency, ledger, or cancellation authority.
 */
template <typename Clock, typename SleepUntil>
std::optional<ReadyFenceSnapshot> wait_for_i2_metal_completion_until(
    const ReadyFence& fence,
    std::chrono::steady_clock::time_point capture_deadline, Clock&& clock,
    SleepUntil&& sleep_until) {
  return detail::wait_for_i2_metal_completion_until_with_pre_poll_hook(
      fence, capture_deadline, [] {}, clock, sleep_until);
}

/**
 * @brief Uses the process monotonic clock to wait within one I2 deadline.
 * @param fence Exact pending or terminal fence to observe.
 * @param capture_deadline Exclusive absolute I2 capture deadline.
 * @return Terminal snapshot accepted only when its fresh post-poll sample is
 * strictly before the deadline, otherwise `std::nullopt`.
 * @throws std::logic_error for an invalid fence.
 * @throws std::system_error from the platform sleeper unchanged.
 * @note This overload delegates to the deterministic injected-clock form and
 * grants no completion or cancellation authority.
 */
inline std::optional<ReadyFenceSnapshot> wait_for_i2_metal_completion_until(
    const ReadyFence& fence,
    std::chrono::steady_clock::time_point capture_deadline) {
  return wait_for_i2_metal_completion_until(
      fence, capture_deadline, [] { return std::chrono::steady_clock::now(); },
      [](std::chrono::steady_clock::time_point wake_at) {
        std::this_thread::sleep_until(wake_at);
      });
}

/**
 * @brief Removes publication authority after an I2 Metal deadline expires.
 * @param residency Process manager that admitted the exact transfer.
 * @param identity Exact source/destination native completion identity.
 * @param pending Exact pending destination retained by the timed-out caller.
 * @return Which manager/fence race boundary performed containment.
 * @throws std::invalid_argument when `pending` does not match the destination
 * revision, producer, or complete binding in `identity`.
 * @throws std::system_error from manager synchronization unchanged.
 * @note A successful discard makes a late native callback fail publication.
 * If completion already published Ready, the helper releases only the exact
 * resident. If completion removed its admission before settling a non-Ready
 * fence, its unique callback remains the only terminal/lease owner.
 */
inline I2TimedOutTransferContainment contain_i2_timed_out_transfer(
    execution::ResidencyManager& residency,
    const execution::DeviceCompletionIdentity& identity, const Value& pending) {
  if (!pending.valid() ||
      pending.revision_id() != identity.destination_revision() ||
      pending.producer_identity() != identity.destination_producer() ||
      pending.storage_binding() != identity.destination_binding()) {
    throw std::invalid_argument(
        "I2 Metal timeout containment requires the exact destination Value.");
  }
  if (residency.discard_transfer(identity)) {
    return I2TimedOutTransferContainment::PendingAdmissionDiscarded;
  }

  const ReadyFenceSnapshot terminal = pending.ready_fence().poll();
  if (!terminal.terminal()) {
    return I2TimedOutTransferContainment::CompletionOwnerSettling;
  }
  if (!terminal.ready()) {
    return I2TimedOutTransferContainment::TerminalWithoutResident;
  }
  if (residency.release_resident(pending.revision_id(),
                                 pending.storage_binding(),
                                 pending.producer_identity())) {
    return I2TimedOutTransferContainment::ReadyResidentReleased;
  }
  return I2TimedOutTransferContainment::TerminalWithoutResident;
}

}  // namespace ps::compute
