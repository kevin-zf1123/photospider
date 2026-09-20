---
spec_schema_version: 1
id: NUM-04K
parent_id: NUM-04
function: round
proposed_operation_keys:
  - numeric.round_strict
  - numeric.round_accelerated_apple_silicon
  - numeric.round_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
repository_branch: ops-specs
repository_commit: 30478d33
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
---

# NUM-04K: round

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Round each floating value to the nearest integral value, with exact halfway
cases choosing the even integer. Preserve shape and dtype; UInt8 and Int64
inputs are identity bit copies. Inherit the [NUM-04 common contract](NUM-04_unary_contract.md).
There is no rounding-mode or decimal-place parameter and no implicit Int64
output conversion.

## Exact behavior

For finite x choose the integer n minimizing |x-n|; a tie selects even n.
Store n in the original dtype, using the input sign for any zero result.
Thus `[-2.5,-1.5,-0.5,-0,+0,0.5,1.5,2.5]` produces
`[-2,-2,-0,-0,+0,+0,2,2]`. Infinities are unchanged. NaNs retain payload/sign
and are quieted. Large finite floats already integral remain unchanged.

Every generic numeric input has a successful numeric output; no integer passes
through floating arithmetic. All three implementations are bitwise equivalent,
independent of caller rounding mode, batching and SIMD width. Host environment
restoration and typed-input validation follow the common contract.

Use exact exponent/fraction tests or a verified ties-to-even integral primitive.
A primitive using ties-away-from-zero does not implement this operation. Classify
NaNs before native operations that could change payload. Work is O(M), element
scratch O(1) and output payload M*b, plus inherited mapping/validation costs.

## Acceptance and current state

Check the explicit halfway vector above, neighbors on both sides of each
halfway point, all integer types/extrema, Float32/Float64 subnormals and large
integral values, signed NaNs and infinities. Use an independent exact-rational
distance/tie oracle plus explicit signed-zero/NaN bit rules. Inherited public
regional/witness/cache/resource/ownership tests apply to all three keys.

The target public fixture binds the halfway vector, compiles a selected round
key and inspects the result bits and dtype through ExecutionContext. These keys
The maintained executable and current validation boundary are documented below.

## Maintained implementation and validation

This operation is registered in `plugins/ops/01-numeric/numeric_unary.cpp` and
exposed through `photospider/numeric/unary.hpp`. It uses exact pointwise Data,
separate typed validation and Atom-scoped failures. Its numerical path follows
[the shared implementation notes](../math-implementation.md).

The [public workflow and commands](../../../../examples/numeric_workflow/README.md)
cover this operation. The combined NUM-04 family suite passed 7,524 independent
integer/Fraction/MPFR cases per profile: Clang 21 strict/Apple locally (MPFR 4.2.0-p12; revalidated 2026-09-21)
and Clang 18 strict/AVX2 in Ubuntu WSL (MPFR 4.2.1). Expanded manual checks and
local installed consumers passed. [Validation and native timing](../math-implementation.md#num-04-validation-and-native-timing)
record the scope and limitations. Manual targets have no CTest/integration
registration; MPFR is used only by the independent Python oracle.
