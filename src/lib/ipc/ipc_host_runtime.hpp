#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "photospider/host/host.hpp"
#include "photospider/ipc/protocol.hpp"

namespace ps::ipc::internal {

/**
 * @brief Private injectable runtime policy for the complete IPC Host.
 *
 * Production construction supplies a steady monotonic clock, a condition-
 * variable waiter, and the secure owned-copy artifact consumer. Long-lived
 * tests replace these callbacks to validate exact polling deadlines, wakeups,
 * and artifact ownership without sleeping or weakening the public factory.
 *
 * @throws std::bad_alloc when callback targets allocate during copy/move.
 * @note This header is source-tree private and is never installed. Callbacks
 *       must remain valid until every adapter polling worker has joined.
 *       `monotonic_now`, `wait_until`, `before_async_completion`, and
 *       `consume_artifact` may run concurrently on different workers;
 *       `wake_waiters` may run concurrently with any blocked wait. Injected
 *       implementations must synchronize any shared mutable state.
 */
struct IpcHostRuntimeDependencies {
  /** @brief Monotonic timestamp source used to form polling deadlines. */
  std::function<std::chrono::steady_clock::time_point()> monotonic_now;

  /**
   * @brief Interruptible wait until one monotonic deadline.
   *
   * The callback returns true when `should_stop` became true and false when the
   * supplied deadline was reached normally. It must tolerate spurious wakeups
   * and must not retain the predicate after returning.
   */
  std::function<bool(std::chrono::steady_clock::time_point,
                     const std::function<bool()>& should_stop)>
      wait_until;

  /**
   * @brief Wakes every wait currently blocked in `wait_until`.
   *
   * Destruction invokes this callback after publishing stop. Implementations
   * must not throw; any unexpected exception is contained by adapter cleanup.
   */
  std::function<void()> wake_waiters;

  /**
   * @brief Private hook immediately before a validated async outcome publishes.
   *
   * Production supplies a no-op. Deterministic lifecycle tests may block here
   * so adapter stop can linearize first and prove that only the destructor
   * publishes client_stopped after wake and descriptor interruption.
   */
  std::function<void()> before_async_completion;

  /**
   * @brief Converts one lease-protected artifact into an owned CPU image.
   *
   * @param metadata Strictly decoded daemon artifact metadata.
   * @return Owned image or an exact local Transport/Protocol failure.
   * @throws std::bad_alloc if owned image allocation fails.
   * @note The callback runs before lease-aware `compute.release`. Task 4.2's
   *       production callback performs strict same-user identity validation and
   *       copies bytes into independent memory. Task 4.3 replaces that callback
   *       with read-only mmap/shared-deleter ownership without changing polling
   *       or Host dispatch.
   */
  std::function<Result<ImageBuffer>(const OutputArtifactMetadata& metadata)>
      consume_artifact;
};

/**
 * @brief Creates an IPC Host with complete private runtime dependencies.
 *
 * @param socket_path Absolute daemon Unix socket path.
 * @param dependencies Nonempty clock/wait/wake/completion/artifact callbacks.
 * @return Unique complete IPC Host implementation.
 * @throws std::invalid_argument if any required callback is absent.
 * @throws std::bad_alloc if adapter allocation or callback transfer fails.
 * @note This private factory exists only for deterministic production-behavior
 *       tests. It performs no daemon connection or embedded fallback.
 */
std::unique_ptr<Host> create_ipc_host_with_dependencies(
    const std::string& socket_path, IpcHostRuntimeDependencies dependencies);

}  // namespace ps::ipc::internal
