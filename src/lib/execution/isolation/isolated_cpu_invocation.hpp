/**
 * @file isolated_cpu_invocation.hpp
 * @brief Declares the non-supervised isolated CPU process invocation vertical.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "execution/isolation/isolated_cpu_invocation_protocol.hpp"  // NOLINT(build/include_subdir)
#include "photospider/data/value.hpp"
#include "plugin/plugin_trust.hpp"      // NOLINT(build/include_subdir)
#include "runtime/resource_ledger.hpp"  // NOLINT(build/include_subdir)

namespace ps::execution {

/** @brief Fixed control descriptor installed in the freshly execed runtime. */
inline constexpr int kIsolatedCpuRuntimeControlDescriptor = 3;

/**
 * @brief Local process/transport failure outside a valid plugin outcome.
 * @throws std::bad_alloc when retaining the diagnostic exhausts memory.
 * @note This non-supervised slice exposes no crash/hang/OOM taxonomy;
 * `PluginRuntimeSupervisor` supplies authenticated bounded classification.
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
 * @brief Explicit Host-owned plan for one fresh positive-stride CPU output.
 * @throws std::bad_alloc when copies of the complete DenseTensor descriptor,
 *         allocation-owning ImageFacet metadata, or layout cannot allocate.
 * @note Plan copies preserve every public ImageFacet field, including owned
 *       diagnostic strings and channel/group/sample vectors. The plan carries
 *       no allocation, producer, readiness, Run, or ledger authority;
 *       `ValueBuilder` validates it again after the child returns.
 */
struct IsolatedCpuDenseTensorOutputPlan final {
  /** @brief Permanent operation output-port identity. */
  IsolatedCpuOpaqueId port_identity;
  /** @brief Host-minted invocation-local immutable plan identity. */
  IsolatedCpuOpaqueId plan_identity;
  /** @brief Permanent representation-Schema identity. */
  IsolatedCpuOpaqueId schema_identity;
  /** @brief Optional primary Facet identity; zero iff `image_facet` is absent.
   */
  IsolatedCpuOpaqueId facet_identity;
  /** @brief Permanent Strided Layout identity. */
  IsolatedCpuOpaqueId layout_identity;
  /** @brief Nonzero logical descriptor structural version. */
  std::uint64_t schema_version = 1U;
  /** @brief Nonzero layout structural version. */
  std::uint64_t layout_version = 1U;
  /** @brief Exact descriptor digest or all zero when unavailable. */
  IsolatedCpuSha256Digest descriptor_digest;
  /** @brief Exact logical-content digest or all zero when unavailable. */
  IsolatedCpuSha256Digest logical_content_digest;
  /** @brief Exact physical-Layout digest or all zero when unavailable. */
  IsolatedCpuSha256Digest layout_digest;
  /** @brief Logical whole-byte DenseTensor descriptor. */
  DenseTensorDescriptor descriptor;
  /** @brief Owned optional complete public ordinary-image interpretation. */
  std::optional<ImageFacet> image_facet;
  /** @brief Exact positive-stride producer layout. */
  StridedLayout layout;
  /** @brief Exact positive physical allocation bytes. */
  std::size_t storage_size = 0U;
  /** @brief Exact positive power-of-two allocation alignment. */
  std::size_t alignment = 1U;
  /** @brief Exact immutable logical output Region. */
  RegionSet region = RegionSet::whole();
};

/**
 * @brief Host-owned source binding metadata parallel to one input Value.
 * @throws std::bad_alloc when copied Region storage cannot allocate.
 * @note The identities are comparison keys only and carry no graph, Value,
 * allocation, capability, or process authority.
 */
struct IsolatedCpuInputBinding final {
  /** @brief Permanent operation input-port identity. */
  IsolatedCpuOpaqueId port_identity;
  /** @brief Invocation-local nonzero upstream-edge identity. */
  IsolatedCpuOpaqueId edge_identity;
  /** @brief Permanent representation-Schema identity. */
  IsolatedCpuOpaqueId schema_identity;
  /** @brief Optional primary Facet identity; zero iff the Value has none. */
  IsolatedCpuOpaqueId facet_identity;
  /** @brief Permanent Strided Layout identity. */
  IsolatedCpuOpaqueId layout_identity;
  /** @brief Nonzero logical descriptor structural version. */
  std::uint64_t schema_version = 1U;
  /** @brief Nonzero layout structural version. */
  std::uint64_t layout_version = 1U;
  /** @brief Exact descriptor digest or all zero when unavailable. */
  IsolatedCpuSha256Digest descriptor_digest;
  /** @brief Exact logical-content digest or all zero when unavailable. */
  IsolatedCpuSha256Digest logical_content_digest;
  /** @brief Exact physical-Layout digest or all zero when unavailable. */
  IsolatedCpuSha256Digest layout_digest;
  /** @brief Exact immutable validity Region. */
  RegionSet region = RegionSet::whole();
};

/**
 * @brief High-level Host request converted into one pointer-free invocation.
 * @throws std::bad_alloc when copied operation/parameter/output storage,
 *         including complete output-plan ImageFacet metadata, cannot allocate.
 * @note Inputs remain Host-owned Ready Values. Their BufferHandle and process-
 * local identities never enter the wire.
 */
struct IsolatedCpuHostInvocation final {
  /** @brief Exact caller-retained identity tuple. */
  IsolatedCpuInvocationIdentity identity;
  /** @brief Bounded operation key interpreted only inside the runtime. */
  std::string operation;
  /** @brief Permanent operation identity selected by the Host. */
  IsolatedCpuOpaqueId operation_identity;
  /** @brief Permanent implementation identity selected by the Host. */
  IsolatedCpuOpaqueId implementation_identity;
  /** @brief Permanent configuration-Schema identity. */
  IsolatedCpuOpaqueId configuration_schema_identity;
  /** @brief Canonically name-sorted scalar parameters. */
  std::vector<IsolatedCpuScalarParameter> parameters;
  /** @brief Optional complete recursive canonical operation configuration. */
  std::vector<IsolatedCpuConfigurationNode> configuration;
  /** @brief Ready Host-visible Strided DenseTensor inputs. */
  std::vector<Value> inputs;
  /** @brief Destination-indexed metadata parallel to `inputs`. */
  std::vector<IsolatedCpuInputBinding> input_bindings;
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
 * @brief Host-composition policy driving aggregate admission and child rlimits.
 *
 * The address-space ceiling and CPU seconds are applied before descriptor exec.
 * `descriptor_overhead` accounts for fixed control, supervision, setup,
 * executable, standard-stream, and dynamic-loader working descriptors in
 * addition to invocation capability descriptors.
 *
 * @throws Nothing for aggregate value operations.
 * @note This policy carries no resource authority; only the injected
 * `ResourceLedger` can mint one-use invocation tokens.
 */
struct PluginInvocationResourcePolicy final {
  /** @brief Positive per-process address-space ceiling in bytes. */
  std::uint64_t address_space_bytes = 1ULL << 40U;

  /** @brief Positive per-process CPU-time ceiling in whole seconds. */
  std::uint64_t cpu_time_seconds = 30U;

  /** @brief Fixed Host descriptor overhead added with checked arithmetic. */
  std::uint64_t descriptor_overhead = 16U;
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
  /** @brief Validated permanent operation identity. */
  IsolatedCpuOpaqueId operation_identity;
  /** @brief Validated permanent implementation identity. */
  IsolatedCpuOpaqueId implementation_identity;
  /** @brief Validated permanent configuration-Schema identity. */
  IsolatedCpuOpaqueId configuration_schema_identity;
  /** @brief Validated canonical scalar parameters. */
  std::vector<IsolatedCpuScalarParameter> parameters;
  /** @brief Validated complete recursive configuration, when present. */
  std::vector<IsolatedCpuConfigurationNode> configuration;
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
 * is a source-private runtime seam, not the in-process operation ABI; the
 * isolation protocol transports only separately encoded pointer-free records.
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
 * heartbeat, deadline, pool, restart, or general sandbox. A callback that
 * never returns can block the caller forever; `PluginRuntimeSupervisor`
 * supplies bounded lifecycle supervision. This direct route still requires a
 * signed sealed Linux runtime, one-use resource admission, and child rlimits.
 * This Issue #102 transport sub-role is compiled into the product archive but
 * is not selected by `ExecutionService`, `WorkerManager`, an embedded Host/CLI,
 * or another end-user route. It deliberately does not claim the target private
 * `PluginInvocationExecutor` role, whose supervised composition is separate.
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
   * @param resource_ledger Attempt-local Host authority that exclusively mints
   * one-use plugin resource tokens and must be nonnull.
   * @param resource_policy Positive address-space, CPU-time, and descriptor
   * bounds applied before descriptor-based exec.
   * @param limits Protocol-v2 bounds whose shared-memory, capability, and
   * descriptor values are nonzero and whose parameter value may be zero; all
   * values remain at or below their hard maxima.
   * @throws std::invalid_argument when authority, policy, path, or limits are
   * invalid.
   * @throws PluginTrustError when the executable is not signed for the
   * isolated-runtime role.
   * @throws std::system_error when `SIGCHLD` state cannot be queried.
   * @throws std::filesystem::filesystem_error when Linux exact-object path
   * normalization fails.
   * @throws std::bad_alloc when retained path, trust, or diagnostic storage
   * cannot allocate.
   * @throws Any other cached `PluginTrustPolicy::load` exception unchanged.
   * @note Construction authorizes and retains a private immutable runtime
   * snapshot; it creates no child, invocation capabilities, or ledger token.
   * Darwin and unsupported platforms fail with `ExactObjectUnsupported`
   * during this construction boundary.
   */
  explicit NonSupervisedIsolatedCpuInvocationExecutor(
      std::filesystem::path runtime_executable,
      std::shared_ptr<ResourceLedger> resource_ledger,
      PluginInvocationResourcePolicy resource_policy = {},
      IsolatedCpuInvocationLimits limits = {});

  /**
   * @brief Executes one complete isolated CPU invocation synchronously.
   * @param invocation Host-owned inputs, plans, identity, operation, and
   * scalar parameters.
   * @return Typed callback outcome with fresh Values only on success.
   * @throws IsolatedCpuProtocolError for invalid local or returned content.
   * @throws PluginTrustError when invocation package identity differs from the
   * signed retained runtime.
   * @throws PluginResourceAdmissionError for replay or aggregate quota
   * rejection before materialization.
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
  /** @brief Absolute runtime path retained only for diagnostics/argv[0]. */
  std::filesystem::path runtime_executable_;

  /** @brief Attempt-local Host authority retained for every token lifecycle. */
  std::shared_ptr<ResourceLedger> resource_ledger_;

  /** @brief Validated immutable admission and child-limit policy. */
  PluginInvocationResourcePolicy resource_policy_;

  /** @brief Signed exact executable descriptor retained across invocations. */
  AuthorizedPluginFile authorized_runtime_;

  /** @brief Retained local protocol-v2 validation bounds. */
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
 * waits for that EOF and can block forever. This endpoint itself has no
 * authentication, heartbeat, deadline, restart, general sandbox, or
 * resource-mint authority; the Host validates trust/admission and applies
 * child rlimits before exec. The callback receives no wire pointer record, FD,
 * Host callback, Graph/Run owner, or cleanup token. It is compiled into the
 * product archive as a #102 runtime seam but no product composition root
 * launches it yet.
 */
int serve_non_supervised_isolated_cpu_invocation_once(
    int control_fd, const IsolatedCpuInvocationLimits& limits,
    const IsolatedCpuRuntimeCallback& callback) noexcept;

}  // namespace ps::execution
