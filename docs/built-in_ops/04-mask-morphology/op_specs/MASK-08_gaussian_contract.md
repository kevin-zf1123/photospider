---
spec_schema_version: 1
id: MASK-08
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
---

# MASK-08: Finite Gaussian feather with one specified rounding boundary

Inherit the complete [MASK baseline](MASK_common_contract.md), including its
precision, descriptor, error, demand, ownership and acceptance obligations.
All members are registered in the bundled strict CPU set through fixed
invocation ABI 8; the specification status remains Proposed.

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-08A](MASK-08A_gaussian_feather.md) | gaussian_feather | Finite normalized Gaussian mask feather |

## Normative shared mathematics and interface

Input Coverage only. Required statics sigma_y,sigma_x finite Float64>=0,
radius_y,radius_x Int64>=0; boundary zero|replicate|reflect_half. Radius is
explicit: no hidden truncate=4 or library default rounding of truncate*sigma.
A constructor may derive radii externally, but records the chosen integers.
There is no fixed semantic radius cap and no image-dimension-derived cap.
Each radius is a nonnegative Int64. Static support sizes, coordinate offsets, tap counts and byte-size
calculations use checked arithmetic during preparation. Admission accounts
for the actual requested support, coefficient/refinement state, intermediates,
output and retained inputs under host capacity, work and stage budgets.
A valid radius can fail with ResourceExhausted when admitted work cannot be
completed within these budgets; parameter validity is not a completion promise.
Resource exhaustion must not silently shorten the radius, truncate positive
coefficients, reduce precision or substitute an approximate filter. Static support/capacity overflow is a preparation error; runtime numerical
premises are unchecked. Cancellation polling includes
coefficient generation and precision refinement. Oracle-only workload caps
are not native parameter limits.
If a sigma is zero, that axis has exactly one tap at zero and MUST have radius
zero; positive sigma with radius zero is also an identity along that axis.

For active taps define g(dy,dx)=exp(-dy²/(2*sigma_y²)-dx²/(2*sigma_x²)),
omitting each zero-sigma term. Z=sum over the entire rectangular support g.
Output RN_T(sum g*sample / Z), with exact real exp and ONE final rounding.
Coefficients, pass intermediates and Z are not individually rounded to T in
the reference. An implementation using separable passes must certify against
this complete 2D reference or use exact/refined intermediates. g>0 for every
finite active tap even where a hardware exp underflows; those taps remain in
support. All required input values are finite [0,1] as an unchecked caller premise.

Zero: samples outside D are +0, and the denominator is NOT edge-renormalized.
Replicate: map index i to min(max(i,0),n-1). Reflect-half: take j=i mod (2*n)
with Euclidean modulo, map j if j<n else 2*n-1-j; for n=1 this always yields 0.
This is not whole-sample mirror. The mathematical support is the union of
mapped source cells, lowered to a fixed set of covering input tiles. Radius 0
in both axes copies the input bits without sample validation. Interior constant fields preserve DC;
zero-border images do not preserve their constant value at canvas edges.

Reference O(Q*(2ry+1)*(2rx+1)) work plus coefficient precision refinement;
O(number_of_taps*precision) admitted coefficient storage. Dense separable
candidate O(N*(rx+ry+1)) is an optimization, not a different rounding contract.
FFT/IIR/infinite Gaussian are distinct algorithms and not silently substituted.
Use MPFR directed enclosures with exact stored inputs; a fixed mpmath precision
or agreeing two successive decimal runs is not a correctness certificate.

## Performance acceptance

Local preview and complete-image execution are separate acceptance workloads
under the same numerical contract. Measure ROI latency and complete-image
latency/throughput separately, together with managed peak memory and cancellation
response. Include support growth with radius, cold/warm coefficient reuse,
Float32/Float64, and the actual requested/source regions. A full-image average
does not establish interactive ROI performance. Implementations may select
different conforming paths by request size without changing observable results.
Record hardware, compiler, workers, Plan/coefficient reuse, image/ROI dimensions, sigma,
radii and refinement/fallback counts. Numerical latency or memory targets require
implementation measurements and an explicit acceptance target; this specification
does not assert unmeasured millisecond limits or production performance.

## Native implementation

[`gaussian.cpp`](../../../../plugins/ops/04-mask-morphology/gaussian.cpp)
and [its fixed-point helper](../../../../plugins/ops/04-mask-morphology/gaussian_fixed.hpp)
implement the `values` output in the input dtype. Coefficient exponents are
constructed as exact rationals, including tiny positive sigma; the native
directed arithmetic produces normalized coefficient enclosures.
The separable path carries unsigned Q120 lower/upper bounds through both
passes. It accepts a sample only when rounding both bounds gives identical
target bits, so there is no target-dtype intermediate rounding.

Ambiguous samples use a complete mapped-support constant test, then directed
evaluation of the full two-dimensional expression at 128 through 4096 fractional
bits. Unresolved certification returns OperationFailed; capacity/work
exhaustion preserves the MASK-common resource failure. Positive coefficients
are never dropped. The retained independent oracle uses MPFR directed
enclosures; production uses the existing native rational/directed workspaces.

Each selected output tile admits its coefficient arrays, refinement workspace
and horizontal intermediates for the mapped source rows. Normal separable work
is O(source_rows*Q_width*(2rx+1) + |Q|*(2ry+1)); refinement has the full reference
cost. Coefficients are generated per invocation and are not cached between
Runs. Both-zero radii copy bits with no coefficient workspace. All loops,
including coefficient generation and refinement, participate in work and
cancellation polling. The optional `max_work` guard follows MASK-common.

## Sources, independent evidence and review boundary

[S10](../research-sources.md#s10); [S11](../research-sources.md#s11)

The reference material supports the explicitly cited concept, not every project
choice in this draft. Member examples and the [oracle suite](../../../../oracle/ops/mask_morphology/README.md)
are the acceptance starting point. Proposed scope does not relax the inherited
NUM numerical standard.
