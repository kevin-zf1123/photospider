#pragma once

#include "photospider/data/representation.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps {
enum class FftOperation : std::uint32_t {
  ForwardReal = 1,
  ImportResponse,
  Multiply,
  InverseReal
};
/** @brief Canonical HW real-transform identity for the paged CPU recipe.
 * Forward sign is negative and unscaled; inverse uses positive sign and /HW.
 * Both axes are transformed, stored HW, unshifted, origin zero, step one and
 * unit "sample". Packing is Full or width-axis R2CHalf. RealProjectionMeasured
 * uses the supplied finite nonnegative Hermitian acceptance tolerances and
 * never claims a certified floating-point error bound.
 */
PHOTOSPIDER_API Result<SpectrumSpec> fft_spectrum_spec(
    std::uint64_t height, std::uint64_t width,
    SpectrumPacking packing = SpectrumPacking::R2CHalf, double atol = 1e-10,
    double rtol = 1e-12);
/** @brief Complete Float64 HW real inverse and one measured imaginary residue.
 * "pixels" contains the real projection, in HW order; "imaginary_residual"
 * is max(abs(imag(inverse/N))) over the same complete transform. This finite
 * measurement is not an error certificate. Metadata binds the full forward
 * Spectrum identity, including original shape and Hermitian acceptance rule.
 */
PHOTOSPIDER_API Result<SchemaTemplate> fft_spatial_schema(
    const SpectrumSpec& spectrum);
/** @brief Registers one executable stage of the external DIF radix-2 recipe.
 * ForwardReal takes a Result with one unbatched, facet-free Float64 HW tensor
 * and no fields. ImportResponse takes the same Result form with Float64 HW2
 * (or HK2 for half packing). These numeric inputs are validated by tensor
 * shape/type rather than a fixed Result schema ID. Multiply takes two
 * identically described Spectrum Results; InverseReal takes one. Complete
 * static Spectrum identity is checked before source reads or allocation.
 * Forward/response/multiply emit validated Spectrum Results. InverseReal
 * expands the omitted half by reflected-axis conjugation and emits an explicit
 * real projection with a measured imaginary residual.
 * Two mandatory full complex generations and bounded windows implement DIF
 * butterflies, odd-leaf direct DFT and transposition without a whole-axis RAM
 * allocation. Arithmetic, pages, generations, stages and I/O consume Root
 * resources. Nonfinite arithmetic and exhausted limits fail explicitly.
 * Whole-transform support remains Conservative. Published Results own their
 * backing; associations store source ObjectIds but do not own source payload.
 * A loaded field CpuStorage retains its read plan and Result implementation,
 * keeping that field backing readable until the last window is released.
 */
PHOTOSPIDER_API Result<OperationDefinition> make_fft_operation(
    FftOperation operation, const SpectrumSpec& spectrum);
}  // namespace ps
