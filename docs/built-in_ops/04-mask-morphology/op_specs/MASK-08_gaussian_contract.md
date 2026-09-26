---
spec_schema_version: 1
id: MASK-08
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
---

# MASK-08: Finite Gaussian feather with one specified rounding boundary

Inherit the complete [MASK baseline](MASK_common_contract.md), including its
precision, descriptor, error, demand, ownership and acceptance obligations.
This family proposes distinct members; it registers no dispatcher or legacy alias.

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
Each radius is a nonnegative Int64. Support sizes, coordinate offsets, tap
counts and byte-size calculations use checked arithmetic. Admission accounts
for the actual requested support, coefficient/refinement state, intermediates,
output and retained inputs under host capacity, work and stage budgets.
A valid radius can fail with ResourceExhausted when admitted work cannot be
completed within these budgets; parameter validity is not a completion promise.
Resource exhaustion must not silently shorten the radius, truncate positive
coefficients, reduce precision or substitute an approximate filter. Arithmetic
overflow follows the common failure contract. Cancellation polling includes
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
support. All required input values are validated finite [0,1].

Zero: samples outside D are +0, and the denominator is NOT edge-renormalized.
Replicate: map index i to min(max(i,0),n-1). Reflect-half: take j=i mod (2*n)
with Euclidean modulo, map j if j<n else 2*n-1-j; for n=1 this always yields 0.
This is not whole-sample mirror. Validate/read the union of mapped source cells,
not repeated physical copies or an enclosing sparse gap. Radius 0 in both axes
copies the input bits after validation. Interior constant fields preserve DC;
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
Record hardware, compiler, workers, cache policy, image/ROI dimensions, sigma,
radii and refinement/fallback counts. Numerical latency or memory targets require
implementation measurements and an explicit acceptance target; this specification
does not assert unmeasured millisecond limits or production performance.

## Sources, independent evidence and review boundary

[S10](../research-sources.md#s10); [S11](../research-sources.md#s11)

The reference material supports the explicitly cited concept, not every project
choice in this draft. Member examples and the [oracle suite](../../../../examples/mask_morphology_oracle/README.md)
are the acceptance starting point. Proposed scope does not relax the inherited
NUM numerical standard.
