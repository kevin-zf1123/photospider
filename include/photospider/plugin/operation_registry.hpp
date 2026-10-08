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
#include "photospider/execution/data_movement.hpp"
#include "photospider/plugin/cpu_parallel_api.h"
#include "photospider/plugin/cpu_tiles_api.h"
#include "photospider/plugin/native_gpu_api.h"
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
   * @note The rule does not require a dense byte product; internal tensor
   * backing may use a valid strided or zero-stride broadcast layout.
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

/** @brief Closed schema-constraint vocabulary for operation ports.
 * Current operation registration requires the `Result` kind for every input
 * and output. Other enumerators are not accepted as standalone port forms.
 */
enum class OperationPortKind : std::uint32_t {
  /** @brief Generic descriptor predicate applied to typed Result backing. */
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
 * @note The Result schema is the port value. These predicates validate its
 * tensor descriptor and semantic facets before callback entry. Typed ports
 * constrain kind, exact facets, dtype and rank; scalar constraints also apply
 * their finite inclusive interval. The compiler includes the resolved schema
 * in stage identity.
 */
struct PHOTOSPIDER_API OperationPortConstraint final {
  /** @brief Port schema kind; registered operations must select `Result`. */
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
  /** @brief Fixed Result schema identity. An empty identity accepts any input
   * schema matching the tensor-member predicate. On C++ outputs, an empty id
   * with version zero requires metadata specialization and a tensor-member
   * predicate; named members are allowed, while omitted tensor_key requires
   * one tensor. C ABI output schemas remain fixed.
   */
  std::string result_schema_id = {};
  std::uint32_t result_schema_version = 0;
  /** @brief Named Result tensor member constrained by dtype/rank/facets above.
   * Empty with a dtype/rank/facet predicate selects the sole tensor member;
   * multiple tensors require a key. Without member predicates the complete
   * fixed representation is selected. An omitted schema identity accepts any
   * valid representation satisfying the member predicate. For output
   * constraints, an empty identity may be resolved from a complete metadata
   * specialization prototype; input constraints use it to accept any matching
   * input schema.
   */
  std::string tensor_key = {};
  /** @brief Require a recognized semantic facet, including ColorArray. */
  bool requires_semantics = false;
  /** @brief Require a complete dimensionless Float32 scalar tensor with full
   * sample shape {1}; finite samples must satisfy [minimum,maximum] before
   * consumer entry, including cached producers. Signed/zero strides are valid.
   */
  bool scalar_bounds = false;
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
   * @note The descriptor need not have a representable dense byte product.
   * Internal tensor backing may use a valid strided or zero-stride broadcast
   * layout.
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
  /** @brief Optional bound on newly owned output payload.
   * For Result publications, the host checks the complete Result's physical
   * owners, deduplicates them, and excludes backing still owned by exposed
   * inputs or authorized tensor grants, including private backing prepared for
   * a structured Whole view. The bound keeps weak source references and does
   * not retain input payload. Each prefix publication is checked as a whole
   * Result. Workspace, metadata, and referenced resources remain separately
   * accounted. Exceeding the bound returns ResourceExhausted/CapacityLimit;
   * contract-2 joint execution scopes that failure to the member. This
   * publication check does not prevent a trusted callback from allocating
   * against the Root before publication.
   */
  std::optional<std::uint64_t> maximum_output_payload_bytes = {};
  /** @brief Legacy static dependency-map metadata.
   * Current operation registration rejects nonempty values; staged dependency
   * execution uses the Result Need and relation contracts instead.
   */
  std::optional<std::vector<DependencyMapPiece>> static_dependency_pieces = {};
  /** @brief Marks a CPU, non-joint staged Result output as regional Atomic.
   * The selected query remains governed by the Result continuation and its
   * Need/publication contract. Included in operation identities.
   */
  bool regional_atomic = false;
  /** @brief Allows publication to preserve a compatible immutable input view.
   * Ordinary/direct collectors retain their existing view behavior. In
   * compiled structured CPU Whole Result execution, the coordinator prepares
   * each payload-authorized Tensor Need after validation and before the next
   * computation poll. It proves an affine view for each authorized box; Auto
   * may collect only when that view is unavailable. Collection uses
   * Root-accounted private input backing retained by the ResultTensorInput.
   * Borrowed owners remain charged independently, and the output payload cap
   * does not prevent Root allocations before publication. This property does
   * not make direct ResultProgramPhase calls fulfill Needs automatically.
   * Joint and GPU view preparation are outside this path. The C Result ABI
   * exposes corresponding output policy flags, and its compiled structured
   * CPU Whole path uses this preparation. It permits view/copy choices without
   * dense precharge.
   * With planar_exact_dependencies this is narrower: the complete Data map
   * must be an identity map from planar input port 0. The executor proves that
   * map, invokes a validate-only callback, and publishes a same-owner alias.
   * It does not preserve arbitrary callback-supplied views. Nonidentity or
   * non-port-0 maps materialize in Auto and fail in RequireView.
   */
  bool preserve_output_views = false;
  /** @brief Requires affine input views for the selected Whole view path.
   * Requires preserve_output_views. For structured Result execution, every
   * authorized Need box must map to one compatible affine owner; compatible
   * fragments from that owner may form one view. If a box cannot be represented
   * as a view, the coordinator returns InvalidArgument/InvalidDomain
   * ViewUnavailable before the computation callback. With this flag unset,
   * Auto may collect only for that unavailable-view case. Direct Result phases
   * do not automatically prepare Need-backed views. This property is part of
   * compiled identity; it does not limit typed validation.
   */
  bool requires_input_views = false;
  /** @brief Result execution protocol version. The supported value is 2;
   * other values are rejected during registration.
   */
  std::uint32_t dependency_version = 2;
  /** @brief Host-allocated state bound and finite poll limit for staged code.
   */
  std::uint64_t continuation_bytes = 0;
  std::uint32_t maximum_dependency_stages = 0;
  /** @brief Complete structured Result output prototype, resolved by compiler.
   * Scalar dtype/shape fields remain defaults and do not describe tensor
   * backing. Requires Result protocol 2. For metadata-derived C++ outputs,
   * the prototype remains complete; the output constraint may leave schema
   * id/version empty only when metadata specialization is required and a
   * tensor-member predicate is present. Inference returns the concrete schema,
   * which replaces the prototype in resolved traits. Fixed-schema outputs
   * retain their id/version.
   */
  std::optional<SchemaTemplate> result_schema = {};
  /** @brief Explicit bitwise value relation; never inferred from read needs.
   * V1 is CPU planar, with complete static pieces and spatial identity maps.
   * The registry validates shape/dtype/maps before compiler publication.
   */
  DataMovementKind data_movement = DataMovementKind::None;
  DataMovementViewPolicy data_movement_view_policy =
      DataMovementViewPolicy::Auto;
};

/**
 * @brief Immutable compiler-visible facts for one operation implementation.
 *
 * @note Traits are copied into semantic IR; callback/DSO identities are not.
 */
struct PHOTOSPIDER_API OperationTraits final {
  /** @brief CPU Result orchestration submits bounded computation stages to
   * the host tile service for Whole or protocol-2 Result execution. Coordinator
   * and tile callbacks have separate service permissions. Host range
   * parallelism remains available to Whole calls.
   */
  bool cpu_staged_tiles = false;
  /** @brief Optional CPU Result joint contract version; zero disables grouping.
   */
  std::uint32_t joint_contract = 0;
  /** @brief Shared host-owned state capacity, charged once per group. */
  std::uint64_t joint_continuation_bytes = 0;
  /** @brief Additional shared scratch bound per joint poll. */
  std::uint64_t joint_workspace_bytes = 0;
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
   * Available only to pure deterministic Atomic Result-v2 operations, with no
   * regional-atomic outputs or static dependency pieces.
   * The default preserves output-scoped block keys.
   */
  bool share_blocks_across_outputs = false;

  /** @brief Exact input count, or fixed prefix count for a repeated template.
   */
  std::uint32_t input_count = 0;
  /** @brief Equal inputs/parameters produce equal output bytes.
   * @note Dependency-v2 Result programs require both purity traits except for
   * Whole, non-joint Atomic outputs with RequestFailureOnly delivery. That
   * Whole case may be non-pure only when `cacheable` is false.
   */
  bool deterministic = true;
  /** @brief Callback has no externally visible side effect.
   * The non-pure Whole Result exception is documented on `deterministic`.
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
   * native dispatch, or field I/O, and clear cancellation, stale-plan, and
   * host-stop checks. A separate host-service retry veto rejects replay after
   * later typed resource, protocol, callback, observer, or other nonretryable
   * failures while preserving the first reported status. Other errors do not
   * fall back. Root work and stage accounting are retained. Fallback-tainted
   * results do not enter optional checkpoint, block, or completed-result
   * caches.
   */
  bool allows_cpu_fallback = false;
  /**
   * @brief Declared output-capacity bound, raised to packed bytes when
   * representable.
   * @note Non-densely-representable generic outputs use this bound with at
   * least one element. Workspace and potential transfers are accounted
   * separately. Result callbacks use their declared continuation and workspace
   * limits in addition to Root resource admission.
   */
  std::uint64_t estimated_bytes = 0;
  /** @brief Version of this complete semantic trait record. */
  std::uint32_t version = 24U;
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
  /** @brief Maximum scratch bytes per invocation, excluding output.
   * Resolved traits include the checked static preparation increment.
   * Result callbacks account actual backing capacity separately from the
   * declared workspace bound.
   */
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

/** @brief Maps sample coverage to its logical Atomic observation domain.
 * Closes ColorArray channel tuples or configured trailing tuple axes; Color
 * boxes must contain complete channels. This is geometry only and grants no
 * input-read permission.
 */
PHOTOSPIDER_API Result<Footprint> operation_observations(
    const OperationMetadata& output, const Footprint& samples,
    const FootprintLimits& limits = {});
/** @brief Expands logical observation coverage to complete sample coverage.
 * Closes ColorArray channels or configured trailing tuple axes; validates the
 * output metadata and observation domain. This mapping grants no read access.
 */
PHOTOSPIDER_API Result<Footprint> observation_samples(
    const OperationMetadata& output, const Footprint& observations,
    const FootprintLimits& limits = {});

/** @brief Owned per-node Result metadata and its typed backing descriptor.
 * Result specialization requires protocol 2 and preserves the Result port kind.
 * Fixed-schema outputs preserve the registered schema id/version. A C++
 * metadata-derived output may resolve a concrete id/version when its registered
 * constraint leaves them empty, requires metadata specialization, and provides
 * a nonempty tensor-member predicate. The predicate may name a member or omit
 * tensor_key to require a sole tensor. Inference returns the concrete schema,
 * which the registry validates before compiler publication. C ABI output
 * schemas remain fixed. Specialization may resolve fields/domain/semantic
 * metadata via the closed SchemaTemplate vocabulary. Input and output metadata
 * carry Result schemas; standalone descriptor fields stay empty. Specialization
 * cannot alter inputs, parameters, output names, callback kinds, failure
 * delivery or workspaces.
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
  DataMovementKind data_movement = DataMovementKind::None;
  DataMovementViewPolicy data_movement_view_policy =
      DataMovementViewPolicy::Auto;
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
  /** @brief Additional runtime scratch bound derived only from static metadata
   * and parameter sizes. The host checked-adds this to workspace_bytes before
   * planning or direct invocation; overflow returns ResourceExhausted.
   * This reserves no compilation payload and performs no runtime work. Actual
   * construction, computation and cancellation remain inside runtime callbacks,
   * using their allocator and work services. The resolved bound participates in
   * operation identity and is shared by the definition's selected outputs.
   * Zero preserves the registered bound. Joint workspace is independent.
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
  friend class ResultJointContinuation;
  Status validate_result_query(const ResultProgramQuery& query) const;
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
  /** @brief Alternative structured stage protocol 2; exclusive with callbacks.
   */
  ResultProgramStart start_result = {};
  /** @brief Optional contract-1 or contract-2 CPU joint callback for Results.
   * Every declared output must be Result/Atomic. Contract 1 requires
   * RequestFailureOnly and at least two outputs; contract 2 requires
   * PerAtomOutcome and may use one output. Tensor members for the same output
   * must select one tensor slot. Contract 1 keys members by distinct output
   * index; contract 2 uses distinct AtomKeys and can name different coordinates
   * of one output. The callback owns one shared continuation state and returns
   * a local Result Need, publication, or typed failure for each member. The
   * direct registry API does not fulfill Needs. Structured CPU execution
   * schedules contracts 1 and 2; singleton `start_result` remains required.
   * With optional grouping disabled, contract 2 still uses its required
   * one-member joint callback. This does not provide native GPU contract 2.
   */
  ResultJointStart start_result_joint = {};
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
   * only the exact Result operation ABI 2 entry point and full table layout;
   * missing, older, and planar-only C entries are rejected. Result modules
   * publish validated tensor/field coverage through the structured contract.
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

  /** @brief Starts a CPU contract-1 or contract-2 Result continuation.
   * Contract 1 accepts 2..64 distinct output indices and requires Atomic
   * Result outputs with RequestFailureOnly delivery. Contract 2 accepts 1..64
   * distinct AtomKeys and requires PerAtomOutcome; different coordinates may
   * name one output, but its tensor slot must stay fixed. In both contracts,
   * queries share one immutable static invocation and nonempty
   * `snapshot_identity` (at most 4096 bytes), and request CPU. Each tensor Q
   * closes over tuple and atomic trailing axes to one nonempty observation;
   * other axes have extent one. The registry retains definition, prepared
   * invocation, keys, cancellation tokens, and Root. It filters cancelled
   * members before invoking the callback, so the callback subset may contain
   * one member. Later polls revalidate raw Q and prepared metadata.
   * This direct entry starts the callback only; the host must fulfill Needs
   * and publish dependency evidence. Its later poll phases and member
   * ResourceBudgets must belong to the continuation Root, and their allocators
   * must share its accounting domain. Structured CPU execution supplies Need
   * fulfillment and dependency-evidence services for contracts 1 and 2. With
   * optional joint grouping disabled, contract 2 still uses its required
   * one-member joint callback. This does not provide native GPU contract-2
   * execution.
   */
  Result<ResultJointContinuation> start_result_joint(
      const std::string& key, const ResourceVector<ResultProgramQuery>& queries,
      const ResourceBudget& resources) const;

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
   * direct start API. For a C++ terminal RequestRecord or contract-2 joint
   * output with an Empty tensor query and no fields, the registry returns a
   * stateless host continuation after static checks and resource admission.
   * Its later poll checks failure and cancellation before sealing an Empty
   * Result; producer callbacks and input reads are skipped. Ordinary Atomic
   * Empty outputs and outputs with fields still run their producer. This fast
   * path does not create an AtomObservation or validation-ledger finality.
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
  Result<ResultJointContinuation> start_result_joint_compiled(
      const std::string& key, const ResourceVector<ResultProgramQuery>& queries,
      const ResourceBudget& resources, const BufferAllocator& allocator) const;
  Result<ResultJointContinuation> start_result_joint_impl(
      const std::string& key, const ResourceVector<ResultProgramQuery>& queries,
      const ResourceBudget& resources, const BufferAllocator& allocator) const;
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
