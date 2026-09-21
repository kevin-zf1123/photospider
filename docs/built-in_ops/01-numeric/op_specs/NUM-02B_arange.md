---
spec_schema_version: 1
id: NUM-02B
parent_id: NUM-02
function: arange
operation_family: numeric.arange
proposed_operation_keys:
  - numeric.arange_strict
  - numeric.arange_accelerated_apple_silicon
  - numeric.arange_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
spec_revision: 0.2.0
document_maturity: D1_draft
implementation_status: implemented
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
repository_branch: ops-impl
repository_commit: 30478d33
---

# NUM-02B: arange

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

The maintainer selected a separate step-defined sequence operator with the
`start`, `step`, `count` interface. This fixes length explicitly rather than
terminating a floating-point loop at an end value.

The selected output support includes Int64 as well as Float32/Float64 sequences.
Integer mode computes exactly without a Float64 intermediate, including integers
whose magnitude exceeds 2^53. The three independent strict/CPU-accelerated keys
follow the common NUM rule: integer values and axis metadata stay exact;
floating values in accelerated profiles obey the shared final FP32 bound.

The selected outputs are `values[N]` and `axis=[start,last,step]`. Axis dtype is
Int64 for an Int64 sequence and Float64 for Float32/Float64 sequences. The axis
therefore preserves integer endpoints and step exactly without passing through
Float64. Floating last follows the exact arithmetic contract below.

Positive, negative and zero steps are all valid. Static count is in
`[1,1048576]`. For count=1, values returns start and axis is `[start,start,0]`;
neither output reads the step payload or schedules its producer solely for this
node. Static schema checks still apply to the declared step edge.

Int64 output requires both scalar inputs to be Int64 `[1]`. Float32/Float64
outputs accept Float32/Float64 `[1]` inputs, allowing different floating dtypes
between start and step. Integer/float mixing and cross-kind conversion require
an explicit cast. No integer passes through Float64 in integer mode.

This specification does not promise compatibility with other libraries' APIs
that infer length from a stop value. No implementation or ADR change is authorized
by this draft. The specification remains Proposed until accepted by the maintainer.

## Parameters and mathematical contract

Input order is start, step. Both are dynamic scalar Values. Count and output
dtype are static. `count: Int64` is required; `dtype: String` is required on
direct nodes and is exactly `int64`, `float32` or `float64`. Constructors infer
Int64 output for two Int64 inputs and otherwise default valid floating inputs
to Float64. An explicit Float32 output remains available. Mixed integer/float
inputs are rejected rather than default-converted. No implicit step/count is
inserted, and changing an input value does not require recompilation.

Define the exact mathematical value for each global index:

```text
v_i = exact(start) + i * exact(step), 0 <= i < N
```

Integer mode uses exact integer arithmetic and checks only the final v_i against
Int64 limits. An intermediate product larger than Int64 is not itself an error
if the exact final sum fits. Floating mode interprets the widened input floats
as exact binary rationals and rounds v_i directly to the output dtype, using
round-to-nearest/ties-to-even. Do not first round an interpolated Float64 value
for Float32 output and do not accumulate a rounded step across indices.

For N>=2, integer axis is `[start,v_(N-1),step]`, with independent Int64 range
validation of last. Floating axis is `[start,RN64(v_(N-1)),step]`, retaining
the widened input start and step. Last is not the narrowed Float32 last sample.
The axis summary describes the generating progression; rounded Float32/Float64
values can repeat. Repeated values and zero steps are valid, without a distinct-
coordinate check. Reconstruct from start, step and global index, not by inferring
a new step from the rounded axis last. N=1 emits `[start,start,0]` without step.

Only actually read floating inputs must be finite; only published results must
fit their output type. Underflow to subnormals or signed zero is allowed, with
gradual underflow enabled and the caller's environment restored. No clipping,
wraparound, NaN/Inf preservation or count truncation is permitted. Index zero
preserves start's signed-zero sign through conversion. For later indices with
an exact zero result, use -0 only if start and step are both -0; otherwise use
+0. Nonzero exact values rounding to zero retain their sign. Singleton axis
uses a positive zero in its third component.

## Whole demand, failures and invalidation

All formal profile keys use a synchronous Whole callback for the selected
output. Nonempty values requests compute all N values; nonempty axis requests
compute its complete three-component Atomic tuple. The executor projects the
owned output to the requested global Region afterwards. Values and axis remain
independent output identities. An axis-only request does not generate values.

For N=1, both outputs read start only. Static specialization excludes the other
port from runtime demand, typed payload validation and invalidation. Its schema
still validates at compile time. For N>=2, both scalars are collected and
validated even for an endpoint-only request. A failed other producer or an
unrepresentable unrequested value therefore fails the selected Whole request.
An axis failure does not certify a separately requested values failure.
Empty requests perform no payload reads or callback work.

Each active input change invalidates all observations of its selected output;
projection restricts returned dirty coverage to the consumer's requested Region.
Input views may have legal offsets, unaligned storage and signed/zero strides.
Outputs are generic packed immutable owners with empty facets. Values own N*b
bytes and axis owns 24 bytes, including when the consumer requests one element.
The public output descriptors, keys and tuple observation identity are unchanged.
Owners survive invocation/context destruction and release at the final owner.

Profile/count/dtype, complete static metadata and witnessed active inputs enter
cache identity. Cache-off preserves arithmetic and ownership. Whole callbacks
have no per-atom numeric diagnostics; report those counters as unavailable.

## Algorithm, numerical versions and budgets

Integer start/step fit signed 64 bits and i<2^20, so a signed 128-bit temporary
is sufficient for exact product-plus-sum before final Int64 checking. A portable
equivalent multiword implementation has the same semantics; ordinary signed
Int64 overflow must never be used as a computational shortcut.

Floating reference evaluation uses exact dyadic arithmetic followed by one
destination rounding. Correctly rounded Float64 fma with exactly represented
index may implement Float64 results; Float32 from Float64 inputs still needs
direct-rounding protection. SIMD implementations operate on all N global indices without tail overread.
The complete values output must be representable before projection.

The three keys preserve identical semantic failures and exact integer/axis
results. Use strict fallback when a floating accelerated path cannot establish
the shared final FP32 bound or strict-reference failure classification. Unsupported CPU
platform keys return BackendUnavailable, without silent operator substitution.
Keep explicit profile keys; Whole per-atom counters are unavailable. Resource, upstream and cancellation
failures are not recoverable numerical fallback events.

For nonempty values, payload is N*b bytes (b=8 for Int64/Float64,
b=4 for Float32); axis independently uses 24 bytes. Maximum values therefore
use 8 MiB or 4 MiB even for a one-index consumer. Constant bounded exact-dyadic
scratch is admitted separately. Values work is O(N), axis work is O(1), with
primitive limb work charged. No intermediate sequence or disk backing is used.

Use existing host allocation/admission/work/stage limits and worker scheduling.
Reserve before allocation and charge old/new temporary overlap. Poll cancellation
before reads, within long exact-rounding work, at least every 64 generated values
and before publishing. Under insufficient capacity/work/stages, fail explicitly
with ResourceExhausted. Cached results are optional; cache-off does not discard
active owners or change arithmetic. These payload estimates are not RSS bounds.

## Error contract and acceptance

Malformed/missing count/dtype parameters use InvalidArgument. Invalid scalar
shape, unsupported input/output dtype or mixed integer/float mode uses TypeMismatch.
Nonfinite read floats use OperationFailed/InvalidDomain. Exact integer range
failure or nonfinite rounded float output/axis last uses OperationFailed /
ArithmeticOverflow with Domain origin and Run scope for the selected output. Preserve the upstream
source, cancellation/stale statuses and ResourceExhausted reason/scope. Include
global failing sample index or axis-last identity; no partial values publish.

| Test | Independent expected behavior |
| --- | --- |
| A01 | Int64 (start=3,step=-2,N=4) -> `[3,1,-1,-3]`, axis `[3,-3,-2]`; zero step generates a constant sequence |
| A02 | Float (0,0.25,5) -> `[0,0.25,0.5,0.75,1]`, axis `[0,1,0.25]` |
| A03 | N=1 ignores a failing step producer and returns `[start]`, `[start,start,0]` |
| A04 | Int64 start=2^53+1,step=1,N=3 -> exact `[9007199254740993,9007199254740994,9007199254740995]`, with exact Int64 axis |
| A05 | start=INT64_MIN,step=INT64_MAX,i=2 -> INT64_MAX-1 despite an overflowing Int64 product |
| A06 | start=INT64_MAX,step=1,N=2: any values request fails; axis also overflows independently |
| A07 | start=-DBL_MAX,step=DBL_MAX,N=3 produces `[-DBL_MAX,0,DBL_MAX]` without intermediate multiplication overflow |
| A08 | Float32 direct rounding, signed zeros, subnormals, allowed repeated floats, final-output overflow and restored rounding environment |
| A09 | Wrong modes/shapes/count, mixed integer/float rejection, dynamic bindings and strided scalar views |
| A10 | Regional/disjoint and full results match at requested indices; N>=2 values[0] still requires step; axis independence, joint and SIMD-tail legality |
| A11 | Exact dirty support, warm/cache-off behavior, cancellation, budget failures and output owners surviving context destruction |
| A12 | Three CPU profiles match the same integer/dyadic oracle and retain explicit platform keys; Whole counters unavailable |

The required target public workflow declares start/step bindings, creates the
chosen arange node with explicit count/dtype, and names values/axis outputs.
Compile once, rerun with changed inputs, then request early indices and verify that later overflow fails Whole. Inspect exact Int64 bytes and output read witnesses.
Use exact integer/dyadic reference values, never a floating oracle for Int64.
Implementation delivery supplies actual target/build/run commands and observed
outputs.

## Implementation and references

All three arange profiles are registered in `numeric_sequences.cpp`, with
bounded exact integer/dyadic formulas and scalar, NEON or AVX2 limb arithmetic.
C++ metadata inference preserves Int64 axis for integer inputs and Float64 axis
for floating inputs. Values retain sample identities; axis retains one tuple, both execute Whole.
The public `numeric/sequences.hpp` helpers supply the documented authoring defaults.

The manual [numeric workflow](../../../../examples/numeric_workflow/README.md)
contains the actual commands and independent expected values. Local Clang
strict/NEON and Ubuntu WSL Clang strict/AVX2 passed the public workflow and
960-case Fraction/IEEE oracle per profile on 2026-09-14. The executable also
checks exact Int64 cancellation above 2^53, requested overflow, tuple behavior,
resources, cache, cancellation, strided inputs and owner lifetime. Installed
public-consumer execution passed locally. These correctness checks add no
integration-test registration or performance claim. Specification acceptance
remains separate from this implementation record.

- [NUM-02 category](../core.md).
- [NUM-02A linspace](NUM-02A_linspace.md).
- [Per-operation template](../../00-foundation/spec-template.md).

Whole migration validation and timing: [sequences Whole](../sequences-whole.md).
