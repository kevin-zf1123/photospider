---
spec_schema_version: 1
id: GRD-01
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# GRD-01: Linear exposure

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-01A](GRD-01A_exposure.md) | Uniform linear exposure | `grade.exposure` |
| [GRD-01B](GRD-01B_exposure_field.md) | Exposure from a spatial EV field | `grade.exposure_field` |

## Shared mathematics and behavior

EV measures stops in linear coordinates. Source reference units remain unchanged. Successive exposure operators retain their own output rounding.

### Current native algorithm

The current profile performs active arithmetic in binary64, including Float32 source values, because the EV input is Float64. The output is converted once to the source dtype. Identity EV and unselected components copy source bits; a zero source preserves its sign for every finite EV.

For ordinary scalar evaluation, compute `gain = exp2(ev)` and then `x * gain`. For extreme EV outside `[-1022, 1023]`, separate the scale: write `x = m * 2^k` with `frexp`, set `n = floor(ev)`, and evaluate `scalbn(m * exp2(ev - n), k + n)`. For finite nonzero binary64 inputs, EV above 2200 necessarily overflows with the source sign, while EV below -2200 necessarily rounds to signed zero. This decomposition avoids an overflowing or underflowing intermediate gain when the final mathematical scale is representable.

Exposure-field packets in `abs(ev) <= 900` may use a polynomial native approximation. Set `n = round_ties_even(ev)`, `r = ev - n`, and `z = r * ln(2)`, so `abs(z) <= ln(2)/2`. Approximate `exp(z)` by its degree-six Chebyshev interpolant for Float32 output or degree-eight interpolant for Float64 output. For degree `d`, the interpolation nodes are `z_j = (ln(2)/2) * cos((2*j+1)*pi/(2*(d+1)))`, `j=0..d`. Store the monomial coefficients in binary64 and evaluate them using binary64 Horner fused multiply-add stages. The gain is the polynomial value multiplied by the exact normal power of two `2^n`. The interpolation relative remainder bounds, with room for coefficient rounding, are `3.9e-9` and `1.7e-12`, respectively. Final multiplication and output conversion follow the calculation dtype above; arithmetic rounding is evaluated with the common native acceptance criteria.

To keep finite/overflow classification independent of packet and scalar-tail placement, recompute a packet using the scalar sequence above when an active approximate product has absolute value at least `max_finite(output_dtype) * (1 - margin)`. The margin is `2^-24` for Float32 output and `2^-28` for Float64 output. EV outside the packet interval uses the same scalar sequence. Gradual underflow and the declared identity/bypass bit copies remain part of the profile.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
