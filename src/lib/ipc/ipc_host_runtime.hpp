#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "photospider/host/host.hpp"
#include "photospider/ipc/protocol.hpp"

namespace ps::ipc::internal {

/**
 * @brief Private POSIX mapping operations used by artifact ownership tests.
 *
 * Production construction supplies direct wrappers around read-only `mmap`,
 * `munmap`, and `close`. Tests may substitute observable wrappers while still
 * exercising the production validation and shared-ownership implementation.
 *
 * @throws Nothing for value construction and copy.
 * @note Every callback must follow the corresponding POSIX return/`errno`
 *       contract and must not throw. `unmap` and `close_fd` are invoked from a
 *       shared-pointer deleter, where exceptions cannot be propagated.
 */
struct ArtifactMappingOperations {
  /**
   * @brief Creates a read-only private mapping of one complete descriptor.
   * @param fd Open validated artifact descriptor.
   * @param length Exact nonzero artifact byte count.
   * @return Mapping address, or `MAP_FAILED` with `errno` set.
   * @throws Nothing.
   * @note The callback maps from offset zero and never takes descriptor
   *       ownership.
   */
  void* (*map_read_only)(int fd, std::size_t length) noexcept = nullptr;

  /**
   * @brief Releases one mapping created by `map_read_only`.
   * @param address Exact mapping base address.
   * @param length Exact mapped byte count.
   * @return Zero on success, or minus one with `errno` set.
   * @throws Nothing.
   * @note The production shared owner invokes this callback exactly once.
   */
  int (*unmap)(void* address, std::size_t length) noexcept = nullptr;

  /**
   * @brief Closes the descriptor retained for one mapping lifetime.
   * @param fd Exact owned artifact descriptor.
   * @return Zero on success, or minus one with `errno` set.
   * @throws Nothing.
   * @note The production shared owner invokes this callback exactly once,
   *       after its matching `unmap` call.
   */
  int (*close_fd)(int fd) noexcept = nullptr;
};

/**
 * @brief Private injectable runtime policy for the complete IPC Host.
 *
 * Production construction supplies a steady monotonic clock, a condition-
 * variable waiter, and the secure read-only mapping consumer. Long-lived
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
  /**
   * @brief Returns the current monotonic time used for polling deadlines.
   * @return One steady-clock timestamp.
   * @throws Whatever the injected timestamp source throws; production does
   *         not throw.
   * @note Calls may execute concurrently on independent polling workers.
   */
  std::function<std::chrono::steady_clock::time_point()> monotonic_now;

  /**
   * @brief Interruptible wait until one monotonic deadline.
   *
   * The callback returns true when `should_stop` became true and false when the
   * supplied deadline was reached normally. It must tolerate spurious wakeups
   * and must not retain the predicate after returning.
   *
   * @param deadline Absolute steady-clock deadline for this polling interval.
   * @param should_stop Borrowed stop predicate valid only for the call.
   * @return True when adapter stop became observable; false on normal expiry.
   * @throws Whatever the injected waiter or predicate throws; production
   *         additionally propagates `std::system_error` from mutex locking or
   *         condition-variable waiting.
   * @note Calls may block concurrently and must be interruptible by
   *       `wake_waiters`.
   */
  std::function<bool(std::chrono::steady_clock::time_point,
                     const std::function<bool()>& should_stop)>
      wait_until;

  /**
   * @brief Wakes every wait currently blocked in `wait_until`.
   *
   * Adapter destruction invokes this callback after publishing stop and
   * contains callback exceptions so cleanup remains nonthrowing.
   *
   * @return Nothing.
   * @throws Whatever an injected wake callback throws; production may throw
   *         `std::system_error` when locking its waiter mutex. Adapter
   *         destruction contains either source.
   * @note The callback may run concurrently with every blocked `wait_until`.
   */
  std::function<void()> wake_waiters;

  /**
   * @brief Private hook immediately before a validated async outcome publishes.
   *
   * Production supplies a no-op. Deterministic lifecycle tests may block here
   * so adapter stop can linearize first and prove that only the destructor
   * publishes client_stopped after wake and descriptor interruption.
   *
   * @return Nothing.
   * @throws Whatever an injected test hook throws; production does not throw.
   * @note Calls may execute concurrently on independent async workers and
   *       must not retain adapter-owned state beyond the call.
   */
  std::function<void()> before_async_completion;

  /**
   * @brief Converts one lease-protected artifact into an owned CPU image.
   *
   * @param metadata Strictly decoded daemon artifact metadata.
   * @return Read-only mapped image or an exact local Transport/Protocol
   *         failure.
   * @throws std::bad_alloc if failure-status diagnostics or shared mapping
   *         ownership cannot allocate.
   * @note The callback runs while the delivery lease still protects
   *       result-to-open. Production validates same-user file identity and
   *       returns a shared mapping whose final owner unmaps and closes once.
   */
  std::function<Result<ImageBuffer>(const OutputArtifactMetadata& metadata)>
      consume_artifact;
};

/**
 * @brief Validates and maps one lease-protected output artifact read-only.
 * @param metadata Strictly decoded daemon artifact metadata.
 * @param operations Complete nonthrowing POSIX mapping operation table.
 * @return Shared mapped CPU image or an exact local Transport/Protocol failure.
 * @throws std::bad_alloc if failure-status diagnostics or shared mapping
 *         ownership cannot allocate.
 * @note This source-tree-private seam exists so lifetime tests can count exact
 *       final-owner `munmap`/`close` calls without exposing POSIX details in an
 *       installed header. The caller must retain the delivery lease until this
 *       function returns.
 */
Result<ImageBuffer> consume_artifact_readonly_mapping(
    const OutputArtifactMetadata& metadata,
    ArtifactMappingOperations operations);

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
