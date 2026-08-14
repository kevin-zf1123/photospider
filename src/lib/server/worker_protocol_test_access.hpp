/**
 * @file worker_protocol_test_access.hpp
 * @brief Declares source-private deterministic worker deadline test hooks.
 */
#pragma once

#include <chrono>
#include <cstdint>

namespace ps::server {

/**
 * @brief Closed observation points around protocol deadline acceptance.
 * @throws Nothing for value operations.
 * @note These points expose no channel, frame, lifecycle, or timing authority;
 * they exist only so a unit test can deterministically move its private clock
 * across one deadline at an exact implementation boundary.
 */
enum class WorkerProtocolDeadlineTestPoint : std::uint8_t {
  /** @brief A complete frame is ready but has not been delivered or reset. */
  FrameReadyBeforeAcceptance = 0U,
  /** @brief One positive `send` has updated exact write progress. */
  WriteProgressAfterSend = 1U,
  /** @brief Semantic interpretation finished before frame acceptance commit. */
  FrameSemanticReadyBeforeAcceptance = 2U,
};

/**
 * @brief Borrowed callbacks for one thread's deterministic deadline test.
 * @throws Nothing for aggregate initialization and value operations.
 * @note Both callbacks must be `noexcept`, must not perform protocol I/O, and
 * must outlive the corresponding `ScopedWorkerProtocolDeadlineTestHooks`.
 * A null callback leaves that behavior on its production-default path.
 */
struct WorkerProtocolDeadlineTestHooks final {
  /** @brief Signature of one monotonic-clock replacement. */
  using NowFunction =
      std::chrono::steady_clock::time_point (*)(void* context) noexcept;
  /** @brief Signature of one exact-boundary observer. */
  using ObserveFunction =
      void (*)(void* context, WorkerProtocolDeadlineTestPoint point) noexcept;

  /** @brief Borrowed opaque state passed unchanged to both callbacks. */
  void* context = nullptr;
  /** @brief Optional current-thread monotonic-clock replacement. */
  NowFunction now = nullptr;
  /** @brief Optional current-thread exact-boundary observer. */
  ObserveFunction observe = nullptr;
};

/**
 * @brief Installs borrowed deadline hooks for the current thread and scope.
 *
 * Construction replaces only the calling thread's prior source-private hook;
 * destruction restores that exact prior pointer. Production threads never
 * instantiate this guard, so their clock and protocol path remain unchanged.
 *
 * @throws Nothing for construction and destruction.
 * @note Instances must be destroyed on their construction thread and must not
 * outlive the referenced `WorkerProtocolDeadlineTestHooks`.
 */
class ScopedWorkerProtocolDeadlineTestHooks final {
 public:
  /**
   * @brief Installs one borrowed hook set on the calling thread.
   * @param hooks Non-null hooks that outlive this guard, or null to mask a
   * previously installed hook within this scope.
   * @throws Nothing.
   */
  explicit ScopedWorkerProtocolDeadlineTestHooks(
      const WorkerProtocolDeadlineTestHooks* hooks) noexcept;

  /** @brief Restores the exact hook pointer replaced at construction. */
  ~ScopedWorkerProtocolDeadlineTestHooks() noexcept;

  /** @brief Prevents duplicate restoration ownership. */
  ScopedWorkerProtocolDeadlineTestHooks(
      const ScopedWorkerProtocolDeadlineTestHooks& other) = delete;
  /** @brief Prevents replacement of restoration ownership. */
  ScopedWorkerProtocolDeadlineTestHooks& operator=(
      const ScopedWorkerProtocolDeadlineTestHooks& other) = delete;

 private:
  /** @brief Borrowed hook pointer active before this guard was constructed. */
  const WorkerProtocolDeadlineTestHooks* previous_ = nullptr;
};

}  // namespace ps::server
