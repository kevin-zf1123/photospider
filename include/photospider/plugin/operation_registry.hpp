#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/cancellation.hpp"
#include "photospider/core/status.hpp"
#include "photospider/data/tensor_layout.hpp"
#include "photospider/data/tensor_span.hpp"
#include "photospider/plugin/fixed_invocation.hpp"
#include "photospider/plugin/native_kernel_api.h"
#include "photospider/plugin/operation_extensions.hpp"
#include "photospider/plugin/operation_program.hpp"

namespace ps {

/** @brief Closed source-parameter type vocabulary published by an operation. */
enum class OperationParameterType : std::uint32_t {
  /** @brief Exact signed 64-bit integer parameter. */
  Int64 = 1U,
  /** @brief Exact IEEE binary64 parameter without integer coercion. */
  Float64 = 2U,
  /** @brief Exact Boolean parameter. */
  Bool = 3U,
  /** @brief Bounded source string parameter. */
  String = 4U,
};

/**
 * @brief One canonical compiler-visible operation parameter declaration.
 *
 * @note Registry publication sorts declarations by key and rejects duplicate,
 * unknown-type, or malformed declarations before compiler visibility.
 */
struct PHOTOSPIDER_API OperationParameterSpec final {
  /** @brief Nonempty bounded strict UTF-8 parameter key. */
  std::string key;
  /** @brief Exact required `ParameterValue` alternative. */
  OperationParameterType type = OperationParameterType::Int64;
  /** @brief Whether semantic lowering requires the key to be present. */
  bool required = true;
  /** @brief Whether a numeric parameter must be finite and within the interval.
   */
  bool bounded = false;
  /** @brief Inclusive bounds; bounded Int64 endpoints are exact integers in
   * +/-2^53-1. */
  double minimum = 0;
  double maximum = 0;
};

/**
 * @brief Copied compile-time port contract included in stage identities.
 * @note The Result schema is the port value. These predicates validate its
 * tensor descriptor before callback entry.
 * Tensor predicates constrain dtype, rank and member identity. The compiler
 * includes the resolved schema in stage identity.
 */
struct PHOTOSPIDER_API OperationPortConstraint final {
  /** @brief Required dtype when nonzero; zero accepts any valid dtype. */
  std::uint32_t element_type = 0;
  /** @brief Required rank when nonzero. */
  std::uint32_t rank = 0;
  /** @brief Allowed dtype set, bit (element code - 1); zero adds no
   * restriction.
   * @note Only low seven bits are valid; nonzero element_type is mutually
   * exclusive.
   */
  std::uint32_t element_type_mask = 0;
  /** @brief Fixed Result schema identity. An empty identity accepts any input
   * schema matching the tensor-member predicate. On C++ outputs, an empty id
   * with version zero requires metadata specialization and a tensor-member
   * predicate; named members are allowed, while omitted tensor_key requires
   * one tensor. C ABI output schemas remain fixed.
   */
  std::string result_schema_id = {};
  std::uint32_t result_schema_version = 0;
  /** @brief Named Result tensor member constrained by dtype/rank above.
   * Empty with a dtype/rank predicate selects the sole tensor member;
   * multiple tensors require a key. Without member predicates the complete
   * fixed representation is selected. An omitted schema identity accepts any
   * valid representation satisfying the member predicate. For output
   * constraints, an empty identity may be resolved from a complete metadata
   * specialization prototype; input constraints use it to accept any matching
   * input schema.
   */
  std::string tensor_key = {};
};

/** @brief Static contract for one named result, independent of sibling outputs.
 * @note A missing input projection means all declared inputs; an explicitly
 * empty projection means no runtime inputs. Indices refer to the common schema.
 * Metadata inference always receives complete input metadata. Pure data records
 * are copied at registration and may be read concurrently afterwards.
 */
struct PHOTOSPIDER_API OperationOutputTraits final {
  /** @brief Unique strict UTF-8 result key, 1..128 bytes. */
  std::string key = "value";
  /** @brief Optional ordered original input-port projection for evaluation. */
  std::optional<std::vector<std::uint32_t>> input_indices;
  /** @brief Result port and tensor-member constraints. */
  OperationPortConstraint output_schema;
  /** @brief Complete structured Result output prototype, resolved by compiler.
   * Tensor descriptors define dtype, shape and structural grouping.
   * For metadata-derived C++ outputs,
   * the prototype remains complete; the output constraint may leave schema
   * id/version empty only when metadata specialization is required and a
   * tensor-member predicate is present. Inference returns the concrete schema,
   * which replaces the prototype in resolved traits. Fixed-schema outputs
   * retain their id/version.
   */
  std::optional<SchemaTemplate> result_schema = {};
};

/**
 * @brief Immutable compiler-visible facts for one operation implementation.
 *
 * @note Traits are copied into semantic IR; callback/DSO identities are not.
 */
struct PHOTOSPIDER_API OperationTraits final {
  /** @brief Bounded report types that this operation may emit. */
  std::vector<ExtensionIdentity> diagnostics;
  /** @brief Exact input count, or fixed prefix count for a repeated template.
   */
  std::uint32_t input_count = 0;
  /** @brief Equal inputs/parameters produce equal output bytes. */
  bool deterministic = true;
  /** @brief Callback has no externally visible side effect.
   * Externally visible work belongs to the call's terminal effect task.
   * Lowering must declare exactly one such task when this flag is false.
   */
  bool side_effect_free = true;
  /** @brief CPU implementation is available. At least one backend is required.
   */
  bool supports_cpu = true;
  /** @brief Local GPU implementation is available. May be GPU-only. */
  bool supports_gpu = false;
  /** @brief Version of this complete semantic trait record. */
  std::uint32_t version = 30U;
  /** @brief Registered template requires pure per-node metadata resolution.
   * Free inference rejects templates. OperationRegistry::prepare_operation
   * clears this flag only after validated specialization.
   */
  bool requires_metadata_specialization = false;
  /** @brief Sorted closed parameter vocabulary for semantic validation. */
  std::vector<OperationParameterSpec> parameter_schema;
  /** @brief Ordered constraints; a repeated template has prefix+one record. */
  std::vector<OperationPortConstraint> input_schema;
  /** @brief Optional trailing homogeneous group; input_count is fixed prefix.
   * @note With maximum>0, input_schema has prefix+one template. Lowering
   * expands it and sets repeated_resolved; the published registry keeps its
   * template.
   */
  /** @brief Active groups require minimum>=1; zero means no group. */
  std::uint32_t repeated_minimum = 0;
  std::uint32_t repeated_maximum = 0;
  std::uint32_t repeated_resolved = 0;
  /** @brief Require repeated inputs to share dtype and logical shape. */
  bool repeated_match = true;
  /** @brief Ordered named results; registry copies and validates all records.
   */
  std::vector<OperationOutputTraits> outputs = {OperationOutputTraits{}};
};

/** @brief Copies one selected result contract while retaining common metadata.
 * @param traits Registry or resolved operation record, never mutated.
 * @param output_index Declaration-order result index.
 * @return A record with one result, or InvalidArgument for an absent result.
 * @throws std::bad_alloc On copied metadata. Pure and thread-safe.
 */
PHOTOSPIDER_API Result<OperationTraits> select_operation_output(
    const OperationTraits& traits, std::uint32_t output_index);
/** @brief Infers all declared outputs in declaration order, without callbacks.
 * @param traits Resolved common and output traits.
 * @param inputs Complete ordered static metadata, including projected inputs.
 * @param parameters Validated static parameters.
 * @return Complete output metadata or the first typed inference failure.
 * @throws std::bad_alloc On metadata allocation. Pure and thread-safe.
 */
PHOTOSPIDER_API Result<std::vector<OperationMetadata>> infer_operation_outputs(
    const OperationTraits& traits, const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters);

/** @brief Expands an operation template and resolves its static parameters.
 * @param traits Validated registry template, copied and never modified.
 * @param input_count Actual input count, at most 1024.
 * @param parameters Exact validated static parameter values.
 * @return Resolved traits or InvalidArgument/TypeMismatch; no callback runs.
 * @throws std::bad_alloc On metadata allocation. Pure and thread-safe.
 */
PHOTOSPIDER_API Result<OperationTraits> resolve_operation_traits(
    const OperationTraits& traits, std::size_t input_count,
    const std::map<std::string, ParameterValue>& parameters);
/** @brief Resolves one Result schema against complete static input metadata.
 * @param traits Resolved traits returned by resolve_operation_traits.
 * @param inputs Complete statically known ordered input metadata.
 * @param parameters Validated static parameters, never retained.
 * @return Owned output metadata or typed validation/overflow failure.
 * @throws std::bad_alloc On metadata allocation. Pure and thread-safe.
 * @note Neither runtime bytes nor callback-specific inference affects shape.
 */
PHOTOSPIDER_API Result<OperationMetadata> infer_operation_output(
    const OperationTraits& traits, const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters);

/**
 * @brief Validates source parameters against one published operation schema.
 * @param traits Canonical registry-copied semantic traits.
 * @param parameters Canonically ordered source parameter map.
 * @return Success, or `InvalidArgument` for unknown, missing, or wrong-type
 * parameters and malformed/conflicting schema declarations.
 * @throws std::bad_alloc If a diagnostic allocation fails.
 * @note Validation performs no defaulting or numeric coercion and publishes no
 * source/IR state.
 */
[[nodiscard]] PHOTOSPIDER_API Status validate_operation_parameters(
    const OperationTraits& traits,
    const std::map<std::string, ParameterValue>& parameters);

/** @brief Owned per-node Result schema specialization.
 * Fixed-schema outputs preserve the registered schema id/version. A C++
 * metadata-derived output may resolve a concrete id/version when its registered
 * constraint leaves them empty, requires metadata specialization, and provides
 * a nonempty tensor-member predicate. The predicate may name a member or omit
 * tensor_key to require a sole tensor. Inference returns the concrete schema,
 * which the registry validates before compiler publication. C ABI output
 * schemas remain fixed. Specialization resolves static tensor shapes and
 * domains through the closed SchemaTemplate vocabulary. Input and output
 * metadata carry Result schemas. Specialization cannot alter inputs,
 * parameters or output names. Task scratch is declared during lowering.
 */
struct OperationOutputSpecialization final {
  OperationMetadata metadata;
  /** @brief Optional static-parameter-derived executable input projection.
   * Absent preserves the registered projection; an empty vector excludes all
   * runtime inputs. Indices are unique original ports in complete metadata.
   * Only registration-declared inputs may be retained. Resolved projections
   * enter existing compiler identities and drive demand and typed validation.
   */
  std::optional<std::vector<std::uint32_t>> input_indices;
};
/** @brief Pure, deterministic metadata inference with no TensorSpan or I/O
 * access. Input descriptors and static parameters are validated first. Return
 * one record per declared output, in registry order. Exceptions are fenced;
 * independent calls may execute concurrently. Returned metadata is owned.
 */
using OperationMetadataSpecializer = std::function<Result<std::vector<
    OperationOutputSpecialization>>(  // NOLINT(whitespace/indent_namespace)
    const std::vector<OperationMetadata>&,
    const std::map<std::string, ParameterValue>&)>;

/** @brief Deterministic static specialization and optional immutable program.
 * Returned state owns only data derived from input metadata/static parameters.
 * It must not contain mutable Run data, TensorSpan payloads, I/O state or
 * private caches. Its destructor is noexcept and may run on any thread.
 * Compilation owns preparation storage. Runtime scratch belongs to the
 * per-task physical Plan declarations.
 */
struct OperationPreparation final {
  /** @brief Explicit resource identities required by this prepared program. */
  std::vector<ResourceIdentity> resources;
  std::vector<OperationOutputSpecialization> outputs;
  std::shared_ptr<const void> state;
};
/** @brief Pure static preparation, called outside registry synchronization.
 * Same validation and exception contract as OperationMetadataSpecializer.
 * A successful explicit preparation runs this callback once; its owning handle
 * can serve multiple outputs, requests and dynamic executions without retries.
 */
using OperationPreparer = std::function<Result<OperationPreparation>(
    const std::vector<OperationMetadata>&,
    const std::map<std::string, ParameterValue>&)>;
/** @brief Registry-created immutable static program and resolved contracts.
 * No public constructor or mutable state is exposed. This handle retains its
 * definition/library until after the program destructor. It is safe to share
 * concurrently. Addresses and derived state never enter semantic
 * identity.
 */
class PHOTOSPIDER_API PreparedOperation final {
 public:
  PreparedOperation(const PreparedOperation&) = delete;
  PreparedOperation& operator=(const PreparedOperation&) = delete;
  PreparedOperation(PreparedOperation&&) = delete;
  PreparedOperation& operator=(PreparedOperation&&) = delete;
  /** @brief Expand one fixed physical subgraph during compilation only. */
  Result<OperationProgram> lower(
      const std::vector<TileRequest>& requests, std::uint64_t tile_extent,
      physical::Backend backend,
      const physical::ValidationLimits& limits) const;
  /** @brief Bind a direct CPU/GPU kernel after Plan publication.
   * Returned ownership retains static resources and the defining module.
   * program must be validated; outputs names this call's operation outputs
   * in declaration order. The callback verifies the fixed task signature.
   * budget admits host preparation metadata before allocation; kernel wrappers
   * retain their leases. Provider-owned opaque context allocations are
   * external.
   * Omitting budget constructs an independent default root and may throw
   * std::bad_alloc before the call; retained leases keep that root alive.
   */
  Result<PreparedKernel> prepare_kernel(
      const physical::Program& program, physical::Id task,
      const std::vector<physical::Id>& outputs,
      const ps_native_gpu_service_v2* native_gpu = nullptr,
      std::shared_ptr<const void> device_owner = {},
      const ResourceBudget& budget = ResourceBudget{}) const;
  /** @brief Complete resolved output contracts; valid for handle lifetime. */
  const OperationTraits& traits() const noexcept;
  /** @brief Borrowed immutable operation-defined program, possibly nullptr.
   * Runtime callbacks may borrow it while their owning session lives. The
   * producing registered operation alone defines its concrete type.
   */
  const void* state() const noexcept;
  /** @brief Explicit, canonical resource identities selected at preparation. */
  const std::vector<ResourceIdentity>& resource_ids() const noexcept;
  /** @brief Immutable report implementations retained independently of
   * registry. */
  const std::vector<std::shared_ptr<const DiagnosticDefinition>>&
  diagnostic_definitions() const noexcept;

 private:
  friend class OperationRegistry;
  struct Impl;
  explicit PreparedOperation(std::shared_ptr<const Impl> impl);
  std::shared_ptr<const Impl> impl_;
};

/**
 * @brief One complete operation definition before registry publication.
 *
 * @note Registration moves or copies the by-value input into one immutable
 * private owning record before publication. Later registry snapshots copy only
 * that record's owning handle and never recopy the callback under registry
 * synchronization.
 */
struct PHOTOSPIDER_API OperationDefinition final {
  /** @brief Nonempty unique strict UTF-8 operation key. */
  std::string key;
  /** @brief Immutable compiler-visible semantic traits. */
  OperationTraits traits;
  /** @brief Pure static task graph expansion, required for fixed execution. */
  OperationLowerer lower;
  /** @brief Kernel preparation with borrowed native services for GPU placement.
   * Success transfers one context owner to the registry for adoption.
   * Optional for a lowering that emits only COPY tasks or elided calls.
   * A kernel descriptor without this callback fails linking at prepare.
   */
  std::function<Result<ps_prepared_kernel_v8>(
      const physical::KernelDesc&, const physical::Placement&,
      const KernelPreparation&, const ResourceSet&,
      const ps_native_gpu_service_v2*)>
      prepare_kernel;
  /** @brief Required exactly for a metadata-specialized traits template. */
  OperationMetadataSpecializer specialize_metadata = {};
  /** @brief Pure static metadata preparation for this operation definition.
   * Mutually exclusive with specialize_metadata; when present,
   * requires_metadata_specialization must be true. Supported only for
   * deterministic, side-effect-free operations.
   */
  OperationPreparer prepare_static = {};
  /** @brief Validate selected immutable resources once per preparation.
   * Called after static specialization and resource selection, before the
   * PreparedOperation is published, including when output demand is Empty.
   * state borrows the immutable prepare_static result, or is null when absent.
   * The references and borrowed bytes expire at callback return; retain an
   * owning resource handle when a longer byte lifetime is required.
   * resources contains only declared identities and remains owned by the
   * preparation. The callback must be deterministic and safe for concurrent
   * preparations. It may inspect
   * frozen resource bytes, but must not read sample payloads or perform I/O.
   * Failure rejects preparation; exceptions are fenced by the registry.
   * Lowering and per-task kernel preparation do not repeat this callback.
   */
  std::function<Status(const void* state, const ResourceSet& resources)>
      validate_resources;
};

/**
 * @brief Startup-configured registry for trusted in-process operations.
 *
 * @note Mutation is serialized and forbidden after `freeze()` succeeds.
 */
class PHOTOSPIDER_API OperationRegistry final {
 public:
  /**
   * @brief Creates an empty mutable registry.
   * @throws std::bad_alloc If private state allocation fails.
   * @note Register operations before compilation begins.
   */
  OperationRegistry();
  /** @brief Register a bounded report codec with the same ownership rules. */
  Status register_diagnostic(DiagnosticDefinition definition);

  /**
   * @brief Stops callbacks, destroys DSO records, and unloads libraries.
   * @throws Nothing.
   * @note Callers must ensure no invocation remains in flight at destruction.
   */
  ~OperationRegistry() noexcept;

  /**
   * @brief Forbids copying synchronized registry/DSO ownership.
   * @param other Source registry that cannot be copied.
   * @throws Nothing; the operation is deleted.
   * @note Use shared ownership of one frozen registry instead.
   */
  OperationRegistry(const OperationRegistry& other) = delete;
  /**
   * @brief Forbids copy assignment of synchronized registry/DSO ownership.
   * @param other Source registry that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Existing callbacks and native-library leases never transfer.
   */
  OperationRegistry& operator=(const OperationRegistry& other) = delete;
  /**
   * @brief Forbids moving registry state after callback addresses are bound.
   * @param other Source registry that cannot be moved.
   * @throws Nothing; the operation is deleted.
   * @note Shared ownership preserves stable registry identity instead.
   */
  OperationRegistry(OperationRegistry&& other) = delete;
  /**
   * @brief Forbids move assignment of registry and native-library state.
   * @param other Source registry that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Frozen identity and callback lifetimes therefore remain stable.
   */
  OperationRegistry& operator=(OperationRegistry&& other) = delete;

  /**
   * @brief Registers one built-in or embedding-supplied operation.
   * @param definition Complete owned definition.
   * @return Success or validation/duplicate/frozen failure.
   * @throws std::bad_alloc If registry allocation fails without mutation.
   * @note At least one backend and a fixed lowering callback are mandatory.
   * Kernel tasks also require prepare_kernel; COPY-only definitions do not.
   * Traits constrain static schemas. Prepared kernels validate their fixed
   * signatures before execution; samples remain unchecked. Passing an rvalue
   * transfers the callables into immutable registry ownership before locking.
   */
  [[nodiscard]] Status register_operation(OperationDefinition definition);

  /**
   * @brief Loads and validates one trusted in-process operation DSO.
   * @param path Exact startup-configured library path: 1..4096 bytes with no
   * embedded NUL.
   * @return Success, `InvalidArgument` for a malformed path or record,
   * `NotFound` when the exact valid path cannot be loaded, or another complete
   * ABI/descriptor validation failure.
   * @throws std::bad_alloc If staging allocation fails without publication.
   * @note Path rejection precedes the platform loader. The loader accepts
   * only ps_operation_module_get_api_v8 and its exact ABI 8 table layout.
   * Missing entry points fail TypeMismatch; malformed headers fail
   * InvalidArgument. Import validates every declaration before atomically
   * publishing operations with fixed lowering and kernel preparation hooks.
   * No signature, trust-store, sandbox, or process isolation is applied.
   */
  [[nodiscard]] Status load_plugin(const std::string& path);

  /**
   * @brief Makes the operation set read-only.
   * @return Success; repeated calls are idempotent.
   * @throws Nothing.
   * @note Compilation requires a frozen registry.
   */
  Status freeze();

  /**
   * @brief Reports whether registry mutation has ended.
   * @return True after `freeze()`.
   * @throws Nothing.
   * @note The state is safe for concurrent read observation.
   */
  [[nodiscard]] bool frozen() const noexcept;

  /**
   * @brief Finds copied semantic traits for one operation.
   * @param key Exact operation key.
   * @return Traits or `NotFound`.
   * @throws std::bad_alloc If a failure diagnostic allocation fails.
   * @note The returned value grants no callback or registry mutation access.
   */
  [[nodiscard]] Result<OperationTraits> find_traits(
      const std::string& key) const;
  /** @brief Validates complete static inputs and prepares an owning program.
   * Input schemas are copied before static callbacks. The callback and all
   * later lowering/kernel preparation share these immutable snapshots; caller
   * edits after return cannot change them. Caller mutation must not race this
   * method. Static parameters are stored by value, preserving floating bits.
   * No TensorSpan reads or registry mutation occur. The callback runs once
   * outside the registry lock; metadata-only definitions receive an empty
   * program. Allocation failure is ResourceExhausted; callback exceptions are
   * fenced as OperationFailed. Returned state and copied static metadata are
   * preparation storage, shared by all selected outputs and concurrent Runs.
   * Different static metadata or parameters require a new preparation.
   * Dynamic payloads and selected output tiles are bound in later stages.
   */
  [[nodiscard]] Result<std::shared_ptr<const PreparedOperation>>
  prepare_operation(const std::string& key,
                    const std::vector<OperationMetadata>& inputs,
                    const std::map<std::string, ParameterValue>& parameters,
                    const ResourceSet& resources = {}) const;

  /**
   * @brief Returns the sorted immutable operation-key inventory.
   * @return Exact key list.
   * @throws std::bad_alloc If result allocation fails.
   * @note Keys remain process configuration, not IPC values.
   */
  [[nodiscard]] std::vector<std::string> keys() const;

 private:
  /** @brief Opaque synchronized registry and DSO ownership state. */
  struct Impl;
  /** @brief Unique private state. */
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps
