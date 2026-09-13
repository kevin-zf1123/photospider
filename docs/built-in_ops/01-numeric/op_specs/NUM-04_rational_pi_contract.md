---
spec_schema_version: 1
id: NUM-04-rational-pi
parent_id: NUM-04
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# NUM-04: exact rational pi-multiple inputs

Inherit the [NUM baseline](NUM_common_contract.md) for metadata, platform keys,
floating environment, error scope, resources and public execution acceptance.
The two Int64 ports and selected mathematical contracts below replace the
floating unary interface. This shared file registers no generic dispatcher.

## Confirmed independent operations

The maintainer requested exact p/q*pi inputs in addition to the existing floating
pi-multiple functions. Add four independent operations: sinpi_rational,
cospi_rational, tanpi_rational and sincpi_rational. Keep the existing Float32/
Float64 sinpi/cospi/tanpi/sincpi interfaces; these new representations do not
replace them or silently interpret a rounded 1/3 as exact one third.

Ordered dynamic ports numerator and denominator are Int64 arrays with identical
shape. Denominator must be positive at every requested position; unreduced
fractions are valid. Output values has the same shape and static Float32/Float64
dtype, default Float64. Exact ratio processing and denominator validation are
per requested position; an unrequested invalid denominator does not fail it.
Each operation has strict and independently named Apple Silicon/x86-64 CPU
accelerated versions. Strict correctly rounds the exact mathematical result;
accelerated allows at most four final output-dtype representable steps, retaining
the corresponding floating pi-function's exact classification/zero/pole rules.
Use strict fallback with diagnostics when the guarantee cannot be met.

Tanpi_rational at exact half-integer ratios returns the fixed positive quiet
NaN as success. Sincpi_rational at p=0 is +1, and at nonzero integer ratios is
+0. For sinpi_rational/cospi_rational/tanpi_rational, reduced denominators
1,2,3,4,6 define additional correctly rounded common-angle paths in every
profile. Other ratios retain the accelerated bound. Sincpi_rational retains
its whole-function bound and only its own zero/integer exact landmarks.

The exact ratio must survive argument reduction and the
mathematical reference; rounding p/q to float before evaluation is not valid.
No new runtime key or conformance result is claimed.

## Complete type, angle and error contract

Both required Value ports have positive rank-1..8 shapes with at most 2^40
logical elements and exactly matching shape/dtype Int64. Output values preserves
shape with empty facets. Required static dtype is String float32/float64;
constructors write float64 by default, direct nodes supply it. No automatic
broadcast, integer/float mixing or alternate denominator-sign mode is provided.
Both inputs remain actual dependencies, including numerator=0.

For p=numerator and q=denominator>0, the exact real multiplier is r=p/q. It has
no negative-zero representation. Retain original numerator sign for specified
integer zero signs. Reduce by exact gcd for canonical ratio evaluation, without
signed overflow at INT64_MIN. Equivalent fractions must yield the same result
within each fixed profile. Cache identities still retain both original sources
and their witnesses. Do not treat ratio equality as permission to ignore an input.

Recognize integers, halves, quarters and the selected reduced common-angle
denominators using integer arithmetic before approximation. Integer p/q gives
sin/tan zero with sign(p), with p=0 giving +0; cos gives parity-dependent +/-1.
Half-integer sin is exactly +/-1, cos is +0, and tan is canonical positive quiet
NaN. Quarter-angle sine/cosine use correctly rounded signed sqrt(1/2) and tan
is +/-1. Reduced thirds/sixths use their exact algebraic trigonometric values,
correctly rounded directly to output dtype. Sincpi at p=0 is +1 and all nonzero
integers are +0. It retains the full ratio in its denominator, not only a
periodically reduced remainder. No input NaN/Inf payload case exists for Int64.

Strict evaluates the stated mathematical function of exact pi*r and rounds
only once directly to output dtype. Sincpi means the whole quotient
sin(pi*r)/(pi*r), not a rounded sine followed by division. No angle*pi product
or rounded rational division may define this reference. Special NaN uses
0x7fc00000 / 0x7ff8000000000000 for Float32/Float64, as a successful tan-pole
result without leaking host floating flags. Other profile classification and
signed-zero rules are exact. Sine/cosine outputs stay in [-1,1] in every profile.

Nonpositive q fails the requested observation with InvalidArgument/InvalidDomain,
identifying denominator and global coordinate. Malformed parameters use
InvalidArgument; wrong dtype/shape uses TypeMismatch at compile/preflight.
Upstream/typed/resource/cancellation/stale failures preserve source/code/reason/
scope. Exact poles are not denominator-validation failures. Numerical failures
are per dependent output Atom; ordinary fail-fast and eligible execute_atoms
isolation remain distinct. A failed observation publishes no partial Value.

## Demand, algorithm and resource bounds

For requested output Q, both numerator and denominator have exact support Q,
with required typed Validation closure separately retained. Empty Q reads no
payload, and an invalid denominator outside Q is not evaluated. Changes to either
source at a coordinate invalidate the corresponding output, plus validation
effects. Descriptor inference uses static metadata/dtype only.

Read arbitrary legal immutable source strides/offsets, including zero/negative
strides and unaligned Int64 values. Return packed owned fragments at requested
global Region/storage origins, with no missing-zero fill or writable aliases.
Keep owners valid after context destruction; cache-off preserves active ownership.

Use exact integer reduction modulo two for sine/cosine or one for tangent,
retaining full magnitude for sincpi. Products such as 2*q, 4*p and absolute
INT64_MIN require safe widened/limb arithmetic; never overflow signed Int64
and test afterward. Reduced quadrant, sign and exact landmark dispatch precede
function approximation. For strict evaluation of ordinary ratios, directed
enclosures must include the exact rational input rather than a rounded MPFR/
Float64 quotient treated as exact. Refine until final rounding is proved.

Accelerated evaluation may use a native pi function only with a proved combined
argument/evaluation error meeting the final contract. At non-dyadic r, ordinary
sinpi(float(r)) alone is not that guarantee. Use strict fallback with actual
per-node diagnostics for unsupported ranges. Runtime cancellation/resources are
sticky failures, not fallback opportunities that erase them.

For M demanded results, output payload is M*b. Account operand windows/owners,
gcd/reduction state, exact-ratio and transcendental limb capacity, region/validation
metadata and all refinement work. Fixed Int64 operand sizes bound basic reduction
width, but do not authorize unbudgeted arbitrary-precision refinement. Poll at
least every 64 samples, within long arithmetic and before publication. Insufficient
precision/work/capacity/stages gives ResourceExhausted, never an unverified angle
or a fallback to the old floating-input semantics.

## Shared acceptance

Test unreduced equivalent fractions, negative numerators, zero, INT64_MIN/MAX,
q=1 and q=INT64_MAX, invalid q only at requested locations, all common-angle
denominators and their near neighbors, and exact tan poles. For example,
p=2^53+1,q=1 has cos(pi*r)=-1 even though converting p to Float64 loses parity;
p=2^53+1,q=2 is a tan pole even when floating p/q rounds to an integer.
For p=INT64_MIN,q=INT64_MAX, retain the tiny offset from -1 rather than rounding
the ratio to -1 before sine evaluation.

Use independent exact rational reduction and directed/high-precision function
or algebraic root oracles. Common angles compare bits across every profile;
other accelerated results compare final ULP and exact classification separately.
Each child spec supplies an analytic fixture. Delivery must execute its actual
public WorkflowDocument through Compiler/ExecutionContext with numerator and
denominator bindings, explicit dtype and named values, supplying real commands.
Exercise sparse/disjoint/strided demands, source changes, cache-off, low budgets,
cancellation, fallback reports and post-context result lifetime. No versioned
implementation, LLVM rational-input support or product/platform run is claimed.

## Individual specifications

- [NUM-04S sinpi_rational](NUM-04S_sinpi_rational.md).
- [NUM-04T cospi_rational](NUM-04T_cospi_rational.md).
- [NUM-04U tanpi_rational](NUM-04U_tanpi_rational.md).
- [NUM-04V sincpi_rational](NUM-04V_sincpi_rational.md).

- [Floating unary contracts](NUM-04_unary_contract.md).
- [Trigonometric contracts](NUM-04_trigonometric_contract.md).
- [CIELCh exact-angle dependency](CRV-06D_color_ramp_cielch.md).
