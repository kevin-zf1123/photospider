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
comparison is not an acceptance test. A reference of zero, a magnitude below
2^-126, a magnitude above 0x1.fffffep127, or uncertain classification uses strict
evaluation. Do not flush subnormals or turn an overflow/domain error into a
finite answer. Strict may still return its specified resource failure.

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
| exp, pow | exp has positive output and may overflow; pow retains its full special/domain table. Ordinary acceleration is limited to exp arguments [-80,80], or positive pow base [2^-16,2^16] and exponent [-16,16], with final range checking. Other legal operands use strict. |
| ln | Positive arguments, signed output, zero at 1; no image-domain clipping. The initial fast input range is [2^-100,2^100], followed by final range checking. |
| radian trig and angles | Angles are radians, not normalized image values. sin/cos outputs are in [-1,1]; tan is unbounded near poles. Initial vector trig reduction is limited to [-1,1] radians. atan2 retains quadrant and signed-zero rules. |
| pi/rational-pi/sinc | Inputs are exact binary or rational multiples as specified, not rounded radian conversions. Named zeros/poles remain exact. Integer/dyadic quadrant reduction precedes small-angle kernels; ordinary sinc admits |x|<=1. The full original argument is retained in sinc denominators. Uncertain poles and final ranges use strict. |
| arithmetic, remap and interpolation | Signed finite numeric units; remap and extrapolation can be unbounded. Domain/edge validation and exact selected endpoints remain unchanged. smoothstep remains in [0,1]. |
| reductions, scans, matrix and calculus | Sum/prefix/integral magnitude grows with length and coefficients; derivative scales inversely with step. No universal [0,1] bound is inferred. Apply the bound independently to each final output and use strict for sensitive cancellation or range uncertainty. |
| CRV interpolation/Bézier/inverse | Coordinates retain their declared units and finite domain. Tangent extrapolation can leave the endpoint range. Monotone segment/root selection is exact; approximate values must satisfy final output error and mathematical interval constraints. |
| RGB/CMYK and coverage | Normalized reference channels conventionally use [0,1], while the primitive's explicit signed/HDR extension remains legal. Coverage/association constraints stay exact. |
| CIELAB/CIELCh | The revised target stores l=L*/100, nominally [0,1] with finite extensions; a*/b*, C* and hue keep their own scales. Apply the FP32 ULP rule in each actual native output coordinate, including l. Existing v1 runtime evidence uses old L* units pending explicit migration; see the [scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md). |
| OKLab/OKLCh, HSL, YCbCr, XYZ | Preserve model units, hue convention, white point, transfer and signed/HDR policy. Values outside [0,1] are permitted exactly where the model specification permits them. |
| LUT/shapers/baking | Explicit axes/domain and out-of-domain policy govern input range. Bake error limits and success gates remain proof bounds; this numeric allowance cannot weaken a requested bake tolerance. |
| uniform/nonuniform lowpass | Signed inputs and negative lobes permit output outside the input min/max. Preserve exact support, zero taps and positive-normalizer validation. Bound the complete normalized sum, not isolated rounded coefficients. |

The FP32 output scale is magnitude-dependent in every row. It does not promise
a fixed UNORM16 error for arbitrary HDR, Lab or integrated signals.

## Implementation and acceptance

Use a conservative enclosure of the strict reference, including source
conversion, coefficient error, arithmetic and final rounding. Approve a candidate
only when the complete enclosure fits its accuracy budget. Fall back for an
uncertain sign/domain/overflow predicate or excessive propagated error. No
probabilistic accuracy or sampled-only runtime guard replaces that enclosure.

NUM-01 preserves strict success/failure decisions: uncertainty near a zero
denominator or invalid function domain triggers strict replay for the affected
sample. SIMD lane composition, tail length, partition and requested Region cannot
change a fixed profile's result. Only demanded input coordinates are read.

Preserve the caller's floating environment and resource/cancellation obligations.
Fallback cannot recover a resource, cancellation or upstream failure. Diagnostics
record actual strict dispatches/fallbacks; cache hits fabricate no arithmetic.
The concrete backend revision, ISA and source/build identity separate numeric
cache entries. Public names, default strict profile and dtype defaults are unchanged.

Independent Fraction/MPFR oracles check strict bits and accelerated final bounds,
including cancellation, subnormal/overflow borders and sensitive compositions.
Endpoint/selection, IEEE payload, support and ownership checks remain exact.
Performance reports identify each workload and distinguish measured speedup,
strict fallback and unmeasured parameter coverage.

## Current implementation

Specification acceptance remains Proposed. The default public
registry resolves 330 profile keys (110 basenames) and 24 maintained legacy keys.
The inventory driver checks this mapping against the runtime registry. Extended
and legacy benchmark modes provide an ordinary workload for each basename and
legacy key, including four separately timed LUT3D Result callbacks. Shared
implementation coverage is not a timing claim for every profile or parameter.
Legacy unsuffixed keys retain their separately documented semantics.

The private source dependency is SLEEF 3.9.0, commit
`906ca7512ee483296780a81a21b9ca715d40dfe1`, with upstream license retained.
Builders [prepare the source manually](../../../../third_party/SLEEF.md) in
`third_party/sleef/`; the upstream source is not tracked in this repository.
Binary64 u10 kernels use explicit AdvSIMD or AVX2/FMA after ISA admission;
Float64 inputs are not narrowed to Float32. The adapter is embedded in static
and shared kernel builds with hidden symbols, no configure-time download and
no consumer SLEEF configuration. Fast-math remains disabled. Backend/source/build
identities include this dependency, preventing reuse of old accelerated cache
entries. Caller rounding modes, exception flags and gradual underflow are preserved.

Kernel results are expanded by four binary64 steps into conservative intervals.
Final acceptance uses a stricter two-ULP32 absolute guard to ensure the public
four-step Float32 bound across binade boundaries. Reference zero, uncertain
classification and results outside the normal finite Float32 scale use strict.
The ordinary atan2 kernel requires both nonzero input magnitudes in
[2^-100,2^100]; its exact signed-zero/quadrant cases precede this check. Other
admitted ranges are listed above. Wider legal inputs remain supported by strict.

| Area | Current algorithm and remaining limits |
| --- | --- |
| NUM-01 | Four-sample node-by-lane scratch, scalar/coefficient reuse, outward RN64 reference intervals, per-sample strict replay and identical tail algorithms. Final output, success/failure and exact coordinate rules are checked separately. |
| Elementary/shared exact arithmetic | Correctly rounded hardware arithmetic under a scoped floating environment; bit-level special cases and exact fallback. Dyadic final ratios use guard/sticky rounding; small divisors use active limbs; directed products share endpoint work. |
| Remap/smoothstep | Exact formula construction, bounded final quotient candidates and strict fallback; regional mapped reads/publication. Smoothstep retains its proven [0,1] range. |
| Sort | Stable exact ordering; project requested boxes to unique logical lines and reuse one accounted permutation per line and output, including non-last axes and disjoint boxes. |
| Prefix/integration | Exact accumulation and at most 64 source samples per read window; every output retains its own prefix certificate. Dense association metadata can remain quadratic. Integral-image rectangles are still accumulated independently. |
| Curves, LUT1D and inverse | Accelerated curve candidates require uniquely rounded Float32 enclosures to preserve monotonicity; Float64 uses exact formulas. Exact cross products reduce collinear PCHIP stencils to linear formulas. Float32 inverse uses bracketed refinement, then strict fallback. General nonlinear Float64 inverse retains exact lattice refinement. |
| Uniform lowpass | Accounted per-continuation coefficient-enclosure reuse, bounded full convolution/normalization and strict fallback. Fragment and per-observation certificate work remains. |
| Matrix | Whole callback with complete input/output and fixed block64 workspace; Apple Float32 selectable Accelerate DGEMM/direct SME FP64/scalar candidates, unique-RN32 certification and exact replay. Float64 retains exact arithmetic; all three candidate implementations are retained for comparison. See NUM-14 for configuration and resource boundaries. |
| Other families | Shared arithmetic improvements apply where called. Nonuniform lowpass retains strict directed integration; LUT3D/parametric formulas retain exact construction; mix retains staged per-observation source selection. No private worker pool or general reduction/2D-scan rewrite is implemented. |

### Current validation

 WSL results establish correctness only. Strict uses bitwise comparisons; accelerated
acceptance checks final FP32-scaled error directly, including Float64 error.

Public checks cover Whole/ROI, partitions and tails; negative/unaligned/zero
strides; caller fenv; cache changes; typed/upstream failures; resource limits,
cancellation and returned/unpublished owner lifetime. Regressions include
neighboring PCHIP queries, exact collinearity versus underflowed slopes,
non-last-axis/disjoint sort lines and upper-edge smoothstep. Native and WSL static/shared installed consumers link
only `Photospider::kernel` and run the expression workflow; the SLEEF license is
installed. Independent
code and contract review findings were corrected and checked. These checks do
not constitute exhaustive parameter/platform or all-input refinement coverage.

Float32 exp in [-80,80] now uses the IQK-derived NEON/AVX2 polynomial with an
exact-rational whole-domain certificate for this admitted interval. It satisfies
the existing final-output rule without evaluating a runtime SLEEF enclosure.
The Float32 IQK certificate is not used as a binary64 expression enclosure.
Range/classification uncertainty retains strict evaluation. See the
[adapter update](../adapter-performance.md) and [exp implementation report](../exp-performance.md) for the
coefficient source, proof, per-lane consistency and measured platform scope.

Float32 sin/cos/sinc now use certified explicit-FMA polynomials on [-1,1];
sinpi/cospi use [-1/4,1/4] with exact quarter-turn landmarks kept in the
algebraic path. Tiny nonzero sine results retain the existing fallback.
Float32 sincpi uses exact integer-unit reduction over its complete finite
domain, a binary64 SIMD central polynomial and hardware division before
Float32 rounding. Its explicit residual factor preserves integer zeros.
The certificate applies to final primitive outputs; it is not substituted for
NUM-01 RN64 AST enclosures or propagated independently through CRV tap sums.
See [trigonometric proof and measurements](../trig-performance.md).
