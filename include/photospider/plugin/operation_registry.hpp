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
#include "photospider/core/status.hpp"
#include "photospider/data/planar_image.hpp"
#include "photospider/data/semantic.hpp"
#include "photospider/data/value.hpp"
#include "photospider/execution/cancellation.hpp"
#include "photospider/plugin/dependency_program.hpp"
#include "photospider/plugin/operation_plugin_api.h"
#include "photospider/plugin/result_program.hpp"

namespace ps {

/** @brief Closed compile-time output-shape inference rule. */
enum class OperationShapeRule : std::uint32_t {
  /** @brief Output is one scalar with shape `{1}`. */
  Scalar = 1U,
  /** @brief Output shape equals the first input shape, independently of dtype.
   */
  PreserveFirstInput = 2U,
  /** @brief All input shapes must match; output dtype is inferred
     independently. */
  MatchAllInputs = 3U,
  /**
   * @brief Output uses the descriptor's explicit bounded logical fixed shape.
   * @note The rule does not require a dense byte product; C++ callbacks may
   * publish a valid strided or zero-stride broadcast Value.
   */
  Fixed = 4U,
  /** @brief Ceil-divide the first spatial input by a static factor. */
  Shrink = 5U,
  /** @brief Independent static axis expressions, resolved before callbacks. */
  Axes = 6U,
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
  /** @brief Exact per-port requirements are resolved by the staged protocol. */
  Dependency = 5U,
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
  /** @brief Complete Float32 {1}, generic or dimensionless scalar/signal.
   * @note Runtime bounds apply before each consumer. Computed views may have
   * any valid layout; direct workflow bindings retain dense declarations.
   */
  Float32Scalar = 2,
  /** @brief Dense Float32 {H,W,4} with exact linear premultiplied profile. */
  RgbaFloat32 = 3,
  /** @brief Float32 {H,W} with canonical typed coverage and finite [0,1]. */
  Float32Mask = 4,
  /** @brief Generic typed semantic constraint; Whole or staged dependency
     demand. */
  Typed = 5,
  /** @brief A paged associated result, with fixed schema and runtime counts. */
  Result = 6,
};
/**
 * @brief Copied compile-time port contract included in stage identities.
 * @note Scalars require Float32 {1} and a finite inclusive interval. Facets
 * are absent or exactly one dimensionless Scalar/single-sample Signal; the
 * signal's sampling-axis domain/unit is retained independently of sample units.
 * Direct bindings retain dense preflight; computed views use logical
 * addressing. Other kinds require positive-zero bound bits. RgbaFloat32 uses
 * the canonical photospider.image v2 descriptor returned by rgba_semantics();
 * Float32Mask uses typed coverage_semantics(). Typed ports constrain kind/exact
 * facets, dtype and rank, using Whole or a staged dependency program. Output
 * facets are inferred independently from dtype/shape. Invalid computed numbers
 * fail OperationFailed before consumer entry, including cache hits; direct
 * numeric binding errors remain InvalidArgument.
 */
struct PHOTOSPIDER_API OperationPortConstraint final {
  /** @brief Closed port kind; scalar output is unsupported. */
  OperationPortKind kind = OperationPortKind::Value;
  /** @brief Finite inclusive binary32 lower bound for scalar ports. */
  float minimum = 0.0F;
  /** @brief Finite inclusive binary32 upper bound for scalar ports. */
  float maximum = 0.0F;
  /** @brief Required semantic kind for Typed; zero accepts any typed kind. */
  std::uint32_t semantic_kind = 0;
  /** @brief Exact semantic payload for Typed; empty accepts its kind. */
  std::vector<ValueFacet> facets = {};
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
  /** @brief Required fixed schema identity for Result ports; empty otherwise.
   */
  std::string result_schema_id = {};
  std::uint32_t result_schema_version = 0;
};

/** @brief Output dtype selection, independent of output shape. */
enum class OperationDtypeRule : std::uint32_t {
  Declared = 0,
  Input = 1,
  Parameter = 2,
  /** @brief Int64 for an Int64 source, otherwise Float64 for a floating source.
   * Uses output_dtype_input; UInt8 is rejected. C++ metadata inference only.
   */
  WidenNumericInput = 3
};
/** @brief Statically available axis-length sources. */
enum class OperationExtentSource : std::uint32_t {
  Constant = 0,
  Parameter = 1,
  InputAxis = 2,
  InputCount = 3,
  /** @brief Length of a canonical channel-index String parameter. */
  IndexListCount = 4,
  /** @brief Ceil of a bounded nonnegative Float64 parameter. */
  CeilParameter = 5
};
/** @brief Checked positive extent plus a nonnegative constant offset. */
struct PHOTOSPIDER_API OperationExtent final {
  OperationExtentSource source = OperationExtentSource::Constant;
  std::uint64_t constant = 1;
  std::string parameter;
  std::uint32_t input = 0;
  std::uint32_t axis = 0;
  std::uint64_t offset = 0;
  /** @brief Subtract a nonnegative Int64 parameter before ceil division. */
  std::string subtract_parameter = {};
  /** @brief Positive divisor and multiplier, applied before offset. */
  std::uint64_t divisor = 1;
  std::uint64_t multiplier = 1;
};
/** @brief Explicit output semantic behavior; transformations use static
 * metadata. */
enum class OperationSemanticRule : std::uint32_t {
  /** @brief Remove typed semantics; incompatible with a typed output port.
   * @note Registration rejects Drop with RgbaFloat32, Float32Mask or Typed.
   */
  Drop = 0,
  PreserveInput = 1,
  Establish = 2,
  Parameter = 3,
  /** @brief Select one HWC channel as HW field/coverage; Int64 parameter. */
  ExtractChannel = 4,
  /** @brief Select HWC channels by canonical index-list String. */
  SwizzleChannels = 5,
  /** @brief Establish explicit HWC semantics from HW channels; String target.
   */
  MergeChannelsParameter = 6,
  /** @brief RGB straight to coverage-premultiplied, retaining channel order. */
  AssociateAlpha = 7,
  /** @brief RGB coverage-premultiplied to straight, retaining channel order. */
  UnassociateAlpha = 8,
  /** @brief Linear sRGB D65 to canonical XYZ with the same white/alpha. */
  RgbToXyz = 9,
  /** @brief D65 XYZ to canonical linear sRGB with the same white/alpha. */
  XyzToRgb = 10,
  /** @brief XYZ to canonical Lab, retaining the explicit reference white. */
  XyzToLab = 11,
  /** @brief Lab to canonical XYZ, retaining the explicit reference white. */
  LabToXyz = 12,
  /** @brief Bounded expression -> dimensionless Float32 sampled signal.
   * Requires generic Float64[K], K in [1,256], and required Float64 start/step
   * parameters. The expression String uses output_semantic_parameter; the
   * inferred rank-one output count is in [1,1048576].
   */
  SampleExpression = 13,
  /** @brief Validates Float32 query Signal + one-channel Signal/Lut table.
   * Query samples use the table axis unit; table N>=2 and representable
   * increasing domain. The String parameter selects reject/clip outside it.
   * Output preserves query shape and drops semantic guarantees.
   */
  ApplyLut1d = 14,
  /** @brief Establish a YCbCr ImagePlane template from linear sRGB D65 without
   * alpha, preserving scene/display reference and white. output_facets contains
   * one plane descriptor with the desired role and nominal sampling geometry.
   */
  YCbCrPlane = 15
};

/** @brief Static contract for one named result, independent of sibling outputs.
 * @note A missing input projection means all declared inputs; an explicitly
 * empty projection means no runtime inputs. Indices refer to the common schema.
 * Metadata inference always receives complete input metadata. Pure data records
 * are copied at registration and may be read concurrently afterwards.
 */
struct PHOTOSPIDER_API OperationOutputTraits final {
  /** @brief Structural image axes/groups; DAG tile size comes from plan. */
  std::optional<PlanarImageLayout> planar_layout = {};
  /** @brief Unique strict UTF-8 result key, 1..128 bytes. */
  std::string key = "value";
  /** @brief Optional ordered original input-port projection for evaluation. */
  std::optional<std::vector<std::uint32_t>> input_indices;
  /** @brief Static output type for scalar or descriptor validation. */
  ElementType output_element_type = ElementType::Float64;
  /** @brief Closed static output-shape inference behavior. */
  OperationShapeRule shape_rule = OperationShapeRule::Scalar;
  /** @brief Closed logical Region propagation behavior. */
  OperationRegionRule region_rule = OperationRegionRule::Whole;
  /** @brief Symmetric element halo, nonzero only for `Halo`. */
  std::uint32_t halo_radius = 0U;
  /**
   * @brief Explicit nonzero rank-1..8 logical shape used only by `Fixed`.
   * @note C++ embedding callbacks may materialize it through any valid Value
   * layout, including a zero-stride broadcast whose dense product overflows.
   */
  std::vector<std::uint64_t> fixed_output_shape;
  OperationPortConstraint output_schema;
  /** @brief Required bounded Int64 parameter resolving a positive spatial halo.
   */
  std::string halo_radius_parameter = {};
  /** @brief Required bounded Int64 parameter for Shrink shape/Region rules. */
  std::string spatial_factor_parameter = {};
  std::uint32_t spatial_factor = 1;
  /** @brief Dtype rule and selected input or required String parameter. */
  OperationDtypeRule output_dtype_rule = OperationDtypeRule::Declared;
  std::uint32_t output_dtype_input = 0;
  std::string output_dtype_parameter = {};
  /** @brief Rank-1..8 axis expressions, present only for Axes. */
  std::vector<OperationExtent> output_axes = {};
  /** @brief Output semantic inference, copied into every compiler identity. */
  OperationSemanticRule output_semantic_rule = OperationSemanticRule::Drop;
  /** @brief Source for preserve/extract/swizzle/alpha/color transformations.
   * @note Transformations require Whole or Dependency and statically known
   * compatible metadata. Swizzle emits generic output when selected roles
   * cannot form a valid descriptor.
   */
  std::uint32_t output_semantic_input = 0;
  std::vector<ValueFacet> output_facets = {};
  std::string output_semantic_parameter = {};
  bool requires_dense_output = false;
  ObservationKind observation_kind = ObservationKind::Atomic;
  /** @brief All relevant stages must implement the declared error delivery. */
  FailureDelivery failure_delivery = FailureDelivery::RequestFailureOnly;
  /** @brief Number of complete trailing axes in each generic Atomic tuple.
   * Zero retains ordinary generic-sample/image-pixel observations. Nonzero
   * requires CPU Whole or staged Atomic execution and no recognized image
   * facet. A partial request expands to its complete tuples; all axes grouped
   * uses the singleton observation domain {1}. Included in contract identities.
   */
  std::uint32_t atomic_trailing_axes = 0;
  /** @brief Optional actual new output payload bound for a CPU Whole or staged
   * view. Unset reserves requested element bytes. A set bound replaces that
   * dense lower bound; workspace, metadata and referenced owners remain
   * accounted. The allocator still enforces this bound and failures remain
   * sticky.
   */
  std::optional<std::uint64_t> maximum_output_payload_bytes = {};
  /** @brief Disjoint complete CPU dependency pieces in observation coordinates.
   * Coverage partitions the full inferred observation domain. Present pieces
   * permit a multi-observation Atomic session without row enumeration. The
   * program requests the complete static mapping once, then publishes its exact
   * requested coverage. Dynamic requests, checkpoints, GPU and joint callbacks
   * are excluded from this path.
   */
  std::optional<std::vector<DependencyMapPiece>> static_dependency_pieces = {};
  /** @brief Runs one normalized regional Atomic request without splitting it
   * into samples. Each dynamic Need stage supplies complete bounded atom rows.
   * Observations remain individual samples; successful certificates retain
   * exact associations. CPU staged, non-joint programs only; checkpoints and
   * pure-block services are unavailable. Included in all contract identities.
   */
  bool regional_atomic = false;
  /** @brief Preserves returned immutable views at ordinary/direct collectors.
   * New payload is admitted on actual allocation, bounded by requested bytes
   * unless maximum_output_payload_bytes replaces that bound. Borrowed owners
   * remain charged independently. CPU Whole or staged non-joint execution only.
   * Whole prefers a covering affine input owner, collecting only when needed.
   * This permits auto view/copy choices without dense precharge.
   */
  bool preserve_output_views = false;
  /** @brief CPU Whole requires one affine backing owner per active input.
   * Requires preserve_output_views. Compatible same-owner fragments may form
   * one view. If an input needs collection across owner
   * fragments, execution returns InvalidArgument/InvalidDomain ViewUnavailable
   * before callback. Direct calls already supply one Value per input. This
   * property is part of compiled identity; it does not limit typed validation.
   */
  bool requires_input_views = false;
  /** @brief Zero for synchronous callback, one for the staged read protocol. */
  std::uint32_t dependency_version = 0;
  /** @brief Host-allocated state bound and finite poll limit for staged code.
   */
  std::uint64_t continuation_bytes = 0;
  std::uint32_t maximum_dependency_stages = 0;
  /** @brief Alternative structured output template, resolved by the compiler.
   * Result ports use this schema; scalar dtype/shape fields remain defaults and
   * do not describe a placeholder Value. Requires structured protocol 2.
   */
  std::optional<SchemaTemplate> result_schema = {};
};

/**
 * @brief Immutable compiler-visible facts for one operation implementation.
 *
 * @note Traits are copied into semantic IR; callback/DSO identities are not.
 */
struct PHOTOSPIDER_API OperationTraits final {
  /** @brief CPU callback consumes and publishes structural planar image
   * windows. Legacy Value callbacks cannot claim image storage compliance. */
  bool planar_storage_capable = false;
  /** @brief Optional CPU joint contract version, zero disables grouping. */
  std::uint32_t joint_contract = 0;
  /** @brief Shared host-owned state capacity, charged once per group. */
  std::uint64_t joint_continuation_bytes = 0;
  /** @brief Additional shared scratch bound per joint poll. */
  std::uint64_t joint_workspace_bytes = 0;
  /** @brief Opts pure staged outputs into one internal block namespace.
   * DependencyPhase::block transitions must then be independent of selected
   * output metadata/index unless explicitly encoded in incoming state or mode.
   * The host keys all resolved output contracts, static parameters, input
   * metadata and current supplied bytes. Public outputs and certificates remain
   * independent. Retention is optional and budgeted; misses recompute,
   * including with caching disabled. This does not synchronize concurrent
   * producers or promise a single evaluation per Run. Available only to pure
   * Atomic dependency-v1 operations; default preserves output-scoped block
   * keys.
   */
  bool share_blocks_across_outputs = false;

  /** @brief Exact input count, or fixed prefix count for a repeated template.
   */
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
  std::uint32_t version = 17U;
  /** @brief Registered template requires pure per-node metadata resolution.
   * Free inference rejects templates. OperationRegistry::resolve_traits
   * clears this flag only after validated specialization.
   */
  bool requires_metadata_specialization = false;
  /** @brief Whether a derived result may enter a disposable local cache. */
  bool cacheable = true;
  /** @brief Sorted closed parameter vocabulary for semantic validation. */
  std::vector<OperationParameterSpec> parameter_schema;
  /** @brief Ordered constraints; a repeated template has prefix+one record. */
  std::vector<OperationPortConstraint> input_schema;
  /** @brief Fixed maximum scratch bytes per invocation, excluding output. */
  std::uint64_t workspace_bytes = 0;
  /** @brief Additional scratch bound per demanded input byte, in 0..16. */
  std::uint32_t workspace_input_multiplier = 0;
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
/** @brief Infers output dtype, shape and semantics through one shared contract.
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
   * @brief Actual physical backend for this attempt, including CPU fallback.
   * @note Only `Cpu` and `Gpu` are accepted at invocation.
   */
  Backend backend = Backend::Cpu;
  /** @brief Cooperative cancellation observation. */
  CancellationToken cancellation;
  /** @brief Exact logical output requested by this invocation. */
  Region output_region;
  /** @brief Original declaration-order selected output; defaults to value. */
  std::uint32_t output_index = 0;
  /** @brief Original input indices for supplied projected inputs. */
  std::vector<std::uint32_t> input_indices;
  /** @brief Complete static input metadata when runtime inputs are projected.
   * Empty selects descriptors from the complete supplied input vector.
   */
  std::vector<OperationMetadata> input_metadata;
  /** @brief Host allocator for output and scratch, valid for callback duration.
   */
  BufferAllocator allocator;
  /** @brief Borrowed native services; valid only during this invocation. */
  const ps_gpu_service_v9* gpu = nullptr;
  /** @brief Explicit immutable resource owners for static output identities.
   * Input Value owners are also admitted by invoke. No dynamic sample port.
   */
  ResourceBindings resources = {};
  /** @brief Optional immutable preparation for synchronous execution.
   * A supplied handle must match this registry, key, complete metadata and
   * exact static parameter bits; a preparation seal mismatch returns Stale
   * before the callback. Ordinary invocation validation can reject first.
   * The executor supplies the plan owner. Direct calls without a handle prepare
   * once. The normalized callback may borrow state() for the call lifetime.
   */
  std::shared_ptr<const PreparedOperation> prepared;
};

/** @brief Function signature for one synchronous operation invocation. */
using CallbackSignature = Result<Value>(const OperationInvocation&);
/** @brief Type-erased callable implementing `CallbackSignature`. */
using OperationCallback = std::function<CallbackSignature>;

/** @brief Borrowed call scope for a structural planar image operation. */
struct PHOTOSPIDER_API PlanarOperationInvocation final {
  const std::vector<PlanarImageReadWindow>& inputs;
  const std::vector<Region>& input_demands;
  const std::map<std::string, ParameterValue>& parameters;
  const Region& output_region;
  const PlanarImageWriteWindow& output;
  CancellationToken cancellation;
  /** @brief Host scratch allocator with the declared aggregate live bound. */
  BufferAllocator allocator;
  /** @brief Resolved, validated output metadata borrowed for this call. */
  const OperationMetadata& output_metadata;
};
/** @brief Callback writes only the requested output window; host publishes
 * that coverage after successful return and cancellation/current checks.
 */
using PlanarOperationCallback = std::function<Status(
    const PlanarOperationInvocation&)>;  // NOLINT(whitespace/indent_namespace)

/** @brief Owned per-node Value or structured Result metadata.
 * Result specialization requires protocol 2 and preserves the registered schema
 * id/version and Result port kind. It may resolve fields/domain/semantic
 * metadata through the validated closed SchemaTemplate vocabulary. Value-only
 * physical bounds/flags and tuple metadata must remain absent for a Result.
 * Specialization cannot alter input/parameter schemas, output names, callback
 * kinds, failure delivery or resource workspaces of the registered definition.
 */
struct OperationOutputSpecialization final {
  OperationMetadata metadata;
  bool regional_atomic = false;
  bool preserve_output_views = false;
  bool requires_input_views = false;
  std::optional<std::uint64_t> maximum_output_payload_bytes = {};
  std::optional<std::vector<DependencyMapPiece>> static_dependency_pieces = {};
  /** @brief Optional static-parameter-derived input projection for CPU Whole.
   * Absent preserves the registered projection; an empty vector excludes all
   * runtime inputs. Indices are unique original ports in complete metadata.
   * Only registration-declared inputs may be retained. Resolved projections
   * enter existing compiler identities and drive demand and typed validation.
   */
  std::optional<std::vector<std::uint32_t>> input_indices;
};
/** @brief Pure, deterministic metadata inference with no Value or I/O access.
 * Input descriptors and static parameters are validated first. Return one
 * record per declared output, in registry order. Exceptions are fenced;
 * independent calls may execute concurrently. Returned metadata is owned.
 */
using OperationMetadataSpecializer = std::function<Result<std::vector<
    OperationOutputSpecialization>>(  // NOLINT(whitespace/indent_namespace)
    const std::vector<OperationMetadata>&,
    const std::map<std::string, ParameterValue>&)>;

/** @brief Deterministic static specialization and optional immutable program.
 * Returned state owns only data derived from input metadata/static parameters.
 * It must not contain mutable Run data, Value payloads, I/O state or private
 * caches. Its destructor is noexcept and may run on any thread. Compilation
 * owns preparation storage separately from runtime continuation budgets.
 */
struct OperationPreparation final {
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
 * concurrently. Addresses and derived state never enter semantic/cache
 * identity.
 */
class PHOTOSPIDER_API PreparedOperation final {
 public:
  PreparedOperation(const PreparedOperation&) = delete;
  PreparedOperation& operator=(const PreparedOperation&) = delete;
  PreparedOperation(PreparedOperation&&) = delete;
  PreparedOperation& operator=(PreparedOperation&&) = delete;
  /** @brief Complete resolved output contracts; valid for handle lifetime. */
  const OperationTraits& traits() const noexcept;
  /** @brief Borrowed immutable operation-defined program, possibly nullptr.
   * Runtime callbacks may borrow it while their owning session lives. The
   * producing registered operation alone defines its concrete type.
   */
  const void* state() const noexcept;

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
  /** @brief Required synchronous implementation callback. */
  OperationCallback callback;
  /** @brief Alternative staged implementation; exactly one callback/start. */
  DependencyStart start_dependency = {};
  /** @brief Optional pure static validation, also applied to Empty queries. */
  DependencyValidator validate_dependency = {};
  /** @brief Optional Atomic joint implementation; singleton start remains
   * required. */
  DependencyJointStart start_joint = {};
  /** @brief Alternative structured stage protocol 2; exclusive with callbacks.
   */
  ResultProgramStart start_result = {};
  /** @brief Required exactly for a metadata-specialized traits template. */
  OperationMetadataSpecializer specialize_metadata = {};
  /** @brief Pure static Value/dependency-v1 preparation; mutually exclusive
   * with specialize_metadata and requires_metadata_specialization must be true.
   * Supported only for deterministic side-effect-free operations.
   */
  OperationPreparer prepare_static = {};
  /** @brief Exclusive structural image callback when planar_storage_capable. */
  PlanarOperationCallback planar_callback = {};
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
   * @note Path rejection precedes the platform loader. Synchronous Fixed C
   * sinks require a dense domain. Staged C programs validate actual output
   * fragments, without requiring the entire logical domain to fit densely.
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
  /** @brief Starts a validated optional CPU joint group; unsupported returns
   * BackendUnavailable. Members are copied; allocator owns shared state once.
   */
  Result<std::shared_ptr<DependencyJointSession>> start_joint(
      const std::string& key, std::vector<DependencyRequest> requests,
      const BufferAllocator& allocator = BufferAllocator{},
      std::function<Status(std::uint64_t)> consume_root_work = {}) const;

  [[nodiscard]] Result<OperationTraits> find_traits(
      const std::string& key) const;
  /** @brief Resolves a registered template against complete static inputs.
   * Performs parameter/port validation and pure specialization outside the
   * registry lock. No payload reads or registry mutation. Returns owning
   * traits usable by ordinary inference; malformed metadata is a Schema
   * failure, allocation exhaustion is ResourceExhausted, and callback
   * exceptions are OperationFailed. The definition lease spans the call.
   */
  [[nodiscard]] Result<OperationTraits> resolve_traits(
      const std::string& key, const std::vector<OperationMetadata>& inputs,
      const std::map<std::string, ParameterValue>& parameters) const;

  /** @brief Validates complete static inputs and prepares an owning program.
   * No Value reads or registry mutation occur. The callback runs once outside
   * the registry lock; metadata-only definitions receive an empty program.
   * Allocation failure is ResourceExhausted; callback exceptions are fenced as
   * OperationFailed. Returned state and copied static metadata are plan/direct
   * preparation storage, outside per-observation runtime scratch admission.
   * Reuse requires this exact registry/definition and bit-identical static
   * parameters/metadata; dynamic payloads, ROI and selected output are
   * excluded.
   */
  [[nodiscard]] Result<std::shared_ptr<const PreparedOperation>>
  prepare_operation(
      const std::string& key, const std::vector<OperationMetadata>& inputs,
      const std::map<std::string, ParameterValue>& parameters) const;

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

  /** @brief Starts one validated atomic observation or complete terminal query.
   * @note Uses the frozen definition and host allocator. Default request-only
   * failure delivery rejects multi-observation atomic starts before callbacks.
   * The returned handle owns state/definition; it has no upstream scheduler.
   * consume_root_work, when supplied by a host, precharges every issued work
   * unit against the current root before the operation runs. It must remain
   * valid through session retirement; failures are sticky and never refunded.
   */
  Result<std::shared_ptr<DependencySession>> start_dependency(
      const std::string& key, DependencyRequest request,
      const BufferAllocator& allocator = BufferAllocator{},
      std::function<Status(std::uint64_t)> consume_root_work = {}) const;

  /** @brief Starts a validated structured continuation in host-owned state.
   * Query metadata must match full compiler inference. Allocation and callback
   * exceptions are fenced, and a definition lease survives through retirement.
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
  /** @brief Verifiable built-in build identity, empty for custom/DSO
   * registries.
   * @note Immutable after freeze; successful extension clears the identity;
   * does not grant trust or sandboxing.
   * @throws std::bad_alloc For the returned copied string.
   */
  std::string persistent_cache_identity() const;

 private:
  friend class execution_internal::StructuredExecution;
  Result<ResultContinuation> start_result_compiled(
      const std::string& key, const ResultProgramQuery& query,
      const BufferAllocator& allocator,
      std::shared_ptr<std::atomic<ErrorCode>> failure) const;
  friend class ExecutionContext;
  /** @brief Host-only structural callback entry after plan/binding validation.
   * The registry still checks exact window authorization and parameters before
   * preparing output pages or invoking the callback. */
  Status invoke_planar(
      const std::string& key, const std::vector<PlanarImageReadWindow>& inputs,
      const std::vector<Region>& input_demands,
      const std::map<std::string, ParameterValue>& parameters,
      const Region& output_region, PlanarImage& output,
      const CancellationToken& cancellation = {},
      const BufferAllocator& allocator = BufferAllocator()) const;
  friend class Compiler;
  friend std::shared_ptr<OperationRegistry> make_default_operation_registry(
      bool);
  bool builtins_ = false;
  Status validate_prepared(
      const PreparedOperation& prepared, const std::string& key,
      const std::vector<OperationMetadata>& inputs,
      const std::map<std::string, ParameterValue>& parameters) const;
  Status validate_dependency_metadata(
      const std::string& key, const std::vector<OperationMetadata>& inputs,
      const std::map<std::string, ParameterValue>& parameters) const;

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
  Result<Value> invoke_dependency_current(
      const std::string& key, const OperationInvocation& invocation,
      const std::function<bool()>& current) const;
  /** @brief Opaque synchronized registry and DSO ownership state. */
  struct Impl;
  /** @brief Unique private state. */
  std::unique_ptr<Impl> impl_;
};

/**
 * @brief Creates the maintained built-in operation set.
 * @param freeze Whether to freeze immediately (default true). Pass false to
 * register embedding operations, then freeze before compilation/execution.
 * @return Shared registry owning all maintained built-in operations.
 * @throws std::bad_alloc If construction fails.
 * @note Adding custom operations clears the built-in persistent cache identity.
 * Mutation is serialized; freeze must precede concurrent execution.
 */
[[nodiscard]] PHOTOSPIDER_API std::shared_ptr<OperationRegistry>
make_default_operation_registry(bool freeze = true);

}  // namespace ps
