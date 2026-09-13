#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>

#include "photospider/data/result.hpp"

namespace ps {
/** @brief Closed representation families understood by the runtime publisher.
 */
enum class RepresentationKind : std::uint32_t {
  Spectrum = 1,
  Bands = 2,
  PathSet = 3,
  Points = 4,
  Components = 5,
  YCbCr420 = 6,
  Brush = 7,
  Iterative = 8
};
enum class SpectrumPacking : std::uint32_t { Full = 1, R2CHalf = 2 };
enum class SpectrumNormalization : std::uint32_t {
  Unscaled = 1,
  InverseBySize = 2,
  Unitary = 3
};
enum class SpectrumRealPolicy : std::uint32_t {
  Complex = 1,
  ExactHermitian = 2,
  RealProjectionMeasured = 3
};
/** @brief Complete transform identity, independent of page geometry.
 * axis_order lists slow-to-fast stored axes. shifts are logical cyclic shifts;
 * packed spectra require zero shifts. Hermitian tolerance is complex absolute
 * magnitude with a relative max-pair magnitude term over the transform domain.
 * RealProjectionMeasured uses a named floating acceptance rule for finite data;
 * it never certifies a floating-point error bound.
 */
struct SpectrumSpec final {
  ResourceVector<std::uint64_t> original_shape;
  ResourceVector<std::uint32_t> transformed_axes, axis_order;
  ResourceVector<std::uint64_t> shifts;
  ResourceVector<double> sample_origin, sample_step;
  SpectrumPacking packing = SpectrumPacking::R2CHalf;
  std::uint32_t packed_axis = 1;
  std::int32_t sign = -1;
  SpectrumNormalization normalization = SpectrumNormalization::Unscaled;
  SpectrumRealPolicy real_policy = SpectrumRealPolicy::RealProjectionMeasured;
  double atol = 1e-10, rtol = 0;
  ResourceString unit = "sample";
};
/** @brief One explicit keyed band; mask bits follow the declared axes order. */
struct BandSpec final {
  std::uint32_t level = 0, mask = 0;
  ResourceVector<std::uint64_t> shape, parent_shape;
  ResourceVector<double> logical_origin, sample_step, phase;
  bool explicit_zero = false;
};
/** @brief Mallat membership and full reconstruction identity.
 * Version 1 implements the named duplicate-last Haar analysis/synthesis rule:
 * low=(a+b)/2, high=(a-b)/2, reconstruction low+-high then parent-shape crop.
 * Required membership is final approximation plus each level's nonzero masks.
 * ExplicitZero is a present member with no stored coefficients; missing members
 * are rejected. It makes no general filter-bank reconstruction claim.
 */
struct BandsSpec final {
  ResourceVector<std::uint64_t> original_shape;
  ResourceVector<std::uint32_t> axes;
  std::uint32_t levels = 0;
  ResourceVector<BandSpec> bands;
  ResourceString filter_snapshot = "haar-average-difference-v1";
};
enum class PathAuthority : std::uint32_t { CoreVerbs = 1, Primitives = 2 };
enum class PathVerb : std::uint8_t {
  Move = 1,
  Line = 2,
  Quadratic = 3,
  Cubic = 4,
  Close = 5
};
enum class PrimitiveTag : std::int64_t {
  Line = 1,
  Quadratic = 2,
  Cubic = 3,
  ArcSweep = 4,
  ArcFullTurn = 5,
  Hermite = 6,
  BSpline = 7
};
/** @brief Canonical primitive row. association stores the unsigned ObjectId
 * bit pattern in the fourth Int64-width slot; geometry fields have one owner.
 */
struct PrimitiveRef final {
  PrimitiveTag tag = PrimitiveTag::Line;
  std::int64_t record_index = 0, payload_member = 6;
  std::uint64_t association = 0;
};
enum class PathAttributeDomain : std::int64_t {
  Path = 1,
  Subpath = 2,
  Segment = 3,
  Control = 4,
  ArcLength = 5
};
enum class PathInterpolation : std::int64_t { Constant = 1, Linear = 2 };
/** @brief Exactly one geometry authority, finite 2D coordinates, nonperiodic
 * splines and explicit full-turn arcs. Positive rational weights only.
 * Primitive records contain tag, record_index, payload field index and this
 * result's ObjectId. Arc endpoint equality is never inferred from sin(2*pi).
 * The schema preserves attributes and normalized arc-length parameter samples;
 * it does not promise topology or antialiasing error from geometric tolerance.
 */
struct PathSetSpec final {
  PathAuthority authority = PathAuthority::CoreVerbs;
  ResourceString coordinate_system = "pixel-xy-right-down";
  std::uint64_t maximum_segments = 1048576, maximum_controls = 4194304;
  std::uint32_t maximum_spline_degree = 16;
};
enum class StableIdScheme : std::uint32_t { InputPosition = 1, Ordinal = 2 };
/** @brief IDs and attributes share one ResultRef association. id_to_row is
 * sorted by ID and explicitly describes any physical reordering of records.
 */
struct PointSetSpec final {
  std::uint32_t dimensions = 2, attribute_width = 1;
  StableIdScheme ids = StableIdScheme::InputPosition;
  std::uint64_t basis_count = 1, maximum_count = 1048576;
  ResourceString coordinate_system = "pixel-xy-right-down";
};
enum class ComponentIdScheme : std::uint32_t {
  MinPixel = 1,
  CompactMinOrder = 2
};
/** @brief Associated label/table representation. Table rows are id,area,min
 * position. Structural validation checks the exact counts and ID basis; it
 * does not by itself prove four-connectivity of an arbitrary imported labelset.
 */
struct ComponentsSpec final {
  std::uint64_t height = 1, width = 1, maximum_count = 1048576;
  ComponentIdScheme ids = ComponentIdScheme::MinPixel;
};
/** @brief Existing named BT.709 full-range YCbCr 4:2:0 box-clipped mode.
 * Y/Cb/Cr have one association. Chroma nominal origin is (.5,.5), step (2,2);
 * support clips to actual source pixels at odd edges. The nominal origin is
 * never used as a substitute for an input dependency footprint.
 */
struct YCbCr420Spec final {
  std::uint64_t height = 1, width = 1;
  ResourceString primaries = "srgb-d65", transfer = "bt709",
                 reference = "display";
};
/** @brief Versioned one-dimensional constant-spacing state with explicit
 * pending/lookahead events and finalized prefixes. Carry stores Layer P,A,E.
 * This representation allows noncausal state but only the named causal helper
 * below computes dabs. It makes no smoothing/smudge equivalence claim.
 */
struct BrushSpec final {
  double spacing = 1;
  std::uint64_t maximum_pending = 64;
  std::uint32_t lookahead = 0;
  ResourceString algorithm = "constant-spacing-1d-v1";
};
struct BrushEvent final {
  std::uint64_t id = 0;
  double position = 0;
};
struct CausalBrushState final {
  std::uint64_t next_event = 0, dab_count = 0;
  double last_position = 0, remaining = 0;
  bool started = false, ended = false;
};
struct BrushAdvance final {
  CausalBrushState state;
  ResourceVector<double> dabs;
};
/** @brief Sequential fold with a carried distance-to-next-dab; events are
 * strictly ordered and positions nondecreasing. End is irreversible. Admission
 * and work failure return no partially advanced state. The input stays intact.
 */
PHOTOSPIDER_API Result<BrushAdvance> advance_causal_brush(
    const BrushSpec& spec, const CausalBrushState& previous,
    const BrushEvent* events, std::uint64_t count, bool end,
    const ResourceBudget& resources,
    const CancellationToken& cancellation = {});
/** @brief Frozen estimate/system generation and explicitly measured residual.
 * The first implementation names a finite diagonal system and infinity norm.
 * A residual alone never supplies a CertifiedBound. require_converged changes
 * the success domain; approximate consumers must select it explicitly.
 */
enum class IterationStopReason : std::int64_t {
  Converged = 0,
  IterationLimit = 1
};
struct IterativeSpec final {
  std::uint64_t size = 1, maximum_iterations = 1000;
  double target_residual = 1e-8;
  bool require_converged = true;
  ResourceString system_snapshot;
  ResourceString initialization = "zero";
};

/** @brief Validated immutable schemas; invalid enums/dimensions/counts or
 * overflowing products fail before any result/source I/O. Returned metadata is
 * caller-owned, or inherits the active resource allocation scope.
 */
PHOTOSPIDER_API Result<SchemaTemplate> spectrum_schema(
    const SpectrumSpec& spec);
PHOTOSPIDER_API Result<SchemaTemplate> bands_schema(const BandsSpec& spec);
PHOTOSPIDER_API Result<SchemaTemplate> path_set_schema(const PathSetSpec& spec);
PHOTOSPIDER_API Result<SchemaTemplate> point_set_schema(
    const PointSetSpec& spec);
PHOTOSPIDER_API Result<SchemaTemplate> components_schema(
    const ComponentsSpec& spec);
PHOTOSPIDER_API Result<SchemaTemplate> ycbcr420_schema(
    const YCbCr420Spec& spec);
PHOTOSPIDER_API Result<SchemaTemplate> brush_schema(const BrushSpec& spec);
PHOTOSPIDER_API Result<SchemaTemplate> iterative_schema(
    const IterativeSpec& spec);
/** @brief Decodes the complete immutable Spectrum contract; unrelated schemas
 * fail TypeMismatch. No data I/O is performed.
 */
PHOTOSPIDER_API Result<SpectrumSpec> spectrum_spec(
    const SchemaTemplate& schema);
/** @brief Typed static decoding; unrelated schemas fail TypeMismatch. */
PHOTOSPIDER_API Result<BandsSpec> bands_spec(const SchemaTemplate& schema);
PHOTOSPIDER_API Result<PathSetSpec> path_set_spec(const SchemaTemplate& schema);
PHOTOSPIDER_API Result<PointSetSpec> point_set_spec(
    const SchemaTemplate& schema);
PHOTOSPIDER_API Result<ComponentsSpec> components_spec(
    const SchemaTemplate& schema);
PHOTOSPIDER_API Result<YCbCr420Spec> ycbcr420_spec(
    const SchemaTemplate& schema);
PHOTOSPIDER_API Result<BrushSpec> brush_spec(const SchemaTemplate& schema);
PHOTOSPIDER_API Result<IterativeSpec> iterative_spec(
    const SchemaTemplate& schema);
struct BandRange final {
  std::uint64_t first = 0, rows = 0;
  bool explicit_zero = false;
};
/** @brief Finds a complete keyed member's logical coefficient interval.
 * ExplicitZero returns its logical count and no stored interval; callers must
 * synthesize the explicitly declared zero member instead of pinning it.
 * Missing keys return NotFound. No data I/O or producer execution occurs.
 */
PHOTOSPIDER_API Result<BandRange> band_range(const ResultRef& result,
                                             std::uint32_t level,
                                             std::uint32_t mask);
/** @brief Runtime recognizes only these reserved family ids. */
PHOTOSPIDER_API bool has_representation_schema(std::string_view id) noexcept;
/** @brief Closed static schema validation, called by SchemaTemplate::validate.
 * Unrelated user schemas return success. Does not recursively call validate.
 */
PHOTOSPIDER_API Status
validate_representation_schema(const SchemaTemplate& schema);
/** @brief Paged explicit validation performed by the coordinator before a
 * recognized complete publication becomes observable. Uses the supplied root
 * for windows/work/I/O and requires matching root provenance. Unknown schemas
 * are left to their registered operation. Recognized families require complete
 * bundles; no future validation may revoke an observed prefix.
 * consume_work is an optional additional admission hook; root work is always
 * charged independently, including when that hook fails.
 * This call performs I/O and must not be invoked inside a result callback.
 */
PHOTOSPIDER_API Status validate_representation(
    const ResultRef& result, const ResourceBudget& resources,
    std::uint64_t maximum_window, const CancellationToken& cancellation = {},
    const std::function<Status(std::uint64_t)>& consume_work = {});
}  // namespace ps
