---
spec_schema_version: 1
id: NUM-07G
parent_id: NUM-07
function: is_close
proposed_operation_keys:
  - numeric.is_close_strict
  - numeric.is_close_accelerated_apple_silicon
  - numeric.is_close_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-07G: is_close

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compare matching Float32/Float64 inputs `a` and `b` using the symmetric formula

abs(a-b) <= atol + rtol * max(abs(a),abs(b))

The two input arrays have identical dtype and shape. Output `values` is UInt8
with unchanged shape and no attached domain description: 1 means close, 0 means not close. Static
parameters `atol` and `rtol` are required Float64 values, finite and nonnegative,
with no implicit default. Negative zero is numerically zero. Missing or wrongly
typed parameters fail compilation; finite/nonnegative tolerances are unchecked
numerical premises, including during static preparation. No other static numeric
parameters, implicit cast or broadcast is provided.

Inherit the [comparison execution contract](NUM-07_comparison_contract.md) for
fixed two-input tile support, structural preparation, errors, resources, mapping and lifetime.

## Exact predicate and special cases

Finite inputs and tolerances denote their exact binary rational values. Compare
the entire formula exactly, including the equality boundary, with no intermediate
floating rounding, overflow or underflow. All three profiles yield identical
UInt8 bits. Decode significands/exponents and use exact scaled-integer comparison
or a certified equivalent. The Plan reserves fixed scratch before issue; the invocation queries cancellation between batches of at most four samples.

Before the finite formula: any NaN yields 0; same-sign infinities yield 1;
otherwise an infinity paired with any different value yields 0. Both signed
zeros yield 1. NaN payloads do not affect this boolean output. All generic
numeric inputs produce successful results unless an operational failure occurs; no floating domain Status is introduced. Both operands remain declared reads even when classification decides the result.

## Acceptance

Conceptual fixture: a=[1,2,3], b=[1,2.25,4], atol=0.25, rtol=0 yields [1,1,0].
Check symmetry, exact tolerance boundaries and adjacent floats, zero tolerances,
signed zeros, infinities, all NaN positions and unequal input-dtype rejection.
For a=MAX_FLOAT64, b=-MAX_FLOAT64, atol=0 and rtol=1.5, the result is 0;
naive floating arithmetic can overflow both sides to infinity and incorrectly
return 1. Verify this by independent exact rational arithmetic, alongside tiny
subnormal differences and products. No epsilon is added to the comparison.

Exercise a public WorkflowDocument with static tolerances, both inputs and
requested output coordinates when implemented. Apply the inherited strided,
disjoint, structural-validation, lifetime and budget/cancellation cases;
resource exhaustion must fail rather than guess a near-threshold predicate.

All three profile definitions use a bounded exact 4352-bit predicate workspace within fixed CPU tile invocations. Prepare decodes the static tolerances once into immutable binary parts. Output is UInt8 and retains only requested tiles. The specification remains Proposed; acceptance uses temporary public checks with independent rational references and carries no performance claim.

See [NUM-07 fixed execution](../comparison-execution.md).
