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
repository_branch: ops-impl
repository_commit: 30478d33
---

# NUM-02B: arange

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

The maintainer selected a separate step-defined sequence operator with the
`start`, `step`, `count` interface. This fixes length explicitly rather than
terminating a floating-point loop at an end value.

The selected output support includes Int64 as well as Float32/Float64 sequences.
Integer mode computes exactly without a Float64 intermediate, including integers
whose magnitude exceeds 2^53. The three independent strict/CPU-accelerated keys
follow the common NUM rule and must give identical result bits.

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

## Independent demand, failures and dirty mapping

| Observation | Data/validation payloads | Not requested |
| --- | --- | --- |
| values[0], any N | Start only | Step and axis |
| values[i], i>0 | Start and step | Other indices and axis last |
| axis, N=1 | Start only | Step |
| axis, N>=2 | Start and step; exact last validation | Values output |
| Empty | None | All runtime inputs |

An overflow at a later index does not fail an earlier requested index. Overflow
of axis last affects a requested axis only. Likewise an invalid unused step
cannot fail values[0]. Static descriptors and mode matching still validate
the declared step edge even when its payload is undemanded.

Values use per-index Dependency execution. Axis is an independent Whole tuple
of three components (24 bytes for either axis dtype). A full-array execution
fails if a requested member fails; the host's supported atom API can retain
independent outcomes. Optional joint execution must not add reads or union
validation obligations between independent members.

Changing start invalidates all values and axis; changing step invalidates only
indices >0 and axis when N>=2. For N=1 it invalidates neither output. Output
descriptors are statically known from count/dtype; axis dtype is Int64 only
for Int64 output, and Float64 otherwise. Preserve transitive input support in
cache records even when changed inputs coincidentally produce the same numbers.

Inputs accept legal immutable `[1]` views including offsets, unaligned reads
and signed/zero strides. Use generic Value-port and recognized facet validation,
without inheriting color/physical-unit interpretation. Publish generic packed
owned values fragments at their actual global indices/Regions and packed axis
at origin zero, both with empty facets. No implicit zero fill or mutable alias
is exposed. Returned owners may outlive the context; release unpublished work
on failure/cancellation and published storage at its final owner.

## Algorithm, numerical versions and budgets

Integer start/step fit signed 64 bits and i<2^20, so a signed 128-bit temporary
is sufficient for exact product-plus-sum before final Int64 checking. A portable
equivalent multiword implementation has the same semantics; ordinary signed
Int64 overflow must never be used as a computational shortcut.

Floating reference evaluation uses exact dyadic arithmetic followed by one
destination rounding. Correctly rounded Float64 fma with exactly represented
index may implement Float64 results; Float32 from Float64 inputs still needs
direct-rounding protection. SIMD/parallel implementations operate on requested
global indices and must not evaluate tail indices outside the request. No
speculative prefix overflow rejects a later exact in-range value.

The three keys produce identical bits and semantic failures. Use strict fallback
when an accelerated rounding path cannot establish equality. Unsupported CPU
platform keys return BackendUnavailable, without silent operator substitution.
Report actual platform/profile and fallback counts through host-owned execution
diagnostics as required for NUM-02A/NUM-01. Resource, upstream and cancellation
failures are not recoverable numerical fallback events.

For M demanded values, payload is M*b bytes, where b=8 for Int64/Float64 and
b=4 for Float32; requested axis adds 24 bytes. The maximum full array therefore
uses 8 MiB or 4 MiB. Per active observation, store at most two scalar inputs,
an index and exact arithmetic/rounding scratch. Integer work is O(1) per value;
bounded Float64 exponent ranges/count bound exact-dyadic scratch sizes, with
actual limb work and temporary capacity charged. No O(N) temporary sequence
is needed for an ROI, and no mandatory disk backing is required.

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
ArithmeticOverflow on the actual requested observation. Preserve the upstream
source, cancellation/stale statuses and ResourceExhausted reason/scope. Include
global sample index or axis-last identity; do not invent an earlier failure
at an unrequested index.

| Test | Independent expected behavior |
| --- | --- |
| A01 | Int64 (start=3,step=-2,N=4) -> `[3,1,-1,-3]`, axis `[3,-3,-2]`; zero step generates a constant sequence |
| A02 | Float (0,0.25,5) -> `[0,0.25,0.5,0.75,1]`, axis `[0,1,0.25]` |
| A03 | N=1 ignores a failing step producer and returns `[start]`, `[start,start,0]` |
| A04 | Int64 start=2^53+1,step=1,N=3 -> exact `[9007199254740993,9007199254740994,9007199254740995]`, with exact Int64 axis |
| A05 | start=INT64_MIN,step=INT64_MAX,i=2 -> INT64_MAX-1 despite an overflowing Int64 product |
| A06 | start=INT64_MAX,step=1,N=2: values[0] succeeds; values[1] and requested axis overflow independently |
| A07 | start=-DBL_MAX,step=DBL_MAX,N=3 produces `[-DBL_MAX,0,DBL_MAX]` without intermediate multiplication overflow |
| A08 | Float32 direct rounding, signed zeros, subnormals, allowed repeated floats, final-output overflow and restored rounding environment |
| A09 | Wrong modes/shapes/count, mixed integer/float rejection, dynamic bindings and strided scalar views |
| A10 | Regional/disjoint and full results match at requested indices; values[0] ignores step; axis independence, joint and SIMD-tail legality |
| A11 | Exact dirty support, warm/cache-off behavior, cancellation, budget failures and output owners surviving context destruction |
| A12 | Three CPU profiles match the same integer/dyadic oracle and expose actual fallback/platform diagnostics |

The required target public workflow declares start/step bindings, creates the
chosen arange node with explicit count/dtype, and names values/axis outputs.
Compile once, rerun with changed inputs, then request early and overflowing
indices independently. Inspect exact Int64 bytes and output read witnesses.
Use exact integer/dyadic reference values, never a floating oracle for Int64.
Implementation delivery supplies actual target/build/run commands and observed
outputs.

## Implementation and references

All three arange profiles are registered in `numeric_sequences.cpp`, with
bounded exact integer/dyadic formulas and scalar, NEON or AVX2 limb arithmetic.
C++ metadata inference preserves Int64 axis for integer inputs and Float64 axis
for floating inputs. Values are per-index observations; axis is one tuple.
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
