/**
 * @file isolated_cpu_invocation.hpp
 * @brief Declares the non-supervised isolated CPU process invocation vertical.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "execution/isolated_cpu_invocation_protocol.hpp"  // NOLINT(build/include_subdir)
#include "photospider/data/value.hpp"

namespace ps::execution {

/** @brief Fixed control descriptor installed in the freshly execed runtime. */
inline constexpr int kIsolatedCpuRuntimeControlDescriptor = 3;

/**
 * @brief Local process/transport failure outside a valid plugin outcome.
 * @throws std::bad_alloc when retaining the diagnostic exhausts memory.
 * @note This non-supervised slice exposes no crash/hang/OOM taxonomy; Issue
 * #103 owns authenticated supervision and bounded failure classification.
 */
class IsolatedCpuInvocationError : public std::runtime_error {
 public:
  /**
   * @brief Creates one Host-owned transport/process diagnostic.
   * @param message Stable local failure reason.
   * @throws std::bad_alloc when runtime-error storage cannot allocate.
   */
  explicit IsolatedCpuInvocationError(const std::string& message)
      : std::runtime_error(message) {}
};

/**
 * @brief Explicit Host plan for one fresh positive-stride CPU output.
 * @throws std::bad_alloc when copied descriptor/layout vectors allocate.
 * @note The plan carries no allocation, producer, readiness, Run, or ledger
 * authority. `ValueBuilder` validates it again after the child returns.
 */
struct IsolatedCpuDenseTensorOutputPlan final {
  /** @brief Logical whole-byte DenseTensor descriptor. */
  DenseTensorDescriptor descriptor;
  /** @brief Optional explicit image-axis interpretation. */
  std::optional<ImageFacet> image_facet;
  /** @brief Exact positive-stride producer layout. */
  StridedLayout layout;
  /** @brief Exact positive physical allocation bytes. */
  std::size_t storage_size = 0U;
};

/**
 * @brief High-level Host request converted into one pointer-free invocation.
 * @throws std::bad_alloc when copied strings or vectors allocate.
 * @note Inputs remain Host-owned Ready Values. Their BufferHandle and process-
 * local identities never enter the wire.
 */
struct IsolatedCpuHostInvocation final {
  /** @brief Exact caller-retained identity tuple. */
  IsolatedCpuInvocationIdentity identity;
  /** @brief Bounded operation key interpreted only inside the runtime. */
  std::string operation;
  /** @brief Canonically name-sorted scalar parameters. */
  std::vector<IsolatedCpuScalarParameter> parameters;
  /** @brief Ready Host-visible Strided DenseTensor inputs. */
  std::vector<Value> inputs;
  /** @brief Positive-stride output plans in callback order. */
  std::vector<IsolatedCpuDenseTensorOutputPlan> outputs;
};

/**
 * @brief Host result after child exit and complete output revalidation.
 * @throws std::bad_alloc when copied Values or diagnostic allocate.
 * @note Only Succeeded carries outputs. PluginFailed and Cancelled are typed
 * callback outcomes, not supervisor crash/hang classifications.
 */
struct IsolatedCpuHostInvocationResult final {
  /** @brief Exact closed callback outcome. */
  IsolatedCpuInvocationOutcome outcome =
      IsolatedCpuInvocationOutcome::PluginFailed;
  /** @brief Fresh Host-owned Values only after a successful invocation. */
  std::vector<Value> outputs;
  /** @brief Host-owned bounded plugin diagnostic. */
  std::string diagnostic;
};

/**
 * @brief Child-process-local mapped tensor view for one callback.
 * @throws std::bad_alloc when copied descriptor vectors allocate.
 * @note Pointers are created only after wire/FD validation and never cross the
 * socket. Exactly one of `input_data` and `output_data` is non-null. The first
 * byte is the descriptor range start; `descriptor.byte_offset` selects logical
 * coordinate zero.
 */
struct IsolatedCpuRuntimeTensor final {
  /** @brief Complete validated pointer-free descriptor copy. */
  IsolatedCpuTensorDescriptor descriptor;
  /** @brief Read-only range start for an input, otherwise null. */
  const std::byte* input_data = nullptr;
  /** @brief Writable range start for an output, otherwise null. */
  std::byte* output_data = nullptr;
  /** @brief Exact mapped descriptor range length. */
  std::size_t size = 0U;
};

/**
 * @brief Child-process-local invocation passed to one CPU callback.
 * @throws std::bad_alloc when copied request state allocates.
 * @note This C++ object graph is reconstructed wholly inside the child and is
 * not an ABI or wire record.
 */
struct IsolatedCpuRuntimeInvocation final {
  /** @brief Exact validated comparison identity. */
  IsolatedCpuInvocationIdentity identity;
  /** @brief Validated operation key. */
  std::string operation;
  /** @brief Validated canonical scalar parameters. */
  std::vector<IsolatedCpuScalarParameter> parameters;
  /** @brief Ordered immutable mapped inputs. */
  std::vector<IsolatedCpuRuntimeTensor> inputs;
  /** @brief Ordered exclusive mapped outputs. */
  std::vector<IsolatedCpuRuntimeTensor> outputs;
};

/**
 * @brief Process-local callback outcome before response construction.
 * @throws std::bad_alloc when diagnostic storage allocates.
 * @note A successful callback must have initialized every planned output.
 */
struct IsolatedCpuRuntimeCallbackResult final {
  /** @brief Success, plugin failure, or cooperative cancellation. */
  IsolatedCpuInvocationOutcome outcome =
      IsolatedCpuInvocationOutcome::PluginFailed;
  /** @brief Bounded-intended plugin diagnostic copied by the runtime. */
  std::string diagnostic;
};

/**
 * @brief Process-local CPU callback invoked after complete request validation.
 * @param invocation Callback-scoped mapped inputs, outputs, and scalar state.
 * @return Closed callback outcome; successful output bytes remain in mappings.
 * @throws Any exception; the runtime converts it to one bounded PluginFailed
 * response while all mappings and received descriptors remain owned.
 * @note The callback must not retain pointers or references after return. This
 * is a source-private runtime seam, not operation ABI v1/v2.
 */
using IsolatedCpuRuntimeCallback = std::function<
    IsolatedCpuRuntimeCallbackResult(  // NOLINT(whitespace/indent_namespace)
        const IsolatedCpuRuntimeInvocation&)>;

/**
 * @brief Synchronous fresh-exec Host adapter without supervisor guarantees.
 *
 * Each call creates a new process, sends one canonical request plus invocation
 * FDs, waits for one response and normal exit, validates output again, and
 * retires every local capability. There is deliberately no authentication,
 * heartbeat, deadline, pool, restart, sandbox, or enforceable resource policy.
 * A callback that never returns can block the caller forever; Issue #103 owns
 * bounded supervision and Issue #104 owns trust/resource enforcement.
 * This Issue #102 transport sub-role is compiled into the product archive but
 * is not selected by `ExecutionService`, `WorkerManager`, an embedded Host/CLI,
 * or another end-user route. It deliberately does not claim the target private
 * `PluginInvocationExecutor` role, whose supervised composition remains #103.
 *
 * @throws std::invalid_argument for invalid construction options.
 * @throws std::system_error when process-wide POSIX setup inspection fails.
 */
class NonSupervisedIsolatedCpuInvocationExecutor final {
 public:
  /**
   * @brief Validates and retains one runtime executable and local hard limits.
   * @param runtime_executable Existing regular executable launched with an
   * empty environment and fixed control descriptor 3.
   * @param limits Protocol-v1 bounds whose shared-memory, capability, and
   * descriptor values are nonzero and whose parameter value may be zero; all
   * values remain at or below their hard maxima.
   * @throws std::invalid_argument when the path or limits are invalid.
   * @throws std::system_error when `SIGCHLD` state cannot be queried.
   * @throws std::bad_alloc when retaining path state cannot allocate.
   * @note Path validation is operability only, not #104 trust admission.
   */
  explicit NonSupervisedIsolatedCpuInvocationExecutor(
      std::filesystem::path runtime_executable,
      IsolatedCpuInvocationLimits limits = {});

  /**
   * @brief Executes one complete isolated CPU invocation synchronously.
   * @param invocation Host-owned inputs, plans, identity, operation, and
   * scalar parameters.
   * @return Typed callback outcome with fresh Values only on success.
   * @throws IsolatedCpuProtocolError for invalid local or returned content.
   * @throws IsolatedCpuInvocationError for shared-memory, FD, spawn, exec,
   * channel, or child-exit failures.
   * @throws Value/readiness/access/allocation exceptions from Host input
   * preparation or fresh output publication.
   * @note The complete Host plan is validated before invocation-capability
   * shm/FD/mmap/fork effects. Local emergency cleanup may signal and
   * synchronously reap the exact child but supplies no bounded deadline or
   * hostile-code containment claim.
   */
  IsolatedCpuHostInvocationResult invoke(
      const IsolatedCpuHostInvocation& invocation) const;

  /**
   * @brief Returns the retained executable path.
   * @return Borrowed immutable path.
   * @throws Nothing.
   */
  const std::filesystem::path& runtime_executable() const noexcept {
    return runtime_executable_;
  }

  /**
   * @brief Returns retained validation limits by value.
   * @return Exact configured limits.
   * @throws Nothing.
   */
  IsolatedCpuInvocationLimits limits() const noexcept { return limits_; }

 private:
  /** @brief Operability-validated runtime path; no trust-admission meaning. */
  std::filesystem::path runtime_executable_;
  /** @brief Retained local validation bounds; no ledger-token meaning. */
  IsolatedCpuInvocationLimits limits_;
};

/**
 * @brief Receives, validates, executes, and answers one runtime invocation.
 * @param control_fd Connected framed Unix stream descriptor, normally fixed
 * fd 3.
 * @param limits Runtime-local hard bounds independent of Host declarations.
 * @param callback Nonempty process-local CPU callback.
 * @return Zero after one valid response is sent; nonzero after request,
 * descriptor, mapping, callback-response, or channel failure.
 * @throws Nothing; all exceptions are contained before process main returns.
 * @note The sender must close its write half after its exact request; decode
 * waits for that EOF and can block forever. This endpoint has no
 * authentication, heartbeat, deadline, restart, sandbox, or resource
 * enforcement. The callback receives no wire pointer record, FD, Host callback,
 * Graph/Run owner, or cleanup token. It is compiled into the product archive as
 * a #102 runtime seam but no product composition root launches it yet.
 */
int serve_non_supervised_isolated_cpu_invocation_once(
    int control_fd, const IsolatedCpuInvocationLimits& limits,
    const IsolatedCpuRuntimeCallback& callback) noexcept;

}  // namespace ps::execution
