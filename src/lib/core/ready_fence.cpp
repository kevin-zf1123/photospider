#include "photospider/memory/ready_fence.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ps {
namespace {

/**
 * @brief Shared observer-local gate for one asynchronous wait callback.
 *
 * @note The atomic linearizes callback entry against cancellation. It does not
 *       represent producer or fence state.
 */
struct WaitControl final {
  /** @brief True until cancellation or callback entry wins the race. */
  std::atomic<bool> active{true};
};

/**
 * @brief Returns a stable diagnostic label for one fence state.
 *
 * @param state State to format.
 * @return Static state label.
 * @throws Nothing.
 */
const char* ready_fence_state_name(ReadyFenceState state) noexcept {
  switch (state) {
    case ReadyFenceState::Pending:
      return "Pending";
    case ReadyFenceState::Ready:
      return "Ready";
    case ReadyFenceState::Failed:
      return "Failed";
    case ReadyFenceState::ProducerCancelled:
      return "ProducerCancelled";
  }
  return "Unknown";
}

/**
 * @brief Builds the inherited diagnostic for a payload-access rejection.
 *
 * @param snapshot Exact non-Ready observation.
 * @return Owned diagnostic string.
 * @throws std::invalid_argument when snapshot is Ready.
 * @throws std::bad_alloc when string formatting cannot allocate.
 */
std::string make_access_error_message(const ReadyFenceSnapshot& snapshot) {
  if (snapshot.ready()) {
    throw std::invalid_argument(
        "ReadyFenceAccessError requires a non-Ready snapshot.");
  }
  std::ostringstream stream;
  stream << "Value payload is unavailable while ReadyFence is "
         << ready_fence_state_name(snapshot.state());
  if (snapshot.failure() != nullptr) {
    stream << " (code " << snapshot.failure()->code() << ": "
           << snapshot.failure()->message() << ')';
  }
  stream << '.';
  return stream.str();
}

}  // namespace

/**
 * @brief One callback retained while its observed fence remains pending.
 *
 * @note All fields are completely constructed before insertion into the fence
 *       waiter vector.
 */
struct ReadyFence::State final {
  /**
   * @brief Complete pending waiter entry.
   *
   * @note The executor owns physical admission; the fence owns this value only
   *       until cancellation or terminal publication.
   */
  struct Waiter final {
    /** @brief Nonzero identity local to this fence state. */
    std::uint64_t id = 0U;

    /** @brief Observer-local callback-entry gate. */
    std::shared_ptr<WaitControl> control;

    /** @brief Shared owning execution mechanism for asynchronous admission. */
    std::shared_ptr<ReadyFenceExecutor> executor;

    /** @brief Preconstructed terminal callback wrapper. */
    ReadyFenceExecutor::Task task;
  };

  /** @brief Serializes state, failure, waiter identities, and waiter ownership.
   */
  std::mutex mutex;

  /** @brief Pending or the unique published terminal state. */
  ReadyFenceState state = ReadyFenceState::Pending;

  /** @brief Retained typed failure only when state is Failed. */
  std::shared_ptr<const ReadyFenceFailure> failure;

  /** @brief Last issued waiter identity; zero means none issued. */
  std::uint64_t last_waiter_id = 0U;

  /** @brief Pending waiters transferred out at terminal publication. */
  std::vector<Waiter> waiters;
};

/**
 * @brief Hidden cancellation state for one wait registration.
 *
 * @note A weak fence link prevents a cancelled registration from extending the
 *       producer/fence lifetime.
 */
struct ReadyFenceWaitRegistration::Impl final {
  /** @brief Fence that may still own the pending waiter entry. */
  std::weak_ptr<ReadyFence::State> fence;

  /** @brief Nonzero pending waiter identity, or zero once already queued. */
  std::uint64_t waiter_id = 0U;

  /** @brief Shared callback-entry gate retained by queued work. */
  std::shared_ptr<WaitControl> control;
};

/** @copydoc ReadyFenceFailure::ReadyFenceFailure */
ReadyFenceFailure::ReadyFenceFailure(ReadyFenceFailureDomain domain,
                                     std::int64_t code, std::string message)
    : domain_(domain), code_(code), message_(std::move(message)) {}

/** @copydoc ReadyFenceFailure::operator== */
bool ReadyFenceFailure::operator==(
    const ReadyFenceFailure& other) const noexcept {
  return domain_ == other.domain_ && code_ == other.code_ &&
         message_ == other.message_;
}

/** @copydoc ReadyFenceSnapshot::ReadyFenceSnapshot */
ReadyFenceSnapshot::ReadyFenceSnapshot(
    ReadyFenceState state,
    std::shared_ptr<const ReadyFenceFailure> failure) noexcept
    : state_(state), failure_(std::move(failure)) {}

/** @copydoc ReadyFenceAccessError::ReadyFenceAccessError */
ReadyFenceAccessError::ReadyFenceAccessError(ReadyFenceSnapshot snapshot)
    : std::runtime_error(make_access_error_message(snapshot)),
      snapshot_(std::move(snapshot)) {}  // NOLINT(whitespace/indent_namespace)

/** @copydoc ReadyFenceWaitRegistration::ReadyFenceWaitRegistration */
ReadyFenceWaitRegistration::ReadyFenceWaitRegistration(
    std::shared_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

/** @copydoc ReadyFenceWaitRegistration::operator= */
ReadyFenceWaitRegistration& ReadyFenceWaitRegistration::operator=(
    ReadyFenceWaitRegistration&& other) noexcept {
  if (this != &other) {
    cancel();
    impl_ = std::move(other.impl_);
  }
  return *this;
}

/** @copydoc ReadyFenceWaitRegistration::~ReadyFenceWaitRegistration */
ReadyFenceWaitRegistration::~ReadyFenceWaitRegistration() noexcept {
  cancel();
}

/** @copydoc ReadyFenceWaitRegistration::cancel */
bool ReadyFenceWaitRegistration::cancel() noexcept {
  if (!impl_ || !impl_->control) {
    impl_.reset();
    return false;
  }

  const bool was_active =
      impl_->control->active.exchange(false, std::memory_order_acq_rel);
  ReadyFence::State::Waiter retired_waiter;
  if (was_active && impl_->waiter_id != 0U) {
    if (const std::shared_ptr<ReadyFence::State> state = impl_->fence.lock()) {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->state == ReadyFenceState::Pending) {
        const auto found = std::find_if(
            state->waiters.begin(), state->waiters.end(),
            [id = impl_->waiter_id](const ReadyFence::State::Waiter& waiter) {
              return waiter.id == id;
            });
        if (found != state->waiters.end()) {
          retired_waiter = std::move(*found);
          state->waiters.erase(found);
        }
      }
    }
  }
  impl_.reset();
  return was_active;
}

/** @copydoc ReadyFenceWaitRegistration::active */
bool ReadyFenceWaitRegistration::active() const noexcept {
  return impl_ && impl_->control &&
         impl_->control->active.load(std::memory_order_acquire);
}

/** @copydoc ReadyFence::ReadyFence */
ReadyFence::ReadyFence(std::shared_ptr<State> state) noexcept
    : state_(std::move(state)) {}

/** @copydoc ReadyFence::already_ready */
ReadyFence ReadyFence::already_ready() {
  static const std::shared_ptr<State> ready_state = [] {
    auto state = std::make_shared<State>();
    state->state = ReadyFenceState::Ready;
    return state;
  }();
  return ReadyFence(ready_state);
}

/** @copydoc ReadyFence::poll */
ReadyFenceSnapshot ReadyFence::poll() const {
  if (!state_) {
    throw std::logic_error("Invalid ReadyFence cannot be polled.");
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  return ReadyFenceSnapshot(state_->state, state_->failure);
}

/** @copydoc ReadyFence::async_wait */
ReadyFenceWaitRegistration ReadyFence::async_wait(
    std::shared_ptr<ReadyFenceExecutor> executor, Callback callback) const {
  if (!state_) {
    throw std::logic_error("Invalid ReadyFence cannot register a wait.");
  }
  if (!executor) {
    throw std::invalid_argument(
        "ReadyFence asynchronous wait requires an executor.");
  }
  if (!callback) {
    throw std::invalid_argument(
        "ReadyFence asynchronous wait requires a callback.");
  }

  auto control = std::make_shared<WaitControl>();
  auto registration = std::make_shared<ReadyFenceWaitRegistration::Impl>();
  registration->fence = state_;
  registration->control = control;
  ReadyFenceExecutor::Task task = [state = state_, control,
                                   callback = std::move(callback)]() mutable {
    if (!control->active.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    ReadyFenceSnapshot snapshot(ReadyFenceState::Pending, nullptr);
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      snapshot = ReadyFenceSnapshot(state->state, state->failure);
    }
    callback(std::move(snapshot));
  };

  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->state == ReadyFenceState::Pending) {
      if (state_->last_waiter_id == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("ReadyFence waiter identity is exhausted.");
      }
      const std::uint64_t waiter_id = state_->last_waiter_id + 1U;
      state_->waiters.push_back(
          {waiter_id, control, std::move(executor), std::move(task)});
      state_->last_waiter_id = waiter_id;
      registration->waiter_id = waiter_id;
      return ReadyFenceWaitRegistration(std::move(registration));
    }
  }

  executor->submit(std::move(task));
  return ReadyFenceWaitRegistration(std::move(registration));
}

/** @copydoc FenceCompleter::publish_terminal */
bool FenceCompleter::publish_terminal(
    const std::shared_ptr<ReadyFence::State>& state, ReadyFenceState terminal,
    std::shared_ptr<const ReadyFenceFailure> failure) noexcept {
  std::vector<ReadyFence::State::Waiter> waiters;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->state != ReadyFenceState::Pending) {
      return false;
    }
    state->failure = std::move(failure);
    state->state = terminal;
    waiters = std::move(state->waiters);
  }

  for (ReadyFence::State::Waiter& waiter : waiters) {
    waiter.executor->submit(std::move(waiter.task));
  }
  return true;
}

/** @copydoc FenceCompleter::FenceCompleter */
FenceCompleter::FenceCompleter(
    std::shared_ptr<ReadyFence::State> state) noexcept
    : state_(std::move(state)) {}

/** @copydoc FenceCompleter::operator= */
FenceCompleter& FenceCompleter::operator=(FenceCompleter&& other) noexcept {
  if (this != &other) {
    cancel();
    state_ = std::move(other.state_);
  }
  return *this;
}

/** @copydoc FenceCompleter::~FenceCompleter */
FenceCompleter::~FenceCompleter() noexcept {
  cancel();
}

/** @copydoc FenceCompleter::complete_ready */
bool FenceCompleter::complete_ready() noexcept {
  if (!state_) {
    return false;
  }
  const bool published =
      publish_terminal(state_, ReadyFenceState::Ready, nullptr);
  state_.reset();
  return published;
}

/** @copydoc FenceCompleter::complete_failed */
bool FenceCompleter::complete_failed(ReadyFenceFailure failure) {
  if (!state_) {
    return false;
  }
  auto retained_failure =
      std::make_shared<const ReadyFenceFailure>(std::move(failure));
  const bool published = publish_terminal(state_, ReadyFenceState::Failed,
                                          std::move(retained_failure));
  state_.reset();
  return published;
}

/** @copydoc FenceCompleter::cancel */
bool FenceCompleter::cancel() noexcept {
  if (!state_) {
    return false;
  }
  const bool published =
      publish_terminal(state_, ReadyFenceState::ProducerCancelled, nullptr);
  state_.reset();
  return published;
}

/** @copydoc ps::make_pending_ready_fence */
PendingReadyFence make_pending_ready_fence() {
  auto state = std::make_shared<ReadyFence::State>();
  return {ReadyFence(state), FenceCompleter(std::move(state))};
}

}  // namespace ps
