---
spec_schema_version: 1
id: NUM-04B
parent_id: NUM-04
function: neg
proposed_operation_keys:
  - numeric.neg_strict
  - numeric.neg_accelerated_apple_silicon
  - numeric.neg_accelerated_x86_64
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

# NUM-04B: neg

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Negate each observed numeric value. Inherit [NUM-04 common requirements](NUM-04_unary_contract.md)
for input/output names, shape preservation, empty output facets, regional and
typed-validation support, ownership, resources, NaN handling and validation.
There are no static parameters. Input and output dtype are the same, restricted
to Int64, Float32 or Float64. UInt8 is rejected with TypeMismatch and requires
an explicit cast before negation; there is no implicit output promotion.

## Exact behavior

- Int64 computes mathematical -x. INT64_MIN fails the requested observation with
  OperationFailed/ArithmeticOverflow because +2^63 is not representable.
- Floating values flip the sign bit, including zero, infinity and NaN. Quiet a
  signaling NaN and preserve its payload before/after this sign-bit transform.
  Thus +0 becomes -0, -0 becomes +0, and the signs of infinities swap.
- No overflow occurs for floating negation, and no finite/nonfinite filtering
  or numerical approximation is allowed. All three keys are bitwise equivalent.

For Int64, check the minimum before signed negation. For floats, use unsigned
bit transport, explicit NaN quieting and sign-bit xor rather than a platform
conversion that may lose payloads. Work is O(M), output payload M*b and element
scratch O(1), in addition to inherited mapping/validation costs. SIMD must preserve
integer-minimum detection and bit rules without reading unrequested tails.

## Independent acceptance

- Int64 `[-7,0,7,INT64_MAX]` -> `[7,0,-7,-INT64_MAX]`; INT64_MIN fails only
  where requested. UInt8 inputs reject before data execution.
- Float32 `0x00000000` -> `0x80000000`, `0xff800000` -> `0x7f800000`, and
  signaling NaN `0x7f812345` -> `0xffc12345`.
- Float64 `0xfff0000000000042` -> `0x7ff8000000000042`; subnormal magnitude
  bits remain unchanged.
- Double negation restores original bits except that an input signaling NaN
  remains quieted. Compare all three versions against an independent unsigned
  sign-bit/integer oracle, and run inherited public ROI/cache/resource cases.

Conceptual public fixture: a bound Float64 `[3]` input `[1,-2,-0]`, one selected
neg key and values output must yield `[-1,2,+0]`. The maintained public workflow command and current validation boundary are documented below.

NUM-01's expression parser unary minus remains a different interface with its own
finite-expression policy; this operation uses the maintained numeric unary registry.

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
