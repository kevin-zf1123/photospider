#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "photospider/data/color_array.hpp"
#include "photospider/data/result.hpp"

namespace ps {
/** @brief Explicit interpolation whose measured error is reported. */
enum class Lut3dInterpolation : std::uint32_t {
  Trilinear = 1,
  Tetrahedral = 2
};
/** @brief Immutable semantics of a measured LUT bake, never a whole-domain
 * bound. shape extents are 2..256; extras is 0..1048576. Tolerances are finite
 * and nonnegative. Colors use the same supported three-component model. Dtypes
 * are Float32/64 and recipe_identity identifies the expanded source graph and
 * its compiler metadata. The owned table schema is version 2 and contains one
 * `colors` tensor [N0,N1,N2,3] with the selected dtype and ColorArray v1 facet.
 */
struct Lut3dBakeDescription {
  std::array<std::uint64_t, 3> shape{2, 2, 2};
  Lut3dInterpolation interpolation = Lut3dInterpolation::Trilinear;
  double atol = 0, rtol = 0;
  ElementType table_dtype = ElementType::Float64;
  ElementType source_dtype = ElementType::Float64;
  std::uint64_t extra_points = 0;
  ColorArrayDescriptor input_description, output_description;
  std::string recipe_identity;
};
/** @brief Fixed-size dynamic measurement; row-major centers precede extras.
 * Maxima are exact-error selections, rounded upward for presentation; +Inf
 * is allowed only in max_abs_error. Ties retain their earliest ordinal.
 * First-failure arrays are +0 and index=-1 when passed. Repeated points count
 * separately. A successful failed report is distinct from an execution error.
 */
struct Lut3dBakeReport {
  bool passed = false;
  std::array<double, 9> axis{};
  std::int64_t validation_count = 0, failed_count = 0;
  std::array<double, 3> max_abs_error{};
  std::array<double, 9> max_error_point{};
  std::array<std::int64_t, 3> max_error_index{};
  std::int64_t first_failure_index = -1;
  std::array<double, 3> first_failure_input{}, first_failure_reference{},
      first_failure_lut{};
};
/** @brief Creates the registered LUT3D bake report v1 CompleteBundle
 * schema with 11 fixed fields, domain [1], quality=Measured and 289 data bytes.
 * Pure/thread-safe, no runtime input; allocation may throw bad_alloc. Invalid
 * metadata fails InvalidArgument/InvalidDomain. Acceptance remains measured
 * only at the recorded centers/extras and applies to this method/table dtype.
 */
PHOTOSPIDER_API Result<SchemaTemplate> lut3d_bake_schema(
    const Lut3dBakeDescription& description);
/** @brief Owned sampled-table Result schema associated with its measured
 * report. Version 2 has one `colors` tensor [N0,N1,N2,3], the converted dtype,
 * ColorArray v1 facet and one atomic trailing component axis. No quality pass
 * is implied by this intermediate. Use the accepted table for
 * application.
 */
PHOTOSPIDER_API Result<SchemaTemplate> lut3d_bake_table_schema(
    const Lut3dBakeDescription& description);
/** @brief Decodes and validates a canonical report or owned-table schema. */
PHOTOSPIDER_API Result<Lut3dBakeDescription> lut3d_bake_description(
    const SchemaTemplate& schema);
/** @brief Reads a sealed report through explicit owned Result read windows.
 * Obtain the report Result from `ExecutionContext::execute()`; its fixed fields
 * are not requested with generic tensor footprints by `execute_fragments()`.
 * Does not start a producer. The report association records observed input
 * ObjectIds in port order; its first entries identify axis, grid and owned
 * table Results, with owners observed from later source batches following.
 * Entry 2 identifies the owned table. Result and windows own backing after
 * context teardown. maximum_window must admit a complete field (at most 72
 * bytes); a smaller field window fails with ResourceExhausted/CapacityLimit.
 * The helper applies the same pure field validation as the gate. It always
 * reads the report's 11 fixed fields, including for a Result with no tensor
 * coverage. Empty coverage skips payload validation for the owned-table tensor,
 * not report field reads. Optional consume_work accounts bounded
 * schema/axis/field validation.
 * Cancellation/I/O/capacity errors propagate; malformed fields fail
 * OperationFailed/InvalidDomain. Thread-safe for an immutable sealed Result.
 * Caller-owned returned arrays require no retained context or callback.
 */
PHOTOSPIDER_API Result<Lut3dBakeReport> read_lut3d_bake_report(
    const ResultRef& result, std::uint64_t maximum_window = 4096,
    const CancellationToken& cancellation = {},
    const std::function<Status(std::uint64_t)>& consume_work = {});
}  // namespace ps
