#pragma once

#include <chrono>
#include <cstddef>
#include <string>

#include "photospider/ipc/protocol.hpp"

namespace ps::ipc::internal {

class Server;
struct ServerOptions;

/**
 * @brief Listener lifecycle points exposed only by the private source tree.
 *
 * @throws Nothing.
 * @note This type is not installed and does not alter daemon flags,
 * environment, protocol metadata, or wire behavior.
 */
enum class ServerLifecycleTestStage {
  /** @brief Bind created a pathname, before identity validation. */
  AfterBind,

  /** @brief Candidate scalar identity was captured, before `listen`. */
  AfterCandidateBeforeListen,

  /** @brief `listen` succeeded, before pathname self-proof. */
  AfterListenBeforeProof,

  /** @brief Pathname self-proof succeeded, before final revalidation. */
  AfterProofBeforeFinalRevalidate,

  /** @brief Cleanup is Active, before request-router runtime startup. */
  AfterActivationBeforeRuntimeStart,
};

/**
 * @brief Minimal deterministic dependencies for server lifecycle tests.
 *
 * @throws Nothing when copied.
 * @note Callbacks must not throw and must remain valid for the complete
 *       `test_create_listener()` or `test_run_server()` call. Production
 *       listener creation never constructs this dependency value.
 */
struct ServerLifecycleTestDependencies {
  /** @brief Opaque caller state forwarded to `stage_hook`. */
  void* context = nullptr;

  /**
   * @brief Optional non-throwing callback at a lifecycle boundary.
   * @param context Borrowed caller state.
   * @param stage Boundary reached by listener creation.
   * @param path Borrowed socket path valid only for this call.
   * @return Nothing.
   * @throws Nothing; implementations must capture their own failures.
   * @note The callback runs synchronously on the listener startup thread.
   */
  void (*stage_hook)(void* context, ServerLifecycleTestStage stage,
                     const char* path) noexcept = nullptr;

  /**
   * @brief Injectable `listen` boundary.
   * @param fd Bound listener descriptor.
   * @param backlog Requested bounded backlog.
   * @return The POSIX `listen` result, with `errno` set on failure.
   * @throws Nothing.
   * @note The callback runs synchronously before Candidate ownership is active.
   */
  int (*listen_call)(int fd, int backlog) noexcept = nullptr;

  /** @brief Maximum proof-frame bytes sent per state-machine iteration. */
  std::size_t proof_write_chunk_bytes = 0;

  /**
   * @brief Optional test-only total proof bytes after which writing stalls.
   * @throws Nothing when assigned.
   * @note Zero selects the complete fixed 36-byte proof frame. Values from one
   *       through 36 select a bounded prefix; larger values fail listener
   *       startup before any proof write rather than exceeding fixed storage.
   */
  std::size_t proof_write_limit_bytes = 0;

  /** @brief Optional observer for matching incomplete proof-prefix lengths. */
  void (*proof_prefix_observer)(void* context,
                                std::size_t observed_bytes) noexcept = nullptr;

  /** @brief Forces the maintained router-start failure cleanup path. */
  bool fail_runtime_start = false;

  /**
   * @brief Optional private override for every ordinary connection IO stage.
   * @throws Nothing when assigned.
   * @note A positive value replaces the fixed product budget only for the
   *       current `test_run_server()` call. Zero or a negative value preserves
   *       the product default. The duration is copied before workers start, so
   *       no worker borrows this test dependency.
   */
  std::chrono::milliseconds ordinary_connection_stage_timeout{0};

  /**
   * @brief Forces an early filesystem path-setup exception from server run.
   * @throws Nothing when assigned.
   * @note This non-installed failpoint verifies that the run-scoped dependency
   *       borrow is cleared before exception propagation and is never selected
   *       by production construction.
   */
  bool fail_path_setup = false;

  /**
   * @brief Optional observer invoked immediately before Active cleanup.
   * @param context Borrowed caller state.
   * @param listener_fd Listener descriptor that must still own the inode.
   * @param path Borrowed configured pathname.
   * @return Nothing.
   * @throws Nothing; implementations must capture failures in caller state.
   */
  void (*before_active_cleanup)(void* context, int listener_fd,
                                const char* path) noexcept = nullptr;
};

/**
 * @brief Exercises private listener creation with deterministic dependencies.
 *
 * @param path Absolute socket path below a protected test-owned directory.
 * @param dependencies Non-owning callbacks valid for the complete call.
 * @return Success when bind, Candidate capture, listen, pathname self-proof,
 *         final revalidation, and cleanup activation complete; otherwise the
 *         daemon-domain startup failure produced by the real listener path.
 * @throws std::bad_alloc if listener diagnostic or path storage cannot
 * allocate.
 * @throws std::filesystem::filesystem_error if parent inspection fails.
 * @throws std::runtime_error if `generate_opaque_id()` cannot obtain entropy.
 * @note Descriptors and the lifecycle lock are released before return. A
 *       successful active socket is identity-cleaned; every pre-activation
 *       failure preserves the current pathname. This non-installed seam is for
 *       maintained behavior tests and is never selected by `photospiderd`.
 */
OperationStatus test_create_listener(
    const std::string& path,
    const ServerLifecycleTestDependencies& dependencies);

/**
 * @brief Runs the real private server with deterministic lifecycle seams.
 *
 * @param server Sole server instance, which must be idle before the call.
 * @param options Explicit/default socket selection.
 * @param stop_fd Read end of the test-owned stop pipe.
 * @param dependencies Non-owning lifecycle seams valid until return.
 * @return The same startup/runtime status as `Server::run()`.
 * @throws std::bad_alloc if listener, runtime, worker, or diagnostic storage
 *         cannot allocate.
 * @throws std::filesystem::filesystem_error if socket parent inspection fails
 *         or `dependencies.fail_path_setup` selects the early exception path.
 * @throws std::runtime_error if default-path validation or listener proof-token
 *         entropy generation fails.
 * @throws std::system_error if a client worker thread cannot be created.
 * @note This source-tree-only seam changes no router, CLI, environment, wire,
 *       or installed surface. Pending clients captured during self-proof enter
 *       the real worker admission path only after runtime startup succeeds. A
 *       positive ordinary-stage timeout is copied before worker admission.
 *       The server drops every remaining dependency borrow on normal and
 *       exceptional exit before the caller may end `dependencies` lifetime.
 */
OperationStatus test_run_server(
    Server& server, const ServerOptions& options, int stop_fd,
    const ServerLifecycleTestDependencies& dependencies);

}  // namespace ps::ipc::internal
