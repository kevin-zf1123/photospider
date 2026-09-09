#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/status.hpp"
#include "photospider/data/value.hpp"
#include "photospider/execution/cancellation.hpp"
#include "photospider/plugin/operation_plugin_api.h"

namespace ps {

/**
 * @brief Local physical backend selected for one plan step.
 *
 * @note GPU names one optional in-process lane, never a remote device.
 * `OperationRegistry::invoke` rejects every unknown numeric representation
 * before capability selection or callback entry.
 */
enum class Backend : std::uint32_t {
  Cpu = 1,
  Gpu = 2,
};

/** @brief Closed compile-time output-shape inference rule. */
enum class OperationShapeRule : std::uint32_t {
  /** @brief Output is one scalar with shape `{1}`. */
  Scalar = 1U,
  /** @brief Output descriptor equals the first input descriptor. */
  PreserveFirstInput = 2U,
  /** @brief All input descriptors must match and output preserves them. */
  MatchAllInputs = 3U,
  /**
   * @brief Output uses the descriptor's explicit bounded logical fixed shape.
   * @note The rule does not require a dense byte product; C++ callbacks may
   * publish a valid strided or zero-stride broadcast Value.
   */
  Fixed = 4U,
  /** @brief Ceil-divide the first spatial input by a static factor. */
  Shrink = 5U,
};

/** @brief Closed compiler-visible Region propagation rule. */
enum class OperationRegionRule : std::uint32_t {
  /** @brief Operation requires and produces whole logical coverage. */
  Whole = 1U,
  /** @brief Output Region maps element-for-element from input Regions. */
  Elementwise = 2U,
  /** @brief Output Region reads an explicit symmetric input halo. */
  Halo = 3U,
  /** @brief Map output cells to their clipped factor-sized input boxes. */
  Shrink = 4U,
};

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

/** @brief Closed input/output semantic port vocabulary. */
enum class OperationPortKind : std::uint32_t {
  /** @brief Generic Value with ordinary shape and Region rules. */
  Value = 1,
  /** @brief Direct whole Float32 {1} declaration with no facets. */
  Float32Scalar = 2,
  /** @brief Dense Float32 {H,W,4} with exact linear premultiplied profile. */
  LinearPremultipliedRgbaFloat32 = 3,
  /** @brief Float32 {H,W}, no facets, finite [0,1] spatial mask. */
  Float32Mask = 4,
};
/**
 * @brief Copied compile-time port contract included in stage identities.
 * @note Scalars require a direct Float32 {1} workflow input, no facets and a
 * finite inclusive interval. Other kinds require positive-zero bound bits.
 * Image ports require dense Float32 HWC RGBA and the exact photospider.image
 * v1 payload rgba;linear-srgb;premultiplied;hwc (34 ASCII bytes, no NUL).
 * RGB is finite/nonnegative; alpha is finite in [0,1], and alpha zero requires
 * RGB zero. HDR RGB and signed zeros are accepted without normalization.
 * Registration validates combinations; image outputs preserve an image first
 * input, while scalar outputs and computed bounded scalar inputs are invalid.
 */
struct PHOTOSPIDER_API OperationPortConstraint final {
  /** @brief Closed port kind; scalar output is unsupported. */
  OperationPortKind kind = OperationPortKind::Value;
  /** @brief Finite inclusive binary32 lower bound for scalar ports. */
  float minimum = 0.0F;
  /** @brief Finite inclusive binary32 upper bound for scalar ports. */
  float maximum = 0.0F;
};

/**
 * @brief Immutable compiler-visible facts for one operation implementation.
 *
 * @note Traits are copied into semantic IR; callback/DSO identities are not.
 */
struct PHOTOSPIDER_API OperationTraits final {
  /** @brief Exact ordered input count. */
  std::uint32_t input_count = 0;
  /** @brief Equal inputs/parameters produce equal output bytes. */
  bool deterministic = true;
  /** @brief Callback has no externally visible side effect. */
  bool side_effect_free = true;
  /** @brief Required CPU implementation is available. */
  bool supports_cpu = true;
  /** @brief Optional local GPU implementation is available. */
  bool supports_gpu = false;
  /** @brief Recoverable GPU failure may execute the CPU implementation. */
  bool allows_cpu_fallback = false;
  /**
   * @brief Declared output-capacity bound, raised to packed bytes when
   * representable.
   * @note Non-densely-representable generic outputs use this bound with at
   * least one element. Workspace and potential transfers are accounted
   * separately; every controlled allocation must fit its complete working-set
   * reservation.
   */
  std::uint64_t estimated_bytes = 0;
  /** @brief Version of this complete semantic trait record. */
  std::uint32_t version = 6U;
  /** @brief Whether a derived result may enter a disposable local cache. */
  bool cacheable = true;
  /** @brief Static output type for scalar or descriptor validation. */
  ElementType output_element_type = ElementType::Float64;
  /** @brief Closed static output-shape inference behavior. */
  OperationShapeRule shape_rule = OperationShapeRule::Scalar;
  /** @brief Closed logical Region propagation behavior. */
  OperationRegionRule region_rule = OperationRegionRule::Whole;
  /** @brief Symmetric element halo, nonzero only for `Halo`. */
  std::uint32_t halo_radius = 0U;
  /** @brief Sorted closed parameter vocabulary for semantic validation. */
  std::vector<OperationParameterSpec> parameter_schema;
  /**
   * @brief Explicit nonzero rank-1..8 logical shape used only by `Fixed`.
   * @note C++ embedding callbacks may materialize it through any valid Value
   * layout, including a zero-stride broadcast whose dense product overflows.
   */
  std::vector<std::uint64_t> fixed_output_shape;
  /** @brief Exactly input_count ordered constraints, at most 1024. */
  std::vector<OperationPortConstraint> input_schema;
  /** @brief Value or image guarantee; image preserves the first image input. */
  OperationPortConstraint output_schema;
  /** @brief Fixed maximum scratch bytes per invocation, excluding output. */
  std::uint64_t workspace_bytes = 0;
  /** @brief Additional scratch bound per demanded input byte, in 0..16. */
  std::uint32_t workspace_input_multiplier = 0;
  /** @brief Required bounded Int64 parameter resolving a positive spatial halo.
   */
  std::string halo_radius_parameter = {};
  /** @brief Required bounded Int64 parameter for Shrink shape/Region rules. */
  std::string spatial_factor_parameter = {};
  /** @brief Resolved factor, 1..16; registry definitions must leave it one. */
  std::uint32_t spatial_factor = 1;
};

/**
 * @brief Maps an input edit to a conservative output dirty Region.
 * @param traits Resolved traits from the compiled node/plan step.
 * @param dirty Nonempty valid input edit Region.
 * @param input_shape Complete input shape.
 * @param output_shape Complete inferred output shape.
 * @param kind Ordered input port kind.
 * @return Checked output coverage or InvalidArgument/TypeMismatch.
 * @throws std::bad_alloc For metadata or diagnostic allocation.
 * @note Pure metadata; thread-safe. Whole/scalar changes dirty the full output.
 */
PHOTOSPIDER_API Result<Region> operation_dirty_region(
    const OperationTraits& traits, const Region& dirty,
    const std::vector<std::uint64_t>& input_shape,
    const std::vector<std::uint64_t>& output_shape, OperationPortKind kind);

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

/**
 * @brief Immutable invocation passed to a registered operation callback.
 *
 * @note Input Values, parameters, and token remain valid for the callback only.
 */
struct PHOTOSPIDER_API OperationInvocation final {
  /** @brief Creates a borrowed invocation; an empty output Region resolves to
   * Whole. */
  OperationInvocation(const std::vector<Value>& values,
                      const std::vector<Region>& demands,
                      const std::map<std::string, ParameterValue>& params,
                      Backend selected = Backend::Cpu,
                      CancellationToken token = {}, Region output = {},
                      BufferAllocator allocation = BufferAllocator())
      : inputs(values),
        input_demands(demands),
        parameters(params),
        backend(selected),
        cancellation(std::move(token)),
        output_region(std::move(output)),
        allocator(std::move(allocation)) {}
  /** @brief Ordered immutable input Values. */
  const std::vector<Value>& inputs;
  /** @brief Planned logical demand for each corresponding input Value. */
  const std::vector<Region>& input_demands;
  /** @brief Canonically ordered source parameters. */
  const std::map<std::string, ParameterValue>& parameters;
  /**
   * @brief Physical backend selected by the validated plan.
   * @note Only `Cpu` and `Gpu` are accepted at invocation.
   */
  Backend backend = Backend::Cpu;
  /** @brief Cooperative cancellation observation. */
  CancellationToken cancellation;
  /** @brief Exact logical output requested by this invocation. */
  Region output_region;
  /** @brief Host allocator for output and scratch, valid for callback duration.
   */
  BufferAllocator allocator;
  /** @brief Borrowed native services; valid only during this invocation. */
  const ps_gpu_service_v6* gpu = nullptr;
};

/** @brief Function signature for one synchronous operation invocation. */
using CallbackSignature = Result<Value>(const OperationInvocation&);
/** @brief Type-erased callable implementing `CallbackSignature`. */
using OperationCallback = std::function<CallbackSignature>;

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
  /** @brief Required synchronous implementation callback. */
  OperationCallback callback;
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
   * @note CPU support and a nonempty callback are mandatory. Fixed traits
   * validate only the logical descriptor; callback output validation applies
   * the published Value's actual layout and backing bytes. Passing an rvalue
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
   * @note Path rejection precedes the platform loader. Fixed C descriptors
   * must be densely representable because ABI v6 carries no output strides.
   * No signature, trust-store, sandbox, or process isolation is applied.
   */
  [[nodiscard]] Status load_plugin(const std::string& path);

  /**
   * @brief Makes the operation set read-only.
   * @return Success; repeated calls are idempotent.
   * @throws Nothing.
   * @note Compilation requires a frozen registry.
   */
  Status freeze() noexcept;

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

  /**
   * @brief Invokes one operation through its exception fence.
   * @param key Exact registered operation key.
   * @param invocation Borrowed invocation, validated before any callback.
   * @return Requested regional Value; `InvalidArgument` for a default input
   * Value, unknown backend, or malformed counts/demands/parameters;
   * `BackendUnavailable` for a known unsupported backend; `TypeMismatch` for
   * Preserve/Match input incompatibility or invalid generic callback output; or
   * the callback's typed failure.
   * @throws std::bad_alloc Only for process resource exhaustion before a
   * recoverable result can be constructed.
   * @note Validation preserves lookup, count, demand, parameter, cancellation,
   * backend-vocabulary, capability, and descriptor order. Every input is
   * checked for validity before descriptor access. The expected output
   * descriptor is computed before callback entry and reused afterward, so an
   * incompatible Preserve/Match invocation cannot run user or DSO code.
   * Callback exceptions other than bad_alloc become `OperationFailed`; a
   * standard exception with a null diagnostic becomes an empty message.
   * Output demand is checked against the Whole/image-channel rule. Supplied
   * demands must cover the output-derived requirement, including resolved
   * static halo and mask spatial shape, and input Values must cover those
   * demands. Invalid coverage never enters the callback or its allocator.
   * Image/scalar ports additionally validate regional metadata, facets,
   * finite scalar intervals and premultiplied pixel domains before callback
   * entry. Malformed computed image outputs are OperationFailed; explicit
   * cancellation/resource failures keep their categories. Image scopes save
   * and restore the thread floating environment for binary32 semantics.
   * Lookup copies only an immutable owning handle under the registry mutex;
   * callback copy/execution never runs there, and a DSO lease remains alive
   * through callback completion.
   */
  [[nodiscard]] Result<Value> invoke(
      const std::string& key, const OperationInvocation& invocation) const;

  /**
   * @brief Returns the sorted immutable operation-key inventory.
   * @return Exact key list.
   * @throws std::bad_alloc If result allocation fails.
   * @note Keys remain process configuration, not IPC values.
   */
  [[nodiscard]] std::vector<std::string> keys() const;
  /** @brief Verifiable built-in build identity, empty for custom/DSO
   * registries.
   * @note Immutable after construction; does not grant trust or sandboxing.
   * @throws std::bad_alloc For the returned copied string.
   */
  std::string persistent_cache_identity() const;

 private:
  friend class ExecutionContext;
  friend std::shared_ptr<OperationRegistry> make_default_operation_registry();
  bool builtins_ = false;
  /**
   * @brief Internal Run entry with periodic graph-currentness observation.
   * @param key Registered key.
   * @param invocation Immutable callback inputs and cancellation.
   * @param current Empty for direct embedding calls; otherwise Run currentness.
   * @return The public invoke result with Cancelled/Stale scan interruption.
   * @throws std::bad_alloc Under the same rules as invoke.
   * @note The probe is never retained in registry or compiled stage state.
   */
  [[nodiscard]] Result<Value> invoke_current(
      const std::string& key, const OperationInvocation& invocation,
      const std::function<bool()>& current) const;
  /** @brief Opaque synchronized registry and DSO ownership state. */
  struct Impl;
  /** @brief Unique private state. */
  std::unique_ptr<Impl> impl_;
};

/**
 * @brief Creates the maintained built-in operation set and freezes it.
 * @return Shared read-only registry containing constant, identity, add, and
 * delay, image exposure-gain, and image opacity operations.
 * @throws std::bad_alloc If construction fails.
 * @note The caller may instead assemble a custom registry before freezing.
 */
[[nodiscard]] PHOTOSPIDER_API std::shared_ptr<OperationRegistry>
make_default_operation_registry();

}  // namespace ps
