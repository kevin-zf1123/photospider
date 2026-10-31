---
spec_schema_version: 1
id: GRD-04
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# GRD-04: Bounded and signed levels

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-04A](GRD-04A_levels_bounded.md) | Bounded levels | `grade.levels_bounded` |
| [GRD-04B](GRD-04B_levels_signed.md) | Signed levels | `grade.levels_signed` |

## Current parameters

Both operation keys use the existing scalar-parameter ABI. The current selector profile represents the common contract's ordered component array as `components:String`, containing unique, comma-separated canonical nonnegative decimal indices such as `"2,0,1"`. That order determines the row order in the required dynamic `Float64[Csel,5]` controls tensor. No image/control broadcasting or implicit control defaults are supplied.

| Static parameter | Type | Meaning |
| --- | --- | --- |
| `axis` | Int64 | Required with raw selectors; zero-based component axis within the tensor cell, excluding explicit batch axes. |
| `components` | String | Required with `axis`; ordered selected component indices in the encoding above. |
| `input_description` | String | Optional FMT tensor-description encoding; resolves component selection instead of raw `axis` and `components`. |
| `group` | String | Optional named color group, only with `input_description`. |

Raw selectors make the input coordinate meaning a caller assertion. Either operation may act on the explicitly selected coordinate representation; neither requires linear RGB. Control rows contain `[black,white,gamma,out0,out1]`, use binary64, and are read from the current dynamic input each invocation. The payload premises `black < white` and `gamma > 0` remain unchecked; output endpoints may be reversed or equal.

## Current native computation profile

The source is promoted to binary64, and normalization, power and output mapping remain binary64 until the final cast to the source dtype. In a finite normal regime, normalization may multiply by a precomputed binary64 reciprocal of `white-black`. The output mapping may use fused multiply-add. For `gamma=1`, the power is omitted; an affine reassociation is used only when its finite-range and endpoint-rounding conditions hold.

For positive normal power arguments, the current approximation evaluates `exp2(log2(a)/gamma)`. A 128-cell mantissa reduction bounds the logarithm residual below `1/256`; the logarithm correction uses three terms for the ordinary Float32 output profile and five terms otherwise. The exponential uses a degree-six polynomial for the ordinary Float32 output profile and a degree-eight polynomial otherwise, after reduction to `[-ln(2)/2,ln(2)/2]`. Every coefficient and intermediate remains binary64. The ordinary profile bounds `abs(out0) <= 1`; other profiles additionally guard cancellation amplification. Arguments outside the approximation range, nonnormal normalization coefficients, and overflow-sensitive nonlinear outputs use direct native division and power.

Default-control identity cases preserve source bits as specified by each member. Normalized endpoints select the declared output endpoints; an endpoint selection may be omitted only when the binary64 error bound lies strictly inside the same Float32 rounding interval, including the required zero sign. Signed power restores the sign of the normalized input. The native exceptional-value behavior and continuous comparison tolerances remain those of the common contract.

## Shared mathematics and behavior

Bounded levels clamps the normalized input. Signed levels retains extensions using signed power. black<white is a caller premise; no epsilon modifies their separation.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
