---
spec_schema_version: 1
id: NUM-02A
parent_id: NUM-02
function: linspace
operation_family: numeric.linspace
proposed_operation_keys:
  - numeric.linspace_strict
  - numeric.linspace_accelerated_apple_silicon
  - numeric.linspace_accelerated_x86_64
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

# NUM-02A: linspace

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

This contract records the endpoint-defined sequence generator separately from
the step-defined NUM-02B arange generator. Both independent operators are
implemented; their public entry points and validation are recorded below.
Specification acceptance remains Proposed.

## Confirmed scope

Generate a one-dimensional numeric sequence using `start`, `end`, and `count`,
including both endpoints for a non-singleton sequence. The sequence is intended
to supply numeric inputs to other operations.

The selected named outputs are `values[N]` in Float32 or Float64 (default
Float64), and Float64 `axis[3]`, storing `[start,end,step]`.

Input slots 0 and 1 are dynamic `start` and `end`, each a Float32/Float64 `[1]`
Value. They may have different dtypes. Static count is in `[1,1048576]`;
ascending and descending intervals are supported. For count=1, return `[start]`
and `axis=[start,start,0]`, without reading/validating end payload or scheduling
its producer solely for this node. Static metadata checks still apply to the
declared end edge. Output conversion is defined by the selected dtype contract.

For N>=2, treat the widened endpoints as exact binary rational values and define

```text
values[0] = RN_dtype(start)
values[N-1] = RN_dtype(end)
values[i] = RN_dtype(((N-1-i)*exact(start) + i*exact(end))/(N-1)), 0<i<N-1
```

Each generated value is correctly rounded directly to its output dtype with
round-to-nearest/ties-to-even. Float32 does not go through an intermediate
rounded Float64 interpolated value. No recurrence adds a rounded step across
the sequence; the global index determines each independently requested value.

The constructor writes `dtype="float64"` unless a Float32 output is requested.
Direct nodes supply required `count: Int64` and `dtype: String` explicitly;
the registry does not infer a missing count or synthesize domain inputs.
NUM-01's sampling conventions are references, not automatically inherited
restrictions on this numeric-value sequence.

## Confirmed repeated-value policy

Equal endpoints are valid: for example, start=end=2 and count=3 produce
`values=[2,2,2]` and `axis=[2,2,0]`. Adjacent generated values may also become
equal through floating-point rounding. Neither case is an error. Consumers
requiring strictly ordered/distinct coordinates must validate that property.
This differs intentionally from NUM-01's duplicate-sampling-coordinate rejection.

## Numerical versions

Follow the maintainer's general rule for subsequent NUM specifications: strict,
Apple Silicon CPU accelerated and x86-64 CPU accelerated have independently
named keys. Strict sequence values retain correct rounding; accelerated floating values
follow the shared FP32-scaled bound. Axis and selected endpoints remain exact.

## Axis, special values and rounding

For N>=2, axis is `[start,end,RN64((exact(end)-exact(start))/(N-1))]`.
Compute this exact rational difference/quotient without an overflowing Float64
intermediate. Equal endpoints have step +0. A nonzero exact step may round to
a signed zero; that is valid and consistent with the repeated-value policy.
Axis does not claim that repeatedly adding its rounded step reproduces values.
Reconstruct the conceptual grid from both endpoints and N using the formula
above. Axis retains the original widened endpoints even for Float32 values.

Only actually read inputs must be finite. Every published numeric component must
be finite. A step whose Float64 rounding overflows fails axis only. A requested
value whose chosen output rounding overflows fails that value only. There is no
NaN/Inf preservation or clipping mode. Subnormals and gradual underflow to signed
zero are allowed; restore the caller's floating-point environment.

Endpoint outputs preserve the endpoint's signed-zero sign through conversion.
For an interior exact zero, return -0 only when both endpoints are -0; otherwise
return +0. A nonzero exact result rounding to zero retains its sign. Thus a
constant -0 interval remains a sequence of -0 values, while axis.step is +0.

## Demand, mapping and invalidation

| Observation | Data and validation payloads | Not read |
| --- | --- | --- |
| N=1 values[0] or axis | Start only | End |
| N>=2 values[0] | Start only | End and derived step |
| N>=2 values[N-1] | End only | Start and derived step |
| N>=2 interior values[i] | Start and end | Axis result and rounded step |
| N>=2 axis | Start and end, plus derived-step validation | Any values output |
| Empty | None | All runtime inputs |

The input schema is validated statically, including unused edges, without
reading their payloads or evaluating upstream producers. Finite validation
follows the table; there is no global end/step check for an endpoint-only values
request. Invalid unused inputs cannot fail an independent requested observation.
For example, values[0] succeeds with finite start even if the end producer fails;
for N=2, values can both succeed while the Float64 axis step overflows.

Values use per-index Dependency execution, with global indices preserved for
nonzero and disjoint requests. Axis is a separately evaluated Whole 3-element
tuple; a nonempty axis request can materialize all 24 bytes. Optional joint work
can share actual common reads without expanding any member's validation domain.
Ordinary execute remains fail-fast for requested errors; supported atom APIs can
retain independent outcomes. Failure of a requested axis does not certify failure
of a separately requested values observation.

Changing start invalidates values[0], interior values and axis; changing end
invalidates the last value, interior values and axis. For N=1, end changes
invalidate nothing. Each output descriptor depends only on static count/dtype;
axis has fixed descriptor Float64 `[3]`. Returned values are generic packed
owned fragments at their actual global Region/storage origin, with element-size
stride and no implicit zero filling. Axis is packed at origin zero. Both have
empty facets and no physical-unit or ObjectId-pairing promise.

Inputs accept legal immutable `[1]` views with offset, signed/zero stride or
unaligned storage. Use existing generic port/recognized-facet validation for
actually supplied metadata/data; drop input facets on output. Published buffers
retain allocator ownership beyond the invocation/context lifetime. Release
unpublished work on failure/cancellation and published bytes at the final owner.

Cache identity includes operation/profile version, count, dtype and exact
witnessed input metadata/bits. Preserve transitive evidence rather than treating
equal output numbers as proof of unchanged dependencies. Profile-separated keys
remain separate even though these three implementations produce equal bits.
Cache-off must not change semantics or active output ownership.

## Algorithms and resources

The reference computes the finite binary-rational weighted sum and denominator
exactly, then rounds once to the destination IEEE dtype. Bounded integer limbs
or compensated arithmetic with a proved exact-rounding fallback are permissible.
Do not replace the reference by double arithmetic merely because most values
match. Opposite extreme finite endpoints require neither an infinite difference
nor an overflowed weighted product when the requested value is representable.

Accelerated CPU implementations can vectorize independent observations and use
fast correctly rounded cases with strict fallback for unresolved rounding.
Every result must match strict bits. No output recurrence, SIMD tail overread,
approximate division or reassociation is permitted to change that result.
Platform-specific names on unsupported platforms return BackendUnavailable;
do not silently select another operator. Report actual platform/profile and
fallback work through the same host-owned diagnostics requirement as NUM-01.

For M requested values, payload is M*b bytes (b=4/8), plus 24 bytes for requested
axis. At the count bound, full values use 4/8 MiB. Each active observation needs
at most two widened scalar inputs plus an index and exact-rounding scratch.
Binary64 endpoint exponents and N<=2^20 bound exact integer sizes; charge actual
temporary capacities and primitive/limb work, including old/new growth overlap.
No O(N) intermediate coordinate array is needed for an ROI. Under bounded operand
sizes, work is O(M) plus optional constant axis work; concrete limb and allocation
costs must be reported by the chosen implementation.

Use existing allocator/admission/work/stage services and host workers. Poll
cancellation before input work, between admitted sample batches, within long
limb/refinement work and before publishing. No more than 64 samples occur between
polls. Budget exhaustion returns ResourceExhausted, without truncating count or
publishing a guessed rounded value. Host service failures remain sticky; strict
fallback cannot erase a prior read/resource/cancellation failure. No disk backing
or persistent mutable state is required. Payload bounds do not claim an RSS bound.

## Errors and acceptance

Malformed/missing static parameters return InvalidArgument; unsupported input
shape/dtype returns TypeMismatch. Nonfinite actually read scalars fail the
request with OperationFailed/InvalidDomain; output or step rounding overflow
uses OperationFailed/ArithmeticOverflow. Preserve current upstream identity,
cancellation/stale status and ResourceExhausted reasons. Numeric failures identify
the requested output and global value index (or axis step), without widening a
late failure to revoke prior independent observations.

| Test | Independent expected result |
| --- | --- |
| L01 | (0,1,5) -> `[0,0.25,0.5,0.75,1]`, axis `[0,1,0.25]`; reverse gives reversed values and negative step |
| L02 | (2,2,3) -> `[2,2,2]`, axis `[2,2,0]`; -0 endpoints preserve -0 value bits |
| L03 | N=1 returns start and `[start,start,0]` without invoking a failing end producer |
| L04 | start=1, end=nextafter(1,+inf), N=3 allows the rounded duplicate `[1,1,end]` |
| L05 | start=`0x1p0`, end=`0x1.0000020000001p0`, N=3: middle Float32 is `0x1.000002p0`; Float64-then-Float32 incorrectly gives 1 |
| L06 | Opposite maximum Float64 endpoints, N=3: middle is zero and step finite; N=2 values remain valid while requested axis overflows |
| L07 | Endpoint-only request ignores the opposite failed input; interior request observes both; axis is independent |
| L08 | Float32 output overflow affects only requested unrepresentable values; zero-step underflow and repeated output values are accepted |
| L09 | Mixed input dtypes, dynamic endpoint changes in one plan, invalid static limits and legal strided views |
| L10 | Nonzero/disjoint requests, joint on/off, caller rounding mode, batches and SIMD widths match strict bits |
| L11 | Exact dirty witnesses, warm/cache-off behavior, cancellation, small-ROI versus full-array budgets and final-owner release |
| L12 | Three supported CPU profiles match exact rational rounding; incompatible-platform failure and actual fallback diagnostics |

The target public fixture is a WorkflowDocument with two scalar input bindings,
one selected linspace key, explicit count/dtype, and named values/axis outputs.
Compile once, execute increasing and decreasing endpoint bindings, then request
selected value indices and check actual producer reads and returned bytes.
Use independent rational/IEEE rounding oracles, not a second production call.
Implementation delivery must provide the real public test target, run command
and observed results.

## Implementation

The six sequence keys, including all three linspace profiles, are registered in
`plugins/ops/01-numeric/numeric_sequences.cpp`. `exact_sequence.hpp` uses bounded
integer limbs for exact binary rational formulas and direct IEEE rounding;
`sequence_profiles.cpp` supplies scalar, NEON and AVX2 widening multiplication.
C++ tuple observation metadata makes axis one atomic observation. The public
`numeric/sequences.hpp` authoring helpers write explicit count/dtype parameters.

The manually invoked [numeric workflow](../../../../examples/numeric_workflow/README.md)
provides build/run commands, editable graph construction and expected results.
On 2026-09-14 the strict and Apple Silicon profiles passed local public-workflow
checks and 960 independent Fraction/IEEE cases per profile. Ubuntu WSL Clang
strict and x86-64 AVX2 passed the same oracle. Local installed-consumer execution
also passed. Resource, cancellation, exact cache, tuple certificates, independent
axis failures and owner lifetime have manual checks. No integration registration is included. On 2026-09-20 the manual
[category timing/accounting driver](../../../../examples/numeric_workflow/README.md#native-category-timing-and-accounting)
measured N=1/256 on native Clang Strict/Apple with exact output checks and managed
resource accounting. Specification acceptance remains separate from this
implementation record.

## Related contracts

- [NUM-02 category](../core.md).
- [NUM-01 expression generator](NUM-01_sample_expression.md).
- [Per-operation template](../../00-foundation/spec-template.md).
- [Shared execution contracts](../../00-foundation/contracts.md).
