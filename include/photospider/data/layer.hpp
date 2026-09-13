#pragma once

#include <array>
#include <functional>
#include <string_view>

#include "photospider/data/result.hpp"

namespace ps {
/** @brief Signed/HDR finite associated color; A in [0,1], A=0 implies P=0. */
struct CoveragePixel final {
  std::array<float, 3> p{};
  float a = 0;
};
/** @brief One indivisible seven-component observation in one linear space. */
struct LayerPixel final {
  CoveragePixel coverage;
  std::array<float, 3> emission{};
};
/** @brief Q + T*background. This observation loses coverage/emission identity.
 */
struct LayerResponsePixel final {
  std::array<float, 3> q{};
  float t = 1;
};
/** @brief Raw additive color and nonnegative mass; mass is not transmittance.
 */
struct RawRgbaSumPixel final {
  std::array<float, 3> p{};
  float mass = 0;
};
/** @brief Ordered binary64 Np[3],Na,Ne[3],W. W>=0, 0<=Na<=W;
 * Na=0 implies Np=0; W=0 implies every numerator is zero.
 */
struct WeightedLayerSum final {
  std::array<double, 8> components{};
};
/** @brief Unweighted P[3],A,E[3] exactly lifted from binary32, followed by a
 * finite nonnegative binary64 weight. Internal reduction scratch is not a
 * published WeightedLayerSum and may temporarily have Na=0 with Np!=0.
 */
struct LayerContribution final {
  std::array<double, 8> components{};
};
PHOTOSPIDER_API Status
validate_layer_contribution(const LayerContribution& value);
/** @brief W=0 is an explicit empty observation, never an invented black pixel.
 */
struct OptionalLayerPixel final {
  bool valid = false;
  LayerPixel value;
};

/** @brief Pure validation; invalid imported associations return TypeMismatch.
 */
PHOTOSPIDER_API Status validate_coverage(const CoveragePixel& value);
PHOTOSPIDER_API Status validate_layer(const LayerPixel& value);
PHOTOSPIDER_API Status validate_raw_rgba_sum(const RawRgbaSumPixel& value);
PHOTOSPIDER_API Status
validate_weighted_layer_sum(const WeightedLayerSum& value);
/** @brief Strict binary32 primitives, each rounded separately under nearest
 * ties-to-even with gradual underflow; caller floating environment is restored.
 * No FMA, epsilon, clamping or implicit emission reassignment. Finite overflow
 * and association underflow return OperationFailed with a stable reason. Inputs
 * are immutable, and failure publishes no partial result. Thread-safe.
 */
PHOTOSPIDER_API Result<LayerPixel> layer_over(const LayerPixel& front,
                                              const LayerPixel& back);
/** @brief Scales P,A by finite opacity in [0,1]; preserves E exactly. */
PHOTOSPIDER_API Result<LayerPixel> layer_opacity(const LayerPixel& input,
                                                 float opacity);
/** @brief Adds gain*emission; behind additionally multiplies by rounded 1-A.
 * Gain and emission must be finite; signed emission and gain are allowed.
 */
PHOTOSPIDER_API Result<LayerPixel> layer_emit(
    const LayerPixel& input, const std::array<float, 3>& emission, float gain,
    bool behind);
/** @brief Flattens with explicit opaque background using (P+E)+(1-A)*B. */
PHOTOSPIDER_API Result<CoveragePixel> layer_flatten(
    const LayerPixel& input, const std::array<float, 3>& background);
PHOTOSPIDER_API Result<LayerResponsePixel> layer_response(
    const LayerPixel& input);
PHOTOSPIDER_API Result<LayerResponsePixel> response_over(
    const LayerResponsePixel& front, const LayerResponsePixel& back);
PHOTOSPIDER_API Result<RawRgbaSumPixel> raw_rgba_plus(
    const CoveragePixel& first, const CoveragePixel& second);
/** @brief Explicit conversion: either require M<=1 or cap alpha while retaining
 * P. Neither path divides by mass or supplies an averaging weight.
 */
PHOTOSPIDER_API Result<CoveragePixel> raw_rgba_coverage(
    const RawRgbaSumPixel& input, bool cap_alpha_keep_color);
/** @brief Strict binary64 leaf and ordered internal-node arithmetic. Weight is
 * finite and nonnegative. These primitives alone do not select a reduction
 * tree.
 */
PHOTOSPIDER_API Result<WeightedLayerSum> weighted_layer_leaf(
    const LayerPixel& input, double weight);
PHOTOSPIDER_API Result<WeightedLayerSum> weighted_layer_add(
    const WeightedLayerSum& left, const WeightedLayerSum& right);
/** @brief Divide each numerator by W in binary64 then round to binary32 and
 * revalidate the complete Layer. W=0 returns valid=false without division.
 */
PHOTOSPIDER_API Result<OptionalLayerPixel> weighted_layer_finalize(
    const WeightedLayerSum& input);

enum class LayerRepresentation : std::uint32_t {
  Layer = 1,
  Response = 2,
  RawSum = 3,
  Contributions = 4,
  WeightedSum = 5,
  OptionalLayer = 6
};
/** @brief Version-one space is linear sRGB, D65, relative scene-referred units.
 * Layer/Response/RawSum have H*W raster observations. Contributions is a
 * dynamic ordered collection (including zero rows); WeightedSum and
 * OptionalLayer have one logical output location. A complete Layer stores
 * associated coverage[4] and emission[3] fields; no field can publish
 * independently.
 */
struct LayerSpec final {
  std::uint64_t height = 1, width = 1;
  std::uint32_t working_space = 1;
};
PHOTOSPIDER_API Result<SchemaTemplate> layer_schema(
    LayerRepresentation representation, const LayerSpec& spec = {});
PHOTOSPIDER_API Result<LayerSpec> layer_spec(const SchemaTemplate& schema);
PHOTOSPIDER_API bool has_layer_schema(std::string_view id) noexcept;
/** @brief Static closed-schema check, no I/O; unrelated ids are untouched. */
PHOTOSPIDER_API Status validate_layer_schema(const SchemaTemplate& schema);
/** @brief Explicit bounded paged validation before runtime publication. All
 * seven Layer components participate even if a later consumer needs only P.
 * Root provenance, work and I/O limits apply; hook adds an independent limit.
 */
PHOTOSPIDER_API Status validate_layer_result(
    const ResultRef& result, const ResourceBudget& resources,
    std::uint64_t maximum_window, const CancellationToken& cancellation = {},
    const std::function<Status(std::uint64_t)>& consume_work = {});
}  // namespace ps
