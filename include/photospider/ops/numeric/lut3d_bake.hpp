#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/data/result.hpp"
#include "photospider/ops/export.hpp"
#include "photospider/ops/numeric/color_description.hpp"

namespace ps {
/** @brief Explicit interpolation whose measured error is reported. */
enum class Lut3dInterpolation : std::uint32_t {
  Trilinear = 1,
  Tetrahedral = 2
};
/** @brief Immutable semantics of a measured LUT bake, never a whole-domain
 * bound. shape extents are 2..256; extras is 0..1048576. Tolerances are finite
 * and nonnegative as unchecked caller preconditions. Colors use the same
 * supported three-component model. Dtypes are Float32/64 and recipe_identity
 * identifies the expanded source graph and its compiler metadata. The owned
 * table schema is version 4 and contains one `colors` tensor [N0,N1,N2,3] with
 * the selected dtype.
 */
struct Lut3dBakeDescription {
  std::array<std::uint64_t, 3> shape{2, 2, 2};
  Lut3dInterpolation interpolation = Lut3dInterpolation::Trilinear;
  double atol = 0, rtol = 0;
  ElementType table_dtype = ElementType::Float64;
  ElementType source_dtype = ElementType::Float64;
  std::uint64_t extra_points = 0;
  TensorDescription input_description, output_description;
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
  /** @brief Configuration identity decoded without comparing its contents. */
  std::array<char, 64> configuration_id{};
  /** @brief ObjectId of the complete table acquired during measurement.
   * Encoded as eight little-endian bytes in the report. Valid only for this
   * process object; equal contents do not authorize a different table object.
   * Equal immutable inputs may produce a different identity on each Run.
   */
  std::uint64_t measured_table_id = 0;
  std::array<double, 9> axis{};
  std::int64_t validation_count = 0, failed_count = 0;
  std::array<double, 3> max_abs_error{};
  std::array<double, 9> max_error_point{};
  std::array<std::int64_t, 3> max_error_index{};
  std::int64_t first_failure_index = -1;
  std::array<double, 3> first_failure_input{}, first_failure_reference{},
      first_failure_lut{};
};
/** @brief Canonical identity of explicit bake settings, stored in report data.
 * Includes the source recipe, both descriptions, dtypes, sampling and
 * tolerance. Returns a 64-byte lowercase SHA-256 string. Static structure
 * and supported choices are checked; numerical premises are trusted.
 */
PHOTOSPIDER_OPS_SDK_API Result<std::string> lut3d_bake_configuration_id(
    const Lut3dBakeDescription& description);
/** @brief Creates the SDK LUT3D bake report v4 static Tensor schema.
 * It has 13 fixed tensor members and 361 data bytes.
 * Pure/thread-safe, no runtime input; allocation may throw bad_alloc. Invalid
 * configuration fails InvalidArgument/InvalidDomain. Acceptance remains
 * measured only at the recorded centers/extras for this method/table dtype.
 */
PHOTOSPIDER_OPS_SDK_API Result<SchemaTemplate> lut3d_bake_schema(
    const Lut3dBakeDescription& description);
/** @brief Owned sampled-table Result schema associated with its measured
 * report. Version 4 has one `colors` tensor [N0,N1,N2,3], the converted dtype,
 * one atomic trailing component axis. No quality pass
 * is implied by this intermediate. Use the accepted table for
 * application.
 */
PHOTOSPIDER_OPS_SDK_API Result<SchemaTemplate> lut3d_bake_table_schema(
    const Lut3dBakeDescription& description);
/** @brief Validates structure against explicitly supplied bake configuration.
 */
PHOTOSPIDER_OPS_SDK_API Status validate_lut3d_bake_schema(
    const SchemaTemplate& schema, const Lut3dBakeDescription& description);
/** @brief Prepared fixed member regions for a LUT3D bake report.
 * Construct during static preparation and retain unchanged while reading
 * Results. A reader trusts the supplied layout and the matching complete Result
 * schema.
 */
struct Lut3dBakeReportLayout {
  std::array<Region, 13> regions;
};
/** @brief Checks report v4 structure and prepares its fixed read regions.
 * Pure/thread-safe; incompatible schema returns TypeMismatch. No payload is
 * read. The caller performs this once during compilation/static preparation.
 */
PHOTOSPIDER_OPS_SDK_API Result<Lut3dBakeReportLayout>
prepare_lut3d_bake_report_layout(const SchemaTemplate& schema);
/** @brief Decodes a completed report using its statically prepared layout.
 * The caller supplies a valid sealed Result covering all thirteen members with
 * the prepared schema. Payload contents, configuration identity and table
 * pairing are unchecked premises. The helper neither scans values nor compares
 * identities and does not start a producer. configuration_id and
 * measured_table_id are data. Result and windows retain backing after context
 * teardown. maximum_window must admit each complete member (at most 72 bytes),
 * or ResourceExhausted is returned. Optional consume_work accounts the 361
 * copied payload bytes. Cancellation, capacity and read-window failures
 * propagate. Thread-safe for an immutable Result/layout; returned arrays own
 * their values without a retained context.
 */
PHOTOSPIDER_OPS_SDK_API Result<Lut3dBakeReport> read_lut3d_bake_report(
    const ResultRef& result, const Lut3dBakeReportLayout& layout,
    std::uint64_t maximum_window = 4096,
    const CancellationToken& cancellation = {},
    const std::function<Status(std::uint64_t)>& consume_work = {});
/** @brief Decode complete explicit bake parameters; missing/wrong values throw
 * std::out_of_range or std::bad_variant_access for the host callback fence.
 */
PHOTOSPIDER_OPS_SDK_API Result<Lut3dBakeDescription> lut3d_bake_from_parameters(
    const std::map<std::string, ParameterValue>& parameters);
}  // namespace ps
