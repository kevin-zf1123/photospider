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
 * ForwardReal takes facet-free Float64 HW; ImportResponse takes Float64 HW2
 * (or HK2 for half packing); Multiply takes two identically described spectra;
 * InverseReal takes one spectrum. Complete static identity is checked before
 * source reads or allocation. Forward/response/multiply emit validated spectra.
 * InverseReal expands the omitted half by reflected-axis conjugation and emits
 * an explicit real projection with a measured imaginary residual.
 * Two mandatory full complex generations and bounded windows implement DIF
 * butterflies, odd-leaf direct DFT and transposition without a whole-axis RAM
 * allocation. Arithmetic, pages, generations, stages and I/O consume root
 * resources. Nonfinite arithmetic and exhausted limits fail explicitly.
 * Whole-transform support remains Conservative; all results own their backing
 * and input associations independently of the context and optional cache.
 */
PHOTOSPIDER_API Result<OperationDefinition> make_fft_operation(
    FftOperation operation, const SpectrumSpec& spectrum);
}  // namespace ps
