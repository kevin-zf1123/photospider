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
#include "photospider/plugin/cpu_parallel_api.h"
#include "photospider/plugin/cpu_tiles_api.h"
#include "photospider/plugin/native_gpu_api.h"
#include "photospider/plugin/operation_extensions.hpp"
#include "photospider/plugin/result_program.hpp"

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
 * tensor descriptor and explicitly bound contracts before callback entry.
 * Tensor predicates constrain dtype, rank and explicit validators. The compiler
 * includes the resolved schema in stage identity.
 */
struct PHOTOSPIDER_API OperationPortConstraint final {
  /** @brief Explicit validators; order is significant and enters identity. */
  std::vector<ExtensionIdentity> validators;
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
  /** @brief Optional bound on newly owned output payload.
   * For Result publications, the host checks the complete Result's physical
   * owners, deduplicates them, and excludes backing still owned by exposed
   * inputs or authorized tensor grants. The bound keeps weak source references
   * and does not retain input payload. Each prefix publication is checked as a
   * whole Result. Workspace, metadata, and referenced resources remain
   * separately accounted. Exceeding the bound returns
   * ResourceExhausted/CapacityLimit. This publication check does not prevent a
   * trusted callback from allocating against the Root before publication.
   */
  std::optional<std::uint64_t> maximum_output_payload_bytes = {};
  /** @brief Host-allocated state bound and finite poll limit for staged code.
   */
  std::uint64_t continuation_bytes = 0;
  std::uint32_t maximum_dependency_stages = 0;
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

/** @brief Static contract for one shared continuation stage.
 * key is unique within the original call. Each block activation starts a fresh
 * producer, shared by that activation's output units. Input projection limits
 * the compiler's mapping; the Plan grants exact read authority before polling.
 * Result schema and bounds are fixed during preparation and enter operation
 * identity; no spill or recomputation occurs within an activation.
 */
struct OperationSharedStage final {
  std::string key;
  /** @brief Validators certify each newly visible tile commit before supply.
   * A requires_complete validator delays visibility until the static full
   * stage is available; it never expands an input Need. */
  std::vector<ExtensionIdentity> validators;
  SchemaTemplate result_schema;
  std::vector<std::uint32_t> input_indices;
  Backend backend = Backend::Cpu;
  std::uint64_t continuation_bytes = 0, workspace_bytes = 0;
  std::uint64_t maximum_work = 0, maximum_output_payload_bytes = 0;
  std::uint32_t maximum_dependency_stages = 0;
  bool mandatory = false;
};

/**
 * @brief Immutable compiler-visible facts for one operation implementation.
 *
 * @note Traits are copied into semantic IR; callback/DSO identities are not.
 */
struct PHOTOSPIDER_API OperationTraits final {
  /** @brief Bounded report types that this operation may emit. */
  std::vector<ExtensionIdentity> diagnostics;
  /** @brief CPU Result orchestration submits bounded computation stages to
   * the host CPU tile service. Coordinator and tile callbacks have separate
   * service permissions. Host range parallelism is also available to CPU polls.
   */
  bool cpu_staged_tiles = false;
  /** @brief Opts pure staged outputs into a shared internal block namespace.
   * `ResultProgramPhase::block` transitions must then be independent of
   * selected output index or output-specific metadata unless that distinction
   * is encoded in incoming state or mode. The host keys the operation and all
   * resolved output contracts, backend, execution mode, static parameters and
   * actual input metadata, then keys each block by kind, range, mode, incoming
   * state and currently supplied tensor content. Public outputs, checkpoints
   * and completed-result caches remain independent. A hit returns only block
   * state; current Needs and dependency evidence remain tied to the selected
   * output. Retention is optional and budgeted; misses recompute. This does not
   * synchronize concurrent producers or promise one evaluation per Run.
   * Available only to pure deterministic Result operations.
   * The default preserves output-scoped block keys.
   */
  bool share_blocks_across_outputs = false;

  /** @brief Exact input count, or fixed prefix count for a repeated template.
   */
  std::uint32_t input_count = 0;
  /** @brief Equal inputs/parameters produce equal output bytes.
   * @note Nonpure operations require mandatory Run-shared stages and must
   * disable completed-result caching.
   */
  bool deterministic = true;
  /** @brief Callback has no externally visible side effect.
   * Externally visible work belongs to a declared mandatory shared stage.
   */
  bool side_effect_free = true;
  /** @brief CPU implementation is available. At least one backend is required.
   */
  bool supports_cpu = true;
  /** @brief Local GPU implementation is available. May be GPU-only. */
  bool supports_gpu = false;
  /** @brief Allow CPU retry for a recoverable GPU `BackendUnavailable`.
   *
   * This requires both CPU and GPU implementations plus deterministic and
   * side-effect-free traits. A retry candidate must be an unqualified
   * `BackendUnavailable` with reason None, origin unspecified or Backend, and
   * scope unspecified or Group. Poll retry also requires a retry-safe attempt
   * with no published output, mandatory I/O, checkpoint or block callback,
   * native dispatch, or tensor I/O, and clear cancellation, stale-plan, and
   * host-stop checks. A separate host-service retry veto rejects replay after
   * later typed resource, protocol, callback, observer, or other nonretryable
   * failures while preserving the first reported status. Other errors do not
   * fall back. Root work and stage accounting are retained. Fallback-tainted
   * results do not enter optional checkpoint, block, or completed-result
   * caches.
   */
  bool allows_cpu_fallback = false;
  /** @brief Version of this complete semantic trait record. */
  std::uint32_t version = 29U;
  std::vector<OperationSharedStage> shared_stages;
  /** @brief Registered template requires pure per-node metadata resolution.
   * Free inference rejects templates. OperationRegistry::prepare_operation
   * clears this flag only after validated specialization.
   */
  bool requires_metadata_specialization = false;
  /** @brief Whether a derived result may enter a disposable local cache. */
  bool cacheable = true;
  /** @brief Sorted closed parameter vocabulary for semantic validation. */
  std::vector<OperationParameterSpec> parameter_schema;
  /** @brief Ordered constraints; a repeated template has prefix+one record. */
  std::vector<OperationPortConstraint> input_schema;
  /** @brief Maximum scratch bytes per invocation, excluding output.
   * Resolved traits include the checked static preparation increment.
   * Result callbacks account actual backing capacity separately from the
   * declared workspace bound.
   */
  std::uint64_t workspace_bytes = 0;
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
 * Result specialization requires protocol 2 and preserves the Result port kind.
 * Fixed-schema outputs preserve the registered schema id/version. A C++
 * metadata-derived output may resolve a concrete id/version when its registered
 * constraint leaves them empty, requires metadata specialization, and provides
 * a nonempty tensor-member predicate. The predicate may name a member or omit
 * tensor_key to require a sole tensor. Inference returns the concrete schema,
 * which the registry validates before compiler publication. C ABI output
 * schemas remain fixed. Specialization resolves static tensor shapes and
 * domains through the closed SchemaTemplate vocabulary. Input and output
 * metadata carry Result schemas. Specialization cannot alter inputs,
 * parameters, output names, callback kinds or workspaces.
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
 * Compilation owns preparation storage separately from runtime continuation
 * budgets.
 */
struct OperationPreparation final {
  /** @brief Explicit resource identities required by this prepared program. */
  std::vector<ResourceIdentity> resources;
  std::vector<OperationOutputSpecialization> outputs;
  /** @brief Optional complete replacement of registered stage prototypes. */
  std::optional<std::vector<OperationSharedStage>> shared_stages;
  std::shared_ptr<const void> state;
  /** @brief Additional runtime scratch bound derived only from static metadata
   * and parameter sizes. The host checked-adds this to workspace_bytes before
   * planning or direct invocation; overflow returns ResourceExhausted.
   * This reserves no compilation payload and performs no runtime work. Actual
   * construction, computation and cancellation remain inside runtime callbacks,
   * using their allocator and work services. The resolved bound participates in
   * operation identity and is shared by the definition's selected outputs.
   * Zero preserves the registered bound.
   */
  std::uint64_t additional_workspace_bytes = 0;
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
 * concurrently. Addresses and derived state never enter semantic/cache
 * identity.
 */
class PHOTOSPIDER_API PreparedOperation final {
  friend class ResultContinuation;

 public:
  PreparedOperation(const PreparedOperation&) = delete;
  PreparedOperation& operator=(const PreparedOperation&) = delete;
  PreparedOperation(PreparedOperation&&) = delete;
  PreparedOperation& operator=(PreparedOperation&&) = delete;
  /** @brief Invoke the owned pure mapping callback for a static plan unit.
   * Exceptions and floating environment changes are fenced. An empty callback
   * is a compilation error, never an empty dependency declaration.
   */
  Result<ResultStaticMapping> map_dependencies(
      const ResultStaticQuery& query) const;
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
  /** @brief Run the explicitly bound validators on host-granted data.
   * port is the original full input/output ordinal. Callbacks run without a
   * registry lock, with admitted scratch and cancellation fencing. Exceptions
   * become typed failures; no validation state is mutated.
   */
  Status validate_port(bool output, std::uint32_t port,
                       const ValidationContext& context) const;
  bool has_validators(bool output, std::uint32_t port) const noexcept;
  /** @brief Whether validation requires complete coverage of all tensor
   * members. */
  bool has_stage_validators(std::uint32_t stage) const noexcept;
  bool requires_complete_stage_validation(std::uint32_t stage) const noexcept;
  Status validate_stage(std::uint32_t stage,
                        const ValidationContext& context) const;
  bool requires_complete_validation(bool output,
                                    std::uint32_t port) const noexcept;

 private:
  friend class OperationRegistry;
  Status validate_inputs(
      const std::vector<OperationMetadata>& inputs,
      const std::map<std::string, ParameterValue>& parameters) const;
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
  /** @brief Static input and shared-stage mapping, called only by planning.
   * The baseline contract eagerly reads complete selected inputs. Operations
   * with narrower or segmented dependencies replace this callback explicitly.
   */
  OperationDependencyMapper map_dependencies = complete_input_dependencies;
  /** @brief Alternative structured stage protocol 2; exclusive with callbacks.
   */
  ResultProgramStart start_result = {};
  /** @brief Required exactly for a metadata-specialized traits template. */
  OperationMetadataSpecializer specialize_metadata = {};
  /** @brief Pure static metadata preparation for this operation definition.
   * Mutually exclusive with specialize_metadata; when present,
   * requires_metadata_specialization must be true. Supported only for
   * deterministic, side-effect-free operations.
   */
  OperationPreparer prepare_static = {};
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
  /** @brief Register a reentrant immutable validator before freeze.
   * Keys are unique across versions. Definitions are copied before locking;
   * malformed, duplicate or frozen registrations fail without partial changes.
   */
  Status register_validator(ValidationDefinition definition);
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
   * @note At least one backend and a Result callback protocol are mandatory. A
   * GPU-only operation rejects CPU planning and cannot enable CPU fallback.
   * Fixed traits validate the logical schema; publication validation checks
   * the Result schema, authorized coverage and backing bytes. Passing an rvalue
   * transfers the callable into immutable registry ownership before locking.
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
   * only the exact Result operation ABI 7 entry point and full table layout;
   * missing, older, and planar-only C entries are rejected. Result modules
   * publish validated tensor coverage through the structured contract.
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
   * No TensorSpan reads or registry mutation occur. The callback runs once
   * outside the registry lock; metadata-only definitions receive an empty
   * program. Allocation failure is ResourceExhausted; callback exceptions are
   * fenced as OperationFailed. Returned state and copied static metadata are
   * plan/direct preparation storage, outside per-observation runtime scratch
   * admission. Reuse requires this exact registry/definition and bit-identical
   * static parameters/metadata; dynamic payloads, ROI and selected output are
   * excluded.
   */
  [[nodiscard]] Result<std::shared_ptr<const PreparedOperation>>
  prepare_operation(const std::string& key,
                    const std::vector<OperationMetadata>& inputs,
                    const std::map<std::string, ParameterValue>& parameters,
                    const ResourceSet& resources = {}) const;

  /** @brief Starts a validated structured continuation in host-owned state.
   * Query metadata must match full compiler inference. The backend enum and
   * operation capability are checked first: an invalid enum returns
   * InvalidArgument, and an unsupported CPU/GPU capability returns
   * BackendUnavailable before a continuation is created. Allocation and
   * callback exceptions are fenced, and a definition lease survives through
   * continuation destruction. Cancellation is checked before and after the
   * operation start factory; cancellation observed after factory entry takes
   * precedence over allocator failure, factory status, or an invalid returned
   * continuation. Any returned state is destroyed before start returns. CPU
   * retry policy belongs to the structured executor, not this
   * direct start API.
   */
  Result<ResultContinuation> start_result(
      const std::string& key, const ResultProgramQuery& query,
      const BufferAllocator& allocator) const;

  /**
   * @brief Returns the sorted immutable operation-key inventory.
   * @return Exact key list.
   * @throws std::bad_alloc If result allocation fails.
   * @note Keys remain process configuration, not IPC values.
   */
  [[nodiscard]] std::vector<std::string> keys() const;

 private:
  friend class execution_internal::StructuredExecution;
  Result<ResultContinuation> start_result_compiled(
      const std::string& key, const ResultProgramQuery& query,
      const BufferAllocator& allocator,
      std::shared_ptr<std::atomic<ErrorCode>> failure) const;
  friend class ExecutionContext;
  friend class Compiler;
  Result<ResultProgramQuery> prepare_result_query(
      const std::string& key, const ResultProgramQuery& query) const;
  Status validate_prepared(
      const PreparedOperation& prepared, const std::string& key,
      const std::vector<OperationMetadata>& inputs,
      const std::map<std::string, ParameterValue>& parameters) const;

  /** @brief Opaque synchronized registry and DSO ownership state. */
  struct Impl;
  /** @brief Unique private state. */
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps
