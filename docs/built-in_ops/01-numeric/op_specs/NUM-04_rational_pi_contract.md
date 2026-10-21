---
spec_schema_version: 1
id: NUM-04-rational-pi
parent_id: NUM-04
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# NUM-04: exact rational pi-multiple inputs

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

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

The ordered Result inputs provide numerator and denominator, each in a Result
with exactly one tensor member in slot 0. Both tensors have Int64 dtype and identical sample shape. Structural metadata and fixed views are checked before issue.
The output is a Result on port
`values`, with the same shape and a static Float32 or Float64 dtype; the
constructors default to Float64. Its schema is `photospider.tensor`, with tensor
key `samples` and no attached domain description. The denominator must be positive at every
computed position as an unchecked caller precondition; unreduced fractions are valid. No diagnostic outcome is promised for nonpositive denominators.
Each operation has strict and independently named Apple Silicon/x86-64 CPU
accelerated versions. Strict correctly rounds the exact mathematical result;
accelerated allows at most four final FP32-scaled representable steps, retaining
the corresponding floating pi-function's exact classification/zero/pole rules.
Use strict fallback when the guarantee cannot be met.

Tanpi_rational at exact half-integer ratios returns the fixed positive quiet
NaN as success. Sincpi_rational at p=0 is +1, and at nonzero integer ratios is
+0. For sinpi_rational/cospi_rational/tanpi_rational, reduced denominators
1,2,3,4,6 define additional correctly rounded common-angle paths in every
profile. Other ratios retain the accelerated bound. Sincpi_rational retains
its whole-function bound and only its own zero/integer exact landmarks.

The exact ratio must survive argument reduction and the
mathematical reference; rounding p/q to float before evaluation is not valid.
The maintained keys and execution evidence are recorded below.

## Complete type, angle and error contract

Both required Result ports must provide positive rank-1..8 tensor shapes with at
most 2^40 logical elements; their slot-0 tensors must have exactly matching
shape and Int64 dtype. The output preserves that shape and uses the selected
static Float32/Float64 dtype. Constructors default to Float64, while direct
nodes provide the `dtype` parameter. No automatic broadcast, integer/float
mixing or alternate denominator-sign mode is provided. Both input Results
remain actual dependencies, including when the numerator is zero.

For p=numerator and q=denominator>0, the exact real multiplier is r=p/q. It has
no negative-zero representation. Retain original numerator sign for specified
integer zero signs. Reduce by exact gcd for canonical ratio evaluation, without
signed overflow at INT64_MIN. Equivalent fractions must yield the same result
within each fixed profile. Both source tile sets remain fixed dependencies.

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

Positive q is a caller precondition. Malformed static parameters use `InvalidArgument`; wrong dtype/shape uses `TypeMismatch` before issue. Upstream, resource and cancellation failures preserve their operational categories. Exact poles remain successful numeric results.

## Demand, algorithm and resource bounds

Each requested output tile lowers to one fixed task reading the intersecting tiles of both Int64 inputs and writing a preallocated dense Float32/Float64 tile. Inputs may have different batch/atomic grids and use the Plan's admitted affine layouts. Prepare validates each port's dtype and view independently of output width. Empty selection emits no tasks or payload reads. Metadata remains static, and successful Result owners survive the context.

Use exact integer reduction modulo two for sine/cosine or one for tangent,
retaining full magnitude for sincpi. Products such as 2*q, 4*p and absolute
INT64_MIN require safe widened/limb arithmetic; never overflow signed Int64
and test afterward. Reduced quadrant, sign and exact landmark dispatch precede
function approximation. For strict evaluation of ordinary ratios, directed
enclosures must include the exact rational input rather than a rounded MPFR/
Float64 quotient treated as exact. Refine until final rounding is proved.

Accelerated evaluation may use a native pi function only with a proved combined
argument/evaluation error meeting the final contract. At non-dyadic r, ordinary
sinpi(float(r)) alone is not that guarantee. Unsupported ranges use strict
fallback. This fixed invocation does not emit per-value fallback counters. Runtime
cancellation and resource errors are sticky failures, not fallback
opportunities that erase them.

Only selected output tiles allocate payload. The complete arithmetic and batch-buffer aggregate is reserved as Host scratch per active worker lane. Int64 operand sizes bound basic reduction width; directed refinement retains fixed capacity and precision limits. Query cancellation before each batch of at most 64 samples and within long arithmetic. Insufficient capacity or unresolved precision returns `ResourceExhausted`. Arithmetic checkpoints do not debit a dynamic sample-work budget.

## Shared acceptance

Test unreduced equivalent fractions, negative numerators, zero, INT64_MIN/MAX,
q=1 and q=INT64_MAX, sparse output selections, all common-angle
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
Exercise sparse/disjoint/strided demands, new bindings, low budgets,
cancellation, fallback behavior and post-context result lifetime. The maintained
implementation uses its own directed arithmetic backend; LLVM libc is not selected.

## Individual specifications

- [NUM-04S sinpi_rational](NUM-04S_sinpi_rational.md).
- [NUM-04T cospi_rational](NUM-04T_cospi_rational.md).
- [NUM-04U tanpi_rational](NUM-04U_tanpi_rational.md).
- [NUM-04V sincpi_rational](NUM-04V_sincpi_rational.md).

- [Floating unary contracts](NUM-04_unary_contract.md).
- [Trigonometric contracts](NUM-04_trigonometric_contract.md).
- [CIELCh exact-angle dependency](CRV-06D_color_ramp_cielch.md).

## Maintained implementation and validation

These four operations share the fixed invocation implementation in `numeric_math_operation.hpp` and are registered by `numeric_unary.cpp`. Prepared input widths remain Int64 for both Float32 and Float64 output. Each requested tile uses immutable prepared metadata and private invocation scratch. See [fixed mathematical execution](../point-math-execution.md).
