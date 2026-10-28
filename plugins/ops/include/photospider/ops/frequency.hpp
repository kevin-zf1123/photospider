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
}  // namespace ps::frequency
