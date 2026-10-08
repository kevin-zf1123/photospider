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
with exactly one tensor member in slot 0; the Result schema may also carry
fields. Both tensors have Int64 dtype and identical sample shape. Recognized
tensor facets and spatial metadata still undergo full-input typed validation.
The output is a Result on port
`values`, with the same shape and a static Float32 or Float64 dtype; the
constructors default to Float64. Its schema is `photospider.tensor`, with tensor
key `samples` and empty facets. The denominator must be positive at every
logical position; unreduced fractions are valid. Exact ratio processing and
denominator validation apply to every input position, so any invalid
denominator fails a nonempty invocation.
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

Nonpositive q fails the complete invocation with InvalidArgument/InvalidDomain
and Run scope, retaining the offending denominator bits in the diagnostic. There
is no Atom key. Malformed parameters use InvalidArgument; wrong dtype/shape uses
TypeMismatch at compile/preflight. Upstream/typed/resource/cancellation failures
preserve their categories. Exact poles remain successful numeric results.

## Demand, algorithm and resource bounds

For nonempty output Q, both input tensors have full-input support and typed
validation. The coordinator supplies authorized Result tensor windows that
preserve legal immutable strides and offsets, including zero/negative strides
and unaligned Int64 values; the operation does not require packed input
collection. The operation writes one complete packed Result, and the executor
projects requested global coordinates afterward. Empty Result queries retain
static validation and resource admission, then return empty tensor coverage
without input reads, denominator checking or arithmetic work. A nonpositive
denominator outside Q still fails a nonempty Whole invocation. Changes to
either source invalidate all observed outputs. Descriptor inference uses
static metadata only. Results and their source associations keep their owners
valid after context destruction; cache-off preserves active ownership.

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
fallback. This Whole callback does not emit per-value fallback counters. Runtime
cancellation and resource errors are sticky failures, not fallback
opportunities that erase them.

For N full logical results, output payload is N*b even for a sparse consumer.
The authorized operand windows use their Result backing; the operation does not
stage full packed copies of the Int64 inputs. Account source owners, gcd and
reduction state, exact-ratio and transcendental limb capacity, output storage,
region/validation metadata and all refinement work. Fixed Int64 operand sizes
bound basic reduction width, but do not authorize unbudgeted arbitrary-precision
refinement. Poll at least every 64 samples, within long arithmetic and before
publication. Insufficient precision, work, capacity or stage allowance returns
`ResourceExhausted`, never an unverified angle or a fallback to the old
floating-input semantics.

## Shared acceptance

Test unreduced equivalent fractions, negative numerators, zero, INT64_MIN/MAX,
q=1 and q=INT64_MAX, invalid q inside and outside consumer projections, all common-angle
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

These four operations share the Result-backed `WholeTensorProgram` in
`plugins/ops/01-numeric/numeric_math_operation.hpp` and are registered with the
other unary operations by `numeric_unary.cpp`. Each continuation receives full
authorized Int64 tensor windows, computes the complete output with the selected
Float32/Float64 dtype, and publishes one packed Result. Strict and accelerated
evaluation retain the rational contracts above; per-value fallback counters
are not emitted by this Whole callback, while execution-level computed-element
diagnostics remain separate.

The [public workflow and commands](../../../../examples/numeric_workflow/README.md)
document Result bindings and the independent oracle. The root registers
`test_numeric_unary_result` for the strict Result workflow. MPFR is used only by
the independent Python oracle.
