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

#include "execution/device_completion.hpp"
#include "execution/residency_manager.hpp"
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

/**
 * @brief Waits for one Metal fence only while an absolute deadline is open.
 * @tparam Clock Nullary callable returning `steady_clock::time_point`.
 * @tparam SleepUntil Callable accepting one `steady_clock::time_point`.
 * @param fence Exact pending or terminal fence to observe.
 * @param capture_deadline Exclusive absolute I2 capture deadline.
 * @param clock Monotonic observation source.
 * @param sleep_until Bounded sleeper used only between polls.
 * @return Terminal snapshot observed strictly before the deadline, otherwise
 * `std::nullopt` even when Ready exists at an exact deadline tie.
 * @throws std::logic_error for an invalid fence.
 * @throws Clock or sleeper failures unchanged.
 * @note The function owns no producer, Value, transfer, residency, ledger, or
 * native cancellation authority. It checks the exclusive deadline before
 * every fence observation and never sleeps past it.
 */
template <typename Clock, typename SleepUntil>
std::optional<ReadyFenceSnapshot> wait_for_i2_metal_completion_until(
    const ReadyFence& fence,
    std::chrono::steady_clock::time_point capture_deadline, Clock&& clock,
    SleepUntil&& sleep_until) {
  while (true) {
    const std::chrono::steady_clock::time_point observed_at = clock();
    if (observed_at >= capture_deadline) {
      return std::nullopt;
    }
    const ReadyFenceSnapshot observed = fence.poll();
    if (observed.terminal()) {
      return observed;
    }
    const auto remaining = capture_deadline - observed_at;
    const std::chrono::steady_clock::time_point wake_at =
        remaining <= kI2MetalCompletionPollInterval
            ? capture_deadline
            : observed_at + kI2MetalCompletionPollInterval;
    sleep_until(wake_at);
  }
}

/**
 * @brief Uses the process monotonic clock to wait within one I2 deadline.
 * @param fence Exact pending or terminal fence to observe.
 * @param capture_deadline Exclusive absolute I2 capture deadline.
 * @return Terminal snapshot observed strictly before the deadline, otherwise
 * `std::nullopt`.
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
