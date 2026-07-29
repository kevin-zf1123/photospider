#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

/**
 * @file ready_fence.hpp
 * @brief Dependency-neutral producer-completion observation and async waits.
 */

namespace ps {

/**
 * @brief Identifies one immutable producer-completion state.
 *
 * @throws Nothing for copying, comparison, and destruction.
 * @note Ready records producer completion only. It does not independently grant
 *       device mapping, cache visibility, or consumer read access.
 */
enum class ReadyFenceState : std::uint32_t {
  /** @brief The registered producer has not published a terminal result. */
  Pending = 0U,

  /** @brief The producer retired its access and completed successfully. */
  Ready = 1U,

  /** @brief The producer retired its access and published a typed failure. */
  Failed = 2U,

  /** @brief The unresolved producer retired or was cancelled. */
  ProducerCancelled = 3U,
};

/**
 * @brief Categorizes a terminal producer failure without backend dependencies.
 *
 * @throws Nothing for copying, comparison, and destruction.
 * @note The category is process/runtime diagnostics, not a serialized provider
 *       ABI or persistent cache identity.
 */
enum class ReadyFenceFailureDomain : std::uint32_t {
  /** @brief Producer-defined payload completion failed. */
  Producer = 0U,

  /** @brief Explicit transfer execution failed. */
  Transfer = 1U,

  /** @brief Owning execution mechanism failed while running producer work. */
  Execution = 2U,
};

/**
 * @brief Immutable typed diagnostic attached to a Failed fence.
 *
 * @throws std::bad_alloc when owned diagnostic strings cannot allocate.
 * @note Failure values contain no exception pointer, provider object, native
 *       handle, or mutable payload authority.
 */
class ReadyFenceFailure final {
 public:
  /**
   * @brief Creates one owned failure diagnostic.
   *
   * @param domain Stable high-level failure category.
   * @param code Provider- or task-local numeric diagnostic code.
   * @param message Human-readable diagnostic copied into this value.
   * @throws std::bad_alloc when message storage cannot allocate.
   */
  ReadyFenceFailure(ReadyFenceFailureDomain domain, std::int64_t code,
                    std::string message);

  /**
   * @brief Returns the stable high-level failure category.
   *
   * @return Category supplied at construction.
   * @throws Nothing.
   */
  ReadyFenceFailureDomain domain() const noexcept { return domain_; }

  /**
   * @brief Returns the producer- or task-local diagnostic code.
   *
   * @return Numeric code supplied at construction.
   * @throws Nothing.
   */
  std::int64_t code() const noexcept { return code_; }

  /**
   * @brief Returns the retained human-readable diagnostic.
   *
   * @return Borrowed immutable message retained by this failure.
   * @throws Nothing.
   */
  const std::string& message() const noexcept { return message_; }

  /**
   * @brief Compares the complete typed diagnostic.
   *
   * @param other Failure to compare.
   * @return True when domain, code, and message all match.
   * @throws Nothing.
   */
  bool operator==(const ReadyFenceFailure& other) const noexcept;

 private:
  /** @brief Stable high-level failure category. */
  ReadyFenceFailureDomain domain_ = ReadyFenceFailureDomain::Producer;

  /** @brief Producer- or task-local diagnostic code. */
  std::int64_t code_ = 0;

  /** @brief Owned human-readable diagnostic. */
  std::string message_;
};

/**
 * @brief Immutable point-in-time observation of one ReadyFence.
 *
 * @throws Nothing for copying, moving, assignment, and destruction.
 * @note A Failed snapshot retains its failure independently of the observer
 *       handle that produced it.
 */
class ReadyFenceSnapshot final {
 public:
  /**
   * @brief Returns the observed producer-completion state.
   *
   * @return Pending or one terminal state.
   * @throws Nothing.
   */
  ReadyFenceState state() const noexcept { return state_; }

  /**
   * @brief Reports whether this snapshot is terminal.
   *
   * @return False only for Pending.
   * @throws Nothing.
   */
  bool terminal() const noexcept { return state_ != ReadyFenceState::Pending; }

  /**
   * @brief Reports whether producer completion succeeded.
   *
   * @return True only for Ready.
   * @throws Nothing.
   */
  bool ready() const noexcept { return state_ == ReadyFenceState::Ready; }

  /**
   * @brief Returns the retained failure diagnostic when state is Failed.
   *
   * @return Borrowed failure pointer, or null for every other state.
   * @throws Nothing.
   * @note The pointer remains valid for this snapshot lifetime.
   */
  const ReadyFenceFailure* failure() const noexcept { return failure_.get(); }

 private:
  /**
   * @brief Creates a complete immutable state observation.
   *
   * @param state Observed state.
   * @param failure Retained failure for Failed, otherwise null.
   * @throws Nothing.
   */
  ReadyFenceSnapshot(ReadyFenceState state,
                     std::shared_ptr<const ReadyFenceFailure> failure) noexcept;

  /** @brief Observed state. */
  ReadyFenceState state_ = ReadyFenceState::Pending;

  /** @brief Retained typed failure only when state_ is Failed. */
  std::shared_ptr<const ReadyFenceFailure> failure_;

  friend class ReadyFence;
};

/**
 * @brief Typed exception raised when a fence-gated payload cannot be read.
 *
 * @throws std::bad_alloc when constructing the inherited diagnostic string.
 * @note The retained snapshot lets callers distinguish Pending, Failed, and
 *       ProducerCancelled without parsing `what()`.
 */
class ReadyFenceAccessError final : public std::runtime_error {
 public:
  /**
   * @brief Creates one payload-access rejection from an observed fence state.
   *
   * @param snapshot Non-Ready state that denied access.
   * @throws std::invalid_argument when snapshot is Ready.
   * @throws std::bad_alloc when exception diagnostic storage cannot allocate.
   */
  explicit ReadyFenceAccessError(ReadyFenceSnapshot snapshot);

  /**
   * @brief Returns the exact state that denied payload access.
   *
   * @return Borrowed immutable snapshot retained by this exception.
   * @throws Nothing.
   */
  const ReadyFenceSnapshot& snapshot() const noexcept { return snapshot_; }

 private:
  /** @brief Exact non-Ready observation captured at the access boundary. */
  ReadyFenceSnapshot snapshot_;
};

/**
 * @brief Owning execution mechanism used only to enqueue fence continuations.
 *
 * Implementations own their worker, queue, exception transport, and shutdown
 * behavior. A fence retains a shared executor reference only while a wait is
 * pending or its callback is queued. The queued continuation transfers that
 * reference to callback-local retention on entry, breaking any temporary
 * executor/owned-queue self-cycle while keeping the executor alive through
 * callback completion or exception unwinding.
 *
 * @throws Nothing from `submit()` and destruction.
 * @note `submit()` must take ownership without running the callback inline.
 *       Value and ReadyFence never create a worker or wait for one.
 * @note An executor whose queue is owned by the executor object may temporarily
 *       retain itself through an admitted callback. Its queue-driving or
 *       shutdown path must remain able to enter or discard admitted work; a
 *       callback that enters releases the queued self-reference before
 *       invoking user code.
 */
class ReadyFenceExecutor {
 public:
  /**
   * @brief Owned callback admitted to the execution mechanism.
   *
   * @throws std::bad_alloc while constructing caller-owned callable state.
   */
  using Task = std::function<void()>;

  /**
   * @brief Releases one concrete execution mechanism.
   *
   * @throws Nothing.
   */
  virtual ~ReadyFenceExecutor() noexcept = default;

  /**
   * @brief Enqueues one callback without executing it inline.
   *
   * @param task Nonempty callback whose ownership moves into the executor.
   * @return Nothing.
   * @throws Nothing. Implementations must provide their own fail-closed
   *         admission behavior.
   * @note The executor owns callback exceptions and must not propagate them
   *       through a fence terminal-publication call.
   */
  virtual void submit(Task task) noexcept = 0;
};

class FenceCompleter;
struct PendingReadyFence;

/**
 * @brief Move-only cancellation handle for one asynchronous fence wait.
 *
 * @throws Nothing for movement, cancellation, and destruction.
 * @note Cancelling prevents a callback that has not begun. It neither waits for
 *       an already running callback nor mutates the producer or shared fence.
 *       One registration object is externally serialized; its atomic callback
 *       gate is safe against concurrent executor callback entry.
 */
class ReadyFenceWaitRegistration final {
 public:
  /**
   * @brief Creates an inactive registration sentinel.
   *
   * @throws Nothing.
   */
  ReadyFenceWaitRegistration() noexcept = default;

  /** @brief Copy construction is forbidden for one cancellation authority. */
  ReadyFenceWaitRegistration(const ReadyFenceWaitRegistration&) = delete;

  /** @brief Copy assignment is forbidden for one cancellation authority. */
  ReadyFenceWaitRegistration& operator=(const ReadyFenceWaitRegistration&) =
      delete;

  /**
   * @brief Transfers callback-cancellation ownership.
   *
   * @param other Registration to consume.
   * @throws Nothing.
   */
  ReadyFenceWaitRegistration(ReadyFenceWaitRegistration&& other) noexcept =
      default;

  /**
   * @brief Replaces this registration with transferred ownership.
   *
   * @param other Registration to consume.
   * @return This registration after transfer.
   * @throws Nothing.
   * @note Any previously active registration is cancelled first.
   */
  ReadyFenceWaitRegistration& operator=(
      ReadyFenceWaitRegistration&& other) noexcept;

  /**
   * @brief Cancels an active callback that has not begun.
   *
   * @throws Nothing.
   * @note Repeated cancellation is a no-op.
   */
  ~ReadyFenceWaitRegistration() noexcept;

  /**
   * @brief Cancels this wait without changing fence state.
   *
   * @return True only when this call changed an active registration to
   *         cancelled.
   * @throws Nothing.
   * @note Pending callback ownership is released only after the fence mutex is
   *       unlocked, so captured object destruction may safely inspect the
   *       fence.
   */
  bool cancel() noexcept;

  /**
   * @brief Reports whether the callback may still begin.
   *
   * @return True before cancellation or callback entry.
   * @throws Nothing.
   */
  bool active() const noexcept;

 private:
  /** @brief Hidden waiter identity, callback gate, and fence association. */
  struct Impl;

  /**
   * @brief Creates one live cancellation handle.
   *
   * @param impl Complete hidden registration state.
   * @throws Nothing.
   */
  explicit ReadyFenceWaitRegistration(std::shared_ptr<Impl> impl) noexcept;

  /** @brief Shared registration state, or null for an inactive sentinel. */
  std::shared_ptr<Impl> impl_;

  friend class ReadyFence;
};

/**
 * @brief Immutable copyable observer of one producer-completion state.
 *
 * @throws Nothing for copying, moving, assignment, and destruction.
 * @note The observer owns no worker, queue, payload write capability, mapping,
 *       visibility action, or consumer read lease.
 * @note Independent observer copies may poll and register waits concurrently
 *       with terminal publication.
 */
class ReadyFence final {
 public:
  /**
   * @brief Callback invoked asynchronously with one terminal snapshot.
   *
   * @throws std::bad_alloc while constructing caller-owned callable state.
   * @note The owning ReadyFenceExecutor transports callback exceptions.
   */
  using Callback = std::function<void(ReadyFenceSnapshot)>;

  /**
   * @brief Creates an invalid observer sentinel.
   *
   * @throws Nothing.
   */
  ReadyFence() noexcept = default;

  /**
   * @brief Creates an immutable already-Ready observer.
   *
   * @return Observer sharing the process runtime's terminal Ready state.
   * @throws std::bad_alloc if the process-wide state is initialized and cannot
   *         allocate.
   * @note This is used by synchronous CPU Value publication.
   */
  static ReadyFence already_ready();

  /**
   * @brief Reports whether this observer retains a fence state.
   *
   * @return True for pending or terminal observers.
   * @throws Nothing.
   */
  bool valid() const noexcept { return state_ != nullptr; }

  /**
   * @brief Observes the current state without waiting.
   *
   * @return Immutable Pending or terminal snapshot.
   * @throws std::logic_error when this observer is invalid.
   * @note The method takes only the short shared-state mutex and performs no
   *       condition-variable, future, worker, device, or IO wait.
   */
  ReadyFenceSnapshot poll() const;

  /**
   * @brief Enqueues one callback after this fence becomes terminal.
   *
   * @param executor Shared owning mechanism that queues the callback.
   * @param callback Nonempty callback receiving the terminal snapshot.
   * @return Move-only observer-local cancellation registration.
   * @throws std::logic_error when this observer is invalid.
   * @throws std::invalid_argument when executor or callback is empty.
   * @throws std::overflow_error when pending waiter identity is exhausted.
   * @throws std::bad_alloc when callback or waiter state cannot allocate.
   * @note A terminal fence submits before this method returns but the executor
   *       must not run the callback inline. A pending fence submits exactly
   *       once after its terminal transition. The queued wrapper retains the
   *       shared executor until callback entry and keeps it alive locally
   *       through callback completion, including cancellation no-op entry and
   *       exception unwinding.
   */
  ReadyFenceWaitRegistration async_wait(
      std::shared_ptr<ReadyFenceExecutor> executor, Callback callback) const;

 private:
  /** @brief Shared synchronized state retained by observers and completer. */
  struct State;

  /**
   * @brief Creates an observer over initialized shared state.
   *
   * @param state Pending or terminal shared state.
   * @throws Nothing.
   */
  explicit ReadyFence(std::shared_ptr<State> state) noexcept;

  /** @brief Shared state, or null for an invalid sentinel. */
  std::shared_ptr<State> state_;

  friend class FenceCompleter;
  friend class ReadyFenceWaitRegistration;
  friend PendingReadyFence make_pending_ready_fence();
};

/**
 * @brief Move-only capability that publishes one ReadyFence terminal state.
 *
 * @throws Nothing for destruction and movement.
 * @note This capability grants no payload write access. The registered producer
 *       must independently retire such access before calling a terminal method.
 *       The unique completer object is externally serialized.
 */
class FenceCompleter final {
 public:
  /**
   * @brief Creates an invalid moved-from-style sentinel.
   *
   * @throws Nothing.
   */
  FenceCompleter() noexcept = default;

  /** @brief Copy construction is forbidden for terminal publication authority.
   */
  FenceCompleter(const FenceCompleter&) = delete;

  /** @brief Copy assignment is forbidden for terminal publication authority. */
  FenceCompleter& operator=(const FenceCompleter&) = delete;

  /**
   * @brief Transfers the unresolved terminal-publication capability.
   *
   * @param other Completer to consume.
   * @throws Nothing.
   */
  FenceCompleter(FenceCompleter&& other) noexcept = default;

  /**
   * @brief Replaces this capability through exact ownership transfer.
   *
   * @param other Completer to consume.
   * @return This completer after transfer.
   * @throws Nothing.
   * @note An unresolved capability already held by this object first publishes
   *       ProducerCancelled.
   */
  FenceCompleter& operator=(FenceCompleter&& other) noexcept;

  /**
   * @brief Publishes ProducerCancelled for an unresolved capability.
   *
   * @throws Nothing.
   */
  ~FenceCompleter() noexcept;

  /**
   * @brief Reports whether this object can still publish a terminal state.
   *
   * @return True only while retaining unresolved shared state.
   * @throws Nothing.
   */
  bool valid() const noexcept { return state_ != nullptr; }

  /**
   * @brief Publishes successful producer completion.
   *
   * @return True when this call performed the unique terminal transition.
   * @throws Nothing.
   * @note Producer payload access must already be retired.
   */
  bool complete_ready() noexcept;

  /**
   * @brief Publishes typed producer failure.
   *
   * @param failure Complete owned failure diagnostic.
   * @return True when this call performed the unique terminal transition.
   * @throws std::bad_alloc when retained failure state cannot allocate.
   * @note Producer payload access must already be retired.
   */
  bool complete_failed(ReadyFenceFailure failure);

  /**
   * @brief Publishes producer cancellation explicitly.
   *
   * @return True when this call performed the unique terminal transition.
   * @throws Nothing.
   * @note Producer payload access must already be retired.
   */
  bool cancel() noexcept;

 private:
  /**
   * @brief Publishes one terminal state and queues every retained waiter.
   *
   * @param state Shared fence state retained by the unique completer.
   * @param terminal Ready, Failed, or ProducerCancelled.
   * @param failure Retained failure for Failed, otherwise null.
   * @return True only when this call performed the Pending-to-terminal change.
   * @throws Nothing.
   * @note Waiter callbacks are submitted after the state mutex is released.
   *       Each preconstructed callback already retains its executor, so
   *       releasing the publication-local waiter cannot abandon queued work.
   */
  static bool publish_terminal(
      const std::shared_ptr<ReadyFence::State>& state, ReadyFenceState terminal,
      std::shared_ptr<const ReadyFenceFailure> failure) noexcept;

  /**
   * @brief Creates the one completer for a new pending fence.
   *
   * @param state Fresh pending shared state.
   * @throws Nothing.
   */
  explicit FenceCompleter(std::shared_ptr<ReadyFence::State> state) noexcept;

  /** @brief Shared pending state, or null after move/terminal publication. */
  std::shared_ptr<ReadyFence::State> state_;

  friend PendingReadyFence make_pending_ready_fence();
};

/**
 * @brief Fresh pending observer and its unique terminal-publication capability.
 *
 * @throws Nothing for movement and destruction.
 * @note Copying this aggregate is disabled transitively by FenceCompleter.
 */
struct PendingReadyFence {
  /** @brief Copyable observer initially reporting Pending. */
  ReadyFence fence;

  /** @brief Unique move-only capability for the matching terminal transition.
   */
  FenceCompleter completer;
};

/**
 * @brief Creates one fresh pending fence and unique completer.
 *
 * @return Pair whose observer reports Pending until its completer resolves.
 * @throws std::bad_alloc when shared state cannot allocate.
 * @note Dropping the returned unresolved completer publishes
 *       ProducerCancelled.
 */
PendingReadyFence make_pending_ready_fence();

}  // namespace ps
