---
spec_schema_version: 1
id: NUM-acceleration
kind: shared_operator_contract
category: 01-numeric
status: Proposed
implementation_status: implemented
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
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
| CIELAB/CIELCh | L* conventionally uses [0,100]; a*/b* are signed, C* and hue retain the individual model's rules. These are not silently normalized to RGB units. The FP32 ULP scale is applied in each channel's native units. |
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

The following describes the maintained implementation on 2026-09-21, based on
`ops-impl@eb0e90c8`. Specification acceptance remains Proposed. The default public
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

### Measured workload scope

Apple M5, Homebrew Clang 21.1.3, RelWithDebInfo, one CPU worker, result cache off.
Compilation/freeze is excluded; one warmup precedes seven measured executions.
The baseline is `eb0e90c8` with matching timing drivers. Times below are
median / maximum microseconds for Apple accelerated Float64 outputs. These
measurements describe the stated fixtures, not a universal throughput guarantee.

| Public workload | Before (us) | Current (us) | Median speedup |
| --- | ---: | ---: | ---: |
| NUM-01 exp(x), [0,1], N=65536, Whole | 9,261,697 / 9,340,981 | 29,687 / 30,055 | 312.0x |
| NUM-01 2*x+1, [0,1], N=65536, Whole | 796,695 / 831,235 | 28,716 / 30,103 | 27.7x |
| NUM-04 exp, N=256 | 36,342 / 36,540 | 198 / 210 | 183.5x |
| NUM-04 ln, N=256 | 14,992 / 15,072 | 194 / 201 | 77.3x |
| NUM-04 sin / cos / tan, N=256 | 64,946 / 65,610; 64,066 / 65,076; 73,550 / 74,797 | 200 / 210; 196 / 202; 201 / 205 | 324.7x / 326.9x / 365.9x |
| NUM-05 pow, N=256 | 43,478 / 44,109 | 232 / 237 | 187.4x |
| Remap, N=256 | 119,946 / 120,467 | 385 / 391 | 311.5x |
| Smoothstep, N=256 | 88,322 / 89,634 | 1,276 / 1,499 | 69.2x |
| Stable sort values, N=256 | 169,426 / 214,594 | 611 / 640 | 277.3x |
| Prefix sum, 256 inputs / 257 outputs | 133,406 / 133,945 | 2,943 / 3,361 | 45.3x |
| Cumulative integral, 256 inputs / 257 outputs | 207,105 / 208,306 | 6,724 / 7,186 | 30.8x |
| PCHIP inverse, 33 collinear knots / 256 queries | 704,835 / 708,815 | 8,344 / 8,420 | 84.5x |

NUM-01 reaches 0.453 us/point for exp and 0.438 us/point for affine evaluation,
meeting the respective 2 and 1 us/point targets. Supplementary [-8,8] workloads
measure exp 27,219 / 27,684 us and affine 26,987 / 27,671 us with zero strict
fallbacks. The internal 256-value exp math benchmark measures 28,701 / 28,803
versus 17 / 20 us; this kernel-only timing is separate from public execution.
Ordinary transcendental fixtures meet the 20x target with zero strict fallback.

Targets are not uniformly met. Mix measures 92,185 -> 89,080 us median (1.03x),
and integral_image [256,2] measures 822,018 -> 800,423 us (1.03x); their repeated
execution/association work remains. Matrix is about 1.1x, LUT3D 1.5x,
parametric Bezier 1.8x and linear inverse 1.7x. Uniform lowpass's five radius-2,
256-disjoint-output fixtures improve only 1.15–1.35x despite zero strict fallback.
They retain 256 fragments/certificates; the shared fragment constructor performs
32,640 pairwise overlap checks. Sinc windows demand 768 unique source samples;
Gaussian uses 1,280 logical taps over 1,025 unique samples. Nonuniform lowpass
improves 1.22–1.64x and retains 256/256 strict fallback. Collinear PCHIP inverse
speedup does not establish speedup for general nonlinear curves. Repeated paired
checks found no sustained >10% regression in the measured cheap paths.

Per-key cost coverage, source support, managed peaks and fallback counts can be
regenerated with `cost_inventory.py` and the workflow drivers. Unique source
support is distinct from repeated reads. Allocation-call counts and stage-time
percentages are not exposed and are not assumed zero. Raw CSV and logs remain
in ignored build directories. Reproduction commands and editable public workflows
are in the [workflow README](../../../../examples/numeric_workflow/README.md).

### Current validation

Native strict/Apple and Ubuntu WSL strict/AVX2 passed the independent oracle
groups: expression 715, unary 7524, binary 14174, range 2826, interpolation 5244,
ordering 2072, scans 2280, calculus 1810, linear/PCHIP 2487, inverse 407 and
uniform lowpass 474 cases per profile. Native Python used MPFR 4.2.0-p12;
WSL used Clang 18 and MPFR 4.2.1. WSL results establish correctness only.
Additional native strict/Apple groups passed reductions 4740, matrix 1110,
LUT1D 1416, LUT3D 1062, parametric Bezier 1428, shapers 4196, non-RGB color
ramps 1784 and RGB 352 cases. Strict uses bitwise comparisons; accelerated
acceptance checks final FP32-scaled error directly, including Float64 error.

Public checks cover Whole/ROI, partitions and tails; negative/unaligned/zero
strides; caller fenv; cache changes; typed/upstream failures; resource limits,
cancellation and returned/unpublished owner lifetime. Regressions include
neighboring PCHIP queries, exact collinearity versus underflowed slopes,
non-last-axis/disjoint sort lines and upper-edge smoothstep. Five focused CTests
(numeric operations, dependency sampling, dependency, resources and compiler)
passed on both targets. Native and WSL static/shared installed consumers link
only `Photospider::kernel` and run the expression workflow; the SLEEF license is
installed. ClangFormat 21 and cpplint passed for 51 changed C++ files. Independent
code and contract review findings were corrected and checked. These checks do
not constitute exhaustive parameter/platform or all-input refinement coverage.
