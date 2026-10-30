---
spec_schema_version: 1
id: NUM-acceleration
kind: shared_operator_contract
category: 01-numeric
status: Proposed
implementation_status: implemented
---

# NUM/CRV accelerated precision and range

This contract applies to every floating numeric result of the independently
named Apple Silicon and x86-64 accelerated NUM/CRV primitives. The mathematical
formula, legal input domain and rounding boundaries in each operation define
its **strict reference**. Strict retains its existing correct-rounding and
cross-platform bitwise requirements. An accelerated implementation may return
that reference exactly or use the following bounded approximation.

## Final-result accuracy

For Float32 output, compare the output with the correctly rounded Float32 strict
reference using ordered finite IEEE distance: at most four adjacent representable
steps. Signed zero, NaN/Inf classification and payload rules are checked
separately, not as integer ULP distances.

For Float64 output, retain Float64 inputs, output storage and exponent range.
For a strict reference r in the normal Float32 finite range, require

```
abs(accelerated - r) <= 4 * 2^(floor(log2(abs(r))) - 23)
```

Compare the actual Float64 numbers; converting both results to Float32 before
comparison is not an acceptance test. A reference of zero, a magnitude below 2^-126, or a magnitude above 0x1.fffffep127 is evaluated strictly. The member's IEEE classifications and caller numerical premises are preserved.

The bound applies to each final arithmetic output, each reduction result and
each requested prefix. It is not a per-tap, per-channel-intermediate or per-node
allowance that can accumulate without limit. NUM-01 compares with the final
result of the original stepwise RN64 AST, including its specified final dtype
conversion. Its input coordinates, axis and literal parsing remain exact.
Other exact-whole-formula operators retain that formula as their reference.

Integer results/overflow, Boolean results, indices, stable sorting, support and
branch selection, raw copies, min/max/clamp selection, exact knot/endpoint
selection, named exact landmarks, and special-value rules remain exact. These
are discrete or representation contracts and receive no approximation allowance.
Mathematically guaranteed ranges (including smoothstep and alpha in [0,1]) and
color association rules also remain constraints. Projection onto a proven
mathematical output interval is allowed; silently clipping generic input or HDR
values is not.

## Image budget and extended domains

A UNORM16 step on [0,1] is 1/65535. One sixteenth of that step is
1/(16*65535), approximately 9.537e-7 absolute. On [0.5,1), this is approximately
16 Float32 ULP; at 1 the upward Float32 spacing makes it approximately 8 ULP.
Near zero the number of ULP is larger. It is not a uniform 16-ULP contract.
This task selects the stronger four-ULP bound, not an additional image profile.

| Family | Legal/typical units and range; acceleration treatment |
| --- | --- |
| NUM-01/02 sequences and expressions | Finite Float32/64 endpoints and coefficients; values may leave [0,1]. Coordinates and axis retain RN64 and exact endpoint rules. Final values use the output-scaled bound. |
| NUM-03, NUM-07, NUM-09/10, ordering | Raw storage, integer coordinates/counts and Boolean predicates retain exact rules; floating scatter sums and quantile interpolation use the final-result bound. |
| exp, pow | exp has positive output and may overflow; pow retains its full special/domain table. Overflow and underflow classification is exact. |
| ln | Positive arguments, signed output, zero at 1; no image-domain clipping. |
| radian trig and angles | Angles are radians, not normalized image values. sin/cos outputs are in [-1,1]; tan is unbounded near poles. atan2 retains quadrant and signed-zero rules. |
| pi/rational-pi/sinc | Inputs are exact binary or rational multiples as specified, not rounded radian conversions. Named zeros/poles remain exact. The sinc denominator uses the full original argument. |
| arithmetic, remap and interpolation | Signed finite numeric units; remap and extrapolation can be unbounded. Caller numerical premises and exact selected endpoints remain unchanged. smoothstep remains in [0,1]. |
| reductions, scans, matrix and calculus | Sum/prefix/integral magnitude grows with length and coefficients; derivative scales inversely with step. No universal [0,1] bound is inferred. The bound applies independently to each final output. |
| CRV interpolation/Bézier/inverse | Coordinates retain their declared units and finite domain. Tangent extrapolation can leave the endpoint range. Monotone segment/root selection is exact; approximate values must satisfy final output error and mathematical interval constraints. |
| RGB/CMYK and coverage | Normalized reference channels conventionally use [0,1], while the primitive's explicit signed/HDR extension remains legal. Coverage/association constraints stay exact. |
| CIELAB/CIELCh | Native TensorDescription tuple consumers store l=L*/100, nominally [0,1] with finite extensions; a*/b*, C* and hue keep their own scales. Apply the FP32 ULP rule in each actual native output coordinate, including l. See the [scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md). |
| OKLab/OKLCh, HSL, YCbCr, XYZ | Preserve model units, hue convention, white point, transfer and signed/HDR policy. Values outside [0,1] are permitted exactly where the model specification permits them. |
| LUT/shapers/baking | Explicit axes/domain and out-of-domain policy govern input range. Bake measurement and the explicitly requested quality gate retain their stated evidence level and tolerance; this numeric allowance cannot weaken a requested bake tolerance. |
| uniform/nonuniform lowpass | Signed inputs and negative lobes permit output outside the input min/max. Preserve exact support, zero taps and the caller premise of a positive normalizer. Bound the complete normalized sum, not isolated rounded coefficients. |

The FP32 output scale is magnitude-dependent in every row. It does not promise
a fixed UNORM16 error for arbitrary HDR, Lab or integrated signals.
