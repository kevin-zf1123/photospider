#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/ops/export.hpp"

namespace ps::frequency {
/** @brief Caller-owned FrequencyGrid/v1 interpretation; every field is
 * explicit. original_shape includes all batch/sample axes, rank 2..8, at most
 * 2^40 samples. y_axis/x_axis are distinct. packing is full/r2c_x; index_order
 * is unshifted/shifted; norm is backward/ortho. spacing is positive finite in
 * unit; origin is finite. Numeric premises are unchecked. These values are not
 * attached to Results and do not certify samples or Hermitian conjugacy.
 */
struct Grid final {
  std::vector<std::uint64_t> original_shape;
  std::int64_t y_axis, x_axis;
  std::string packing, index_order, norm, unit, identity;
  std::array<double, 2> spacing, origin;
};
/** @brief Two ordinary graph inputs and explicit interpretation, without
 * owners. real/imag can refer to any compatible edited producer. Their runtime
 * tiles have independent immutable owners. Copying this object copies no tensor
 * payload.
 */
struct Spectrum final {
  WorkflowInput real, imag;
  Grid grid;
};
/** @brief Append FRQ-01A/B or FRQ-02A/B transactionally.
 * member is fft2/ifft2/rfft2/irfft2; input arity is two except rfft2 (one).
 * grid describes the frequency side, including the explicit original odd/even
 * width for irfft2. Keys are frequency.fft2/ifft2/rfft2/irfft2. Outputs use
 * that member's registered port names. Ordinary floating-point FFTs use
 * native binary32 arithmetic for Float32 data and binary64 for Float64.
 * Dimension-derived coefficients are generated in binary64 and converted to
 * the working dtype. Explicit FMA is permitted by this ordinary FFT contract.
 * Correct rounding, per-component ULP bounds and bitwise reproducibility
 * across algorithms/platforms are not promised. No numerical refinement or
 * implicit precision dispatcher is performed. Static syntax/shape errors return
 * InvalidArgument or compile-time TypeMismatch; numerical domains are
 * unchecked. bad_alloc may escape. Serialize edits to one document; independent
 * documents are safe. References borrow no payload/cache. Execution propagates
 * admission, cancellation and upstream failures; successful Result/read-window
 * owners may outlive their ExecutionContext.
 */
PHOTOSPIDER_OPS_API Result<std::vector<WorkflowNodeOutput>> transform(
    WorkflowDocument& document, const std::string& member,
    const std::vector<WorkflowInput>& inputs, const Grid& grid);
/** @brief Append an explicit bit-preserving full-grid permutation.
 * inverse=false requires unshifted input; inverse=true requires shifted input.
 * axes is a nonempty unique axis set; the returned grid changes index_order.
 * Errors, lifetime, thread and allocation rules match transform. Half grids are
 * rejected structurally. Each component can be exported independently.
 */
PHOTOSPIDER_OPS_API Result<Spectrum> shift(
    WorkflowDocument& document, const Spectrum& input,
    const std::vector<std::int64_t>& axes, bool inverse);
/** @brief Append explicit full unshifted Hermitian projection, without
 * validation. profile is strict/accelerated_apple_silicon/accelerated_x86_64.
 * Error/owner/thread rules match transform.
 * Self-conjugate real samples are bit copies, imaginary samples are structural
 * +0. Other pairs use exact RN component averages; numerical conjugacy is not
 * checked or certified.
 */
PHOTOSPIDER_OPS_API Result<Spectrum> project_hermitian(
    WorkflowDocument& document, const Spectrum& input,
    const std::string& profile);
/** @brief Append FRQ-06A with explicit dimensionless response interpretation.
 * Grid shape, packing, axes and index_order must match; response norm does not
 * multiply spectrum amplitude. broadcast_axes may contain only non-frequency
 * singleton response axes. Units/spacing association and samples are unchecked.
 * The output grid is input.grid. profile follows project_hermitian; lifetime
 * and failure rules match transform.
 */
PHOTOSPIDER_OPS_API Result<Spectrum> multiply(
    WorkflowDocument& document, const Spectrum& input, const Spectrum& response,
    const std::vector<std::int64_t>& broadcast_axes,
    const std::string& response_unit, const std::string& profile);
/** @brief FRQ-07/08 execution configuration. All fields are explicit.
 * shape is same/full/valid; normalization is none/sum/l1. Kernel is a rank-2
 * Float64 tensor. fft_shape is padding for linear or per-block FFT shape for
 * overlap_add/save. payload_shape is used only by blocked members. Axes refer
 * to the full logical input rank. Anchor is inside the kernel; boundary is
 * fixed zero, or wrap for circular convolution. No implicit color processing.
 */
struct ConvolutionOptions final {
  std::int64_t y_axis, x_axis;
  std::array<std::int64_t, 2> anchor;
  std::string shape, normalization;
  std::array<std::int64_t, 2> fft_shape, payload_shape;
};
/** @brief Append
 * linear_fft_convolution/circular_fft_convolution/overlap_add/save. Uses
 * ordinary floating point, with no per-component ULP guarantee. Errors,
 * ownership and document serialization rules match transform(). Kernel values
 * are converted to the input's working dtype; Float64 is never narrowed.
 */
PHOTOSPIDER_OPS_API Result<WorkflowNodeOutput> convolve(
    WorkflowDocument&, const std::string& member, WorkflowInput input,
    WorkflowInput kernel, const ConvolutionOptions&);
/** @brief Append dct2/idct2. type is I/II/III/IV; norm is backward/ortho.
 * inverse identifies the forward matrix being inverted. DCT-I dimensions must
 * both be >=2. Other dimensions may be one. Output has input shape/dtype.
 * The returned ordinary numerical tensor is interpreted as a DCT grid by the
 * caller's explicit type/norm/axes, never implicitly as RGB. F-profile error,
 * lifetime, allocation and document-thread rules match transform().
 */
PHOTOSPIDER_OPS_API Result<WorkflowNodeOutput> dct(
    WorkflowDocument&, WorkflowInput, std::int64_t y_axis, std::int64_t x_axis,
    const std::string& type, const std::string& norm, bool inverse);
/** @brief Static named wavelet configuration, without tensor payload owners.
 * profile is
 * haar_mean_lifting_v1/cdf53_float_lifting_v1/stationary_haar_mean_v1. levels
 * >=0, bounded by shapes and checked stationary offsets. Axes are distinct,
 * dtype Float32/Float64. source_grid/unit identify caller-owned interpretation.
 * spacing/origin and association premises are unchecked.
 */
struct WaveletOptions final {
  std::vector<std::uint64_t> original_shape;
  std::int64_t y_axis, x_axis, levels;
  ElementType dtype;
  std::string profile, source_grid, unit;
  std::array<double, 2> spacing, origin;
};
/** @brief One immutable coefficient reference and its static interpretation.
 * Detail levels start at one. The final LL is at options.levels (zero for
 * identity). orientation is LL/LH_y/HL_x/HH. Original and actual shapes,
 * sampling, crop, axis order, boundary, phase and rounding are explicit.
 */
struct WaveletBand final {
  std::string band_id, role, orientation, boundary, phase, axis_order;
  std::string analysis_profile, synthesis_profile, rounding_profile;
  std::uint64_t level;
  std::vector<std::uint64_t> shape, original_shape;
  std::vector<RegionDimension> valid_crop;
  std::array<std::uint64_t, 2> sampling_step;
  WorkflowInput value;
};
struct WaveletBands final {
  std::string schema;
  WaveletOptions options;
  std::vector<WaveletBand> bands;
};
/** @brief Append a complete FilterBands/v1 static directory transactionally.
 * SDK composition respects the fixed Result member limit without a global
 * five-level cap. Compilation retains only requested bands and predecessors.
 * Materialized identity outputs use COPY and independent tile allocations.
 * Native float/double lifting retains formulas, x-then-y analysis, y-then-x
 * synthesis and specified boundaries; bit-perfect reconstruction is not
 * promised. Error, ownership, allocation and thread rules match transform().
 */
PHOTOSPIDER_OPS_API Result<WaveletBands> decompose_wavelet(
    WorkflowDocument&, WorkflowInput, const WaveletOptions&);
/** @brief Reconstruct compatible edited/reordered coefficients.
 * Checks the complete static directory including saved original shape, dtype,
 * role/level, phase and profile. No runtime association or coefficient scan.
 * Producer identity is irrelevant. Ordinary F-profile and lifecycle rules
 * match decompose_wavelet(). No hidden precision refinement or result cache.
 */
PHOTOSPIDER_OPS_API Result<WorkflowNodeOutput> inverse_wavelet(
    WorkflowDocument&, const WaveletBands&);
}  // namespace ps::frequency
