---
spec_schema_version: 1
id: NUM-05
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-05: shared binary contract

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

The selected independent operations are add, subtract, multiply, divide,
minimum, maximum, pow, atan2 and atan2pi. Each receives strict, Apple Silicon CPU
accelerated and x86-64 CPU accelerated keys under the common NUM version rule.
There is no generic function-mode dispatcher.

Both inputs must have exactly the same full sample shape and dtype; output
shape is preserved as ordinary sample axes. Use explicit broadcast and cast
operations for shape/type adaptation. The operator-specific files decide
supported dtypes and output dtype, mathematical rules, integer overflow and
any permitted accelerated approximation. These specifications remain Proposed;
that status is independent of implementation status.

| Spec | Supported dtypes | Accelerated quality |
| --- | --- | --- |
| [NUM-05A add](NUM-05A_add.md) | All four | Bitwise strict equivalence |
| [NUM-05B subtract](NUM-05B_subtract.md) | All four | Bitwise strict equivalence |
| [NUM-05C multiply](NUM-05C_multiply.md) | All four | Bitwise strict equivalence |
| [NUM-05D divide](NUM-05D_divide.md) | Float32/64 | Bitwise strict equivalence |
| [NUM-05E minimum](NUM-05E_minimum.md) | All four | Bitwise strict equivalence |
| [NUM-05F maximum](NUM-05F_maximum.md) | All four | Bitwise strict equivalence |
| [NUM-05G pow](NUM-05G_pow.md) | Float32/64 | <=4 FP32-scaled ULP; exact special classification |
| [NUM-05H atan2](NUM-05H_atan2.md) | Float32/64 | <=4 ULP; exact special directions |
| [NUM-05I atan2pi](NUM-05I_atan2pi.md) | Float32/64 | <=4 ULP; exact special directions |

## Selected floating and operand policy

Floating operations follow NUM-04's IEEE-like result policy: domain errors,
division by zero and floating overflow produce specified NaN/infinity outputs
as successful observations. Both operands at the requested coordinate are read
and validated, including cases where a special numeric identity determines the
result. No implicit short-circuit changes upstream failure/data obligations.

Normally propagate the sole input NaN, or the first input when both are NaN,
preserving its payload/sign and setting the quiet bit. This priority applies
regardless of which NaN was signaling. A newly generated NaN uses the fixed
positive quiet pattern specified by NUM-04. Pow special rules are stated in its
individual file and override only their
explicitly stated cases. Minimum/maximum retain this NaN priority.

Confirmed operator-specific decisions:

- Add/subtract/multiply support all four dtypes and preserve dtype. Floating
  results are correctly rounded to that dtype; integer exact results outside
  UInt8/Int64 fail the complete invocation, without wrap, saturation
  or implicit promotion.
- Divide supports Float32/Float64 only, preserving dtype. Integer conversion is
  explicit; there is no implicit integer quotient mode.
- Minimum/maximum support all four dtypes and preserve dtype, with bitwise
  equivalent profiles. Mixed signed zeros choose -0 for minimum and +0 for
  maximum; same-sign zeros retain that sign. They propagate an input NaN. If both
  inputs are NaN, quiet and
  preserve the first input's payload/sign, following the common priority.

## Interface, demand and execution

Each operation has named inputs `a`, `b` and output `values`, except atan2/atan2pi
use `y` and `x` in that order. Each bound input Result contains exactly one
tensor member in slot 0 and may also contain fields. The Result schema IDs may
differ. Input rank is 1..8, extents are positive, and the full sample shapes
and dtypes match. Typed tensor facets and spatial metadata receive full-input
validation. The `values` output is a Result using schema `photospider.tensor`,
tensor key `samples`, the complete input sample shape as ordinary axes, and
empty facets. Output batch topology is dropped. No implicit broadcast, cast or
unit inference occurs.

Compile/preflight rejects unsupported dtype, mismatched shape or unknown static
parameters before numeric reads. A nonempty Whole request needs complete input
support with Data, Validation and Descriptor roles (mask 13). The coordinator
supplies authorized tensor windows, including legal signed and zero strides;
the operation does not require a packed copy of either input. It validates and
computes the complete packed output Result, after which the executor projects
requested coordinates. Empty requests may use a metadata-only poll, but perform
no sample reads or arithmetic and produce empty tensor coverage. A change to
either input invalidates all observed output coordinates. Special numeric
identities retain both input obligations.

Integer overflow anywhere, including outside the requested coordinates, fails
the invocation with OperationFailed/ArithmeticOverflow/Run and no Atom key.
Failure releases unpublished output and scratch; published Results retain their
immutable storage beyond context lifetime. Inherit [NUM-04 execution conventions](NUM-04_unary_contract.md)
for floating environment, rounding, ownership and cancellation. Output and
fixed scratch use managed resources even for sparse requests. Work is O(N) for
simple operations plus explicitly charged refinement; sparse requests may cost
more work and memory.

Acceptance uses independent exact integer/rational or high-precision mathematical
oracles, not the implementation helper. Exercise all supported dtypes, signed
zeros, subnormals, extrema, NaN payload priority, valid strided inputs, disjoint
requests, typed validation, Whole overflow failure and resource/cancellation cleanup.
A conceptual WorkflowDocument fixture binds both arrays, compiles a selected key,
and reads requested values through ExecutionContext. Implementation delivery
provides actual runnable public fixtures and results as documented below.

## Maintained implementation

All 27 keys are registered by `plugins/ops/01-numeric/numeric_binary.cpp`,
with independently named constructors in `photospider/ops/numeric/binary.hpp`.
The shared Whole Result implementation validates both complete inputs, even
when a numeric identity determines a result. Floating elementary operations
use controlled correctly rounded hardware arithmetic after exact special-value
classification, with exact fallback; integer operations retain checked exact
arithmetic. Accelerated ordinary positive-base power and angle results use
SLEEF binary64 enclosures within the shared admitted ranges. `atan2pi` divides
an angle enclosure by an enclosed pi. Rejected candidates use the certified
strict backend. See [math implementation](../math-implementation.md) for
rounding, scratch, work accounting and unresolved-refinement limits.

The [public example and commands](../../../../examples/numeric_workflow/README.md)
include an editable add/multiply composition, independent integer/Fraction/MPFR
oracles and direct error/resource checks. `test_numeric_binary_result` covers
the strict Result workflow; the installed consumer exercises the same example
source through `Photospider::kernel`. These checks establish CPU behavior, not GPU support.
