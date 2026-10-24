#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/ops/export.hpp"

namespace ps::filter {
/** @brief Explicit FIL-13A options. All fields must be supplied.
 * y_axis/x_axis are distinct logical axes (including batch prefixes).
 * Gaussian sigma/radii use pixels; low/high use the selected magnitude units.
 * The caller supplies 0 < low < high, finite nonnegative sigma, compatible
 * radii, norm l1/l2 and connectivity 4/8. Inputs are raw Float32/64 fields.
 */
struct CannyOptions final {
  std::int64_t y_axis, x_axis;
  double sigma_y, sigma_x;
  std::int64_t radius_y, radius_x;
  std::string norm;
  double low, high;
  std::int64_t connectivity;
};
/** @brief Append Gaussian, Sobel, magnitude, quantized NMS and hysteresis.
 * The document is changed transactionally; serialize edits to one document.
 * Returns a UInt8 edge output. Input schemas are checked at compilation;
 * invalid option syntax/IDs returns InvalidArgument. Numeric domains are
 * unchecked. Allocation may throw bad_alloc. Execution propagates resource,
 * cancellation and upstream failures. Intermediate RN stages are strict.
 */
PHOTOSPIDER_OPS_API Result<WorkflowNodeOutput> canny_quantized4(
    WorkflowDocument& document, WorkflowInput input,
    const CannyOptions& options);

/** @brief Static FIL-14A configuration and stable bank identity.
 * Frequency is cycles/pixel in [0,.5]; theta/phase are radians; sigma/aspect
 * are positive. dc is keep/remove and normalization is none/l1_envelope.
 * Every value is explicit. Duplicate configurations with distinct IDs are
 * permitted; IDs are nonempty and unique within one ordered bank.
 */
struct GaborConfiguration final {
  std::string bank_id;
  double frequency, theta, phase, sigma, aspect;
  std::int64_t radius_y, radius_x;
  std::string dc, normalization;
};
/** @brief One logical FilterBands/v1 bank entry with two independent Results.
 * The caller owns source-grid/color interpretation. Both roles retain the
 * source dtype, shape, origin and sampling; profile is gabor_v1 and stage
 * rounding is strict. Export any subset: unselected bank nodes have no tasks.
 */
struct GaborBand final {
  std::string bank_id;
  WorkflowNodeOutput real, imaginary;
};
/** @brief Append the ordered FIL-14C kernel/response pairs transactionally.
 * Axes address the full logical sample shape. Each kernel has a centered
 * anchor, reflect_half boundary, no correlation normalization or bias.
 * Static directory/option errors return InvalidArgument; compiled descriptors
 * establish matching shapes/dtypes. Numerical premises are unchecked.
 * Independent documents are thread-safe; editing one document is serialized.
 * Returned graph references borrow no payload. Execution Results retain their
 * own tile owners and may outlive the execution context.
 */
PHOTOSPIDER_OPS_API Result<std::vector<GaborBand>> gabor_bank(
    WorkflowDocument& document, WorkflowInput input, std::int64_t y_axis,
    std::int64_t x_axis, const std::vector<GaborConfiguration>& configurations);

/** @brief Append a selected FilterBands/v1 member as an ordinary tensor.
 * band is gaussian_N, detail_N or base. Compilation checks the schema version
 * and requested member; the producer validates its collection directory.
 * Only the selected member and its required ancestors execute.
 * The output uses explicit COPY and owns independent tiles.
 */
PHOTOSPIDER_OPS_API Result<WorkflowNodeOutput> select_band(
    WorkflowDocument& document, WorkflowInput bands, const std::string& band);

/** @brief Explicit SDK directory configuration for FIL-15.
 * original_shape addresses all logical sample axes, including batch prefixes.
 * source_grid and unit identify the caller's grid. Origin is unchanged by the
 * even sampling phase; spacing is multiplied by two at each level. profile is
 * strict, accelerated_apple_silicon or accelerated_x86_64. No image is read
 * while authoring; compilation checks declared shape/dtype against descriptors.
 */
struct PyramidOptions final {
  std::vector<std::uint64_t> original_shape;
  ElementType dtype;
  std::int64_t y_axis, x_axis, levels;
  std::string source_grid, unit;
  std::array<double, 2> origin, spacing;
  std::string profile;
};
/** @brief One statically enumerated FilterBands/v1 entry.
 * valid_crop contains (offset, extent) pairs in logical axis order. The value
 * can refer to an independently authored or edited coefficient Result.
 * Metadata is caller-owned and is checked before reconstruction is appended.
 */
struct PyramidBand final {
  std::string band_id, source_grid, role;
  std::uint64_t level;
  ElementType dtype;
  std::vector<std::uint64_t> original_shape, shape;
  std::vector<std::array<std::uint64_t, 2>> valid_crop;
  std::array<std::uint64_t, 2> sampling_step;
  std::array<double, 2> origin, spacing;
  std::string analysis_profile, synthesis_profile, rounding_profile;
  std::string padding_history;
  WorkflowInput value;
};
/** @brief Logical collection spanning as many fixed ABI Results as needed. */
struct PyramidBands final {
  std::string schema, kind;
  PyramidOptions options;
  std::vector<PyramidBand> bands;
};
/** @brief Append the complete FIL-15A directory transactionally.
 * Fixed Results contain at most 16 members; deeper pyramids compose blocks of
 * at most 15 reductions. The legal level limit remains the first 1x1 level.
 * Export any subset of returned references; only required ancestors execute.
 * Materialized G0 and selected bands use explicit COPY with fresh tile owners.
 * Invalid static configuration returns InvalidArgument/TypeMismatch; allocation
 * may throw bad_alloc. Runtime resource/cancellation/upstream errors propagate.
 */
PHOTOSPIDER_OPS_API Result<PyramidBands> gaussian_pyramid(
    WorkflowDocument& document, WorkflowInput input,
    const PyramidOptions& options);
/** @brief Append FIL-15B with detail levels 0..L-1 and a level-L base.
 * Ownership, selection, static checks and failures match gaussian_pyramid.
 * All reduce, expand and residual RN stages are preserved across blocks.
 */
PHOTOSPIDER_OPS_API Result<PyramidBands> laplacian_pyramid(
    WorkflowDocument& document, WorkflowInput input,
    const PyramidOptions& options);
/** @brief Reconstruct a checked logical FIL-15C directory transactionally.
 * Entries may be reordered or independently edited. IDs must be unique and
 * each expected level/role/shape/phase/profile must occur exactly once.
 * Compiled COPY assemblies retain independent tiles; runtime numerical and
 * association premises remain unchecked. Producer identity is not required.
 */
PHOTOSPIDER_OPS_API Result<WorkflowNodeOutput> reconstruct_laplacian(
    WorkflowDocument& document, const PyramidBands& bands);

/** @brief Caller-supplied grid for a logical filter collection.
 * These fields describe all logical sample axes and the source's world grid.
 * Shapes/dtypes are verified against the producer when the graph is compiled.
 */
struct FilterGrid final {
  std::vector<std::uint64_t> original_shape;
  ElementType dtype;
  std::int64_t y_axis, x_axis;
  std::string source_grid, unit;
  std::array<double, 2> origin, spacing;
};
/** @brief One Gabor bank ID and its two explicitly named response roles. */
struct GaborBankEntry final {
  GaborConfiguration configuration;
  GaborBand response;
  FilterGrid grid;
  std::vector<std::array<std::uint64_t, 2>> valid_crop;
  std::array<std::uint64_t, 2> sampling_step;
  std::string real_role, imaginary_role;
  std::string analysis_profile, rounding_profile, padding_history;
};
/** @brief Complete static FilterBands/v1 Gabor directory. */
struct GaborBank final {
  std::string schema, backend_profile;
  FilterGrid grid;
  std::vector<GaborBankEntry> bands;
};
/** @brief Append FIL-14C with an explicit checked logical directory.
 * profile is strict, accelerated_apple_silicon or accelerated_x86_64.
 * Every entry retains its ordered unique ID, full configuration, original
 * grid, valid crop and unit sampling phase. Roles own independent Results.
 * Static errors leave document unchanged; allocation may throw bad_alloc.
 * Runtime resource/cancellation/upstream failures propagate unchanged.
 */
PHOTOSPIDER_OPS_API Result<GaborBank> gabor_bank(
    WorkflowDocument& document, WorkflowInput input, const FilterGrid& grid,
    const std::vector<GaborConfiguration>& configurations,
    const std::string& profile);
}  // namespace ps::filter
