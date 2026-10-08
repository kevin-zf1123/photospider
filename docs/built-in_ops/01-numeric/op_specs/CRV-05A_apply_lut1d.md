---
spec_schema_version: 1
id: CRV-05A
parent_id: CRV-05
function: apply_lut1d
operation_family: curve.apply_lut1d
proposed_operation_keys:
  - curve.apply_lut1d_strict
  - curve.apply_lut1d_accelerated_apple_silicon
  - curve.apply_lut1d_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
verification_status: focused_result_math_ctest_and_installed_consumer
clarification_status: complete
repository_branch: ops-specs
repository_commit: current working tree
---

# CRV-05A: apply_lut1d

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) and the explicitly cited
[CRV-01A linear contract](CRV-01A_interpolate_linear.md) for unchanged rounding,
resources, host errors, ownership and acceptance. The uniform axis, arbitrary
input shape and singleton-table rules below take precedence. Clarification is
complete; specification status remains Proposed while the public operation is
implemented.

## Confirmed purpose

Map each numeric input element through one scalar table[L], preserving the
input array shape in output values. This is independent of the per-channel
multi-table operation and of a scalar-to-vector color ramp.

## Confirmed dynamic interface

Use required Result inputs input, table[L] and axis[3], in that order. Each
contains one tensor member under any schema id/version and member key; use its
complete `sample_shape()`, including batch axes. Input is a numeric array of
arbitrary supported rank. Axis carries start/end/step and can connect to CRV-04's
named axis output; table connects to values. The exact sample-grid definition uses
start/end and table length L. The supplied step is checked for consistency, not
repeatedly accumulated to create knots. Output `values` is a Result using schema
`photospider.tensor` v1/member `samples`, preserving the complete input shape,
selected dtype and empty facets. Detailed axis and dtype rules follow.

## Confirmed interpolation method

Use fixed linear interpolation between adjacent samples in table order, with
their actual reconstructed coordinates. At an exact sample-coordinate hit,
select the corresponding table value directly. No method parameter or implicit
nearest/PCHIP behavior is provided. Source PCHIP can be baked into this discrete
table, but the later LUT application still uses its own piecewise-linear function.

## Confirmed multi-sample axis validity

For L>=2, allow ascending or descending start/end, but require the complete
reconstructed Float64 sample grid to be strictly ordered with no repeated
coordinates. Validate the entire grid for any nonempty application request.
Each coordinate is reconstructed with the sampler's direct endpoint-weighted
RN64 rule, with exact endpoint copies, not accumulated step additions.
Axis.step must match the correctly rounded endpoint-derived step. Invalid
ordering, repeated sample coordinates or inconsistent step fails the observation;
the operator does not merge table entries or silently repair the axis.

## Confirmed domain behavior

Static out_of_domain is reject/clamp/linear_extrapolate, default reject.
Compare the input with the actual minimum/maximum axis coordinates, accounting
for descending tables. Reject fails outside that closed domain. Clamp selects
the nearest numerical-domain endpoint's table value. Linear extrapolation uses
the adjacent pair at that domain end, retaining its actual signed coordinate
spacing. Array index zero need not be the minimum-coordinate endpoint.

For L=1, require axis=[start,start,+0]. Reject accepts only input numerically
equal to start; clamp and linear_extrapolate both return the sole table value,
treating the extrapolated slope as zero. Every requested input is still read and
validated; a one-entry table does not erase that dependency or its errors.

## Confirmed dtype mapping

Input and table independently accept Float32/Float64 and may mix dtypes. Axis
is always Float64 [3]. Output dtype statically selects Float32/Float64. The
constructor defaults it from the authoring `input_type` hint; this hint does not
infer the dtype of the bound Result input, which the compiler validates
independently. Direct nodes supply the selected dtype explicitly. Neither table dtype nor axis dtype forces an implicit output
promotion. Integer numeric inputs require explicit conversion before application.

## Confirmed numerical quality and table demand

Inherit CRV-01A linear's numerical and mathematical selection rules: strict correctly rounds
the complete selected linear interpolation or extrapolation expression once to
output dtype. Both accelerated versions obey the shared final FP32 bound. All
input values, selected table entries and final output must be finite.
Nonfinite demanded data or final overflow fails the Run.

An exact knot hit, clamp or singleton-table constant selects one table entry;
an interpolation/extrapolation segment reads both adjacent entries. Generic table
values outside all evaluated stencils are not additionally finite-checked; full
input collection and typed validation remain required. Exact selection correctly
converts the source value and preserves its zero sign. Other exact zero results
are -0 only when both selected endpoints are -0, otherwise +0; nonzero underflow
retains its mathematical sign. Global axis validity remains an independent
obligation even for a one-entry table read.

## Complete descriptors and axis formula

Require 1<=L<=1048576. Each Result has one tensor member under any schema
id/version/key. Input `sample_shape()` has rank 1..8, positive extents and logical
element count <=2^40; table shape is [L] and axis shape is Float64 [3]. Output
port `values` uses `photospider.tensor` v1/member `samples`, preserves complete
input shape, has the chosen floating dtype and empty facets. Batch dimensions are
part of that shape. No input semantic
facet, color role or unit is inferred or propagated to the generic output;
complete collected recognized typed inputs retain their validation obligations.

Required static String dtype is float32/float64 and out_of_domain is
reject/clamp/linear_extrapolate. The constructor writes the authoring `input_type` hint and reject by default;
direct nodes supply both static parameters explicitly. No interpolation-method, axis-shape or
runtime output-shape parameter is introduced. Static metadata checks cover every
declared edge independently of requested data and infer output without evaluation.

Let a=axis[0], b=axis[1], d=axis[2]. All must be finite. For L=1 require b
to preserve a's bit pattern and d=+0; the singleton coordinate is a. For L>=2:

    expected_step = RN64((exact(b)-exact(a))/(L-1))
    x[0] = a; x[L-1] = b
    x[j] = RN64(((L-1-j)*exact(a)+j*exact(b))/(L-1)), 0<j<L-1

Require a!=b, finite nonzero expected_step with direction b-a, and d bitwise
equal to expected_step. Validate x[j]<x[j+1] for ascending axes, or > for
descending axes, across the complete grid. Exact intermediate arithmetic avoids
overflow in the coordinate formula; a nonfinite derived step is invalid axis.
Signed-zero endpoints retain their supplied bits, but numerical duplicate
coordinates are invalid. The full validation obligation consumes work even if
the requested query hits an endpoint.

For a non-knot query q inside the domain, find its unique adjacent bracketing
pair j,j+1 in table order. Compute the exact mathematical value

    ((x[j+1]-q)*table[j] + (q-x[j])*table[j+1])/(x[j+1]-x[j])

with one final destination rounding. For extrapolation use the nearest domain
end's adjacent pair in the same formula, including negative denominators for a
descending table. Endpoint/clamp/singleton paths apply their exact selection
rules instead. The specification interpolates between rounded Float64 grid
coordinates, not unrounded ideal positions or a rounded fractional index.

## Exact demand and invalidation

Every nonempty Whole request requests complete input, table and axis Results
with Data, Validation and Descriptor (role 13), including typed validation and
upstream failures. It validates the complete reconstructed axis first, then every
finite/domain query before table arithmetic. It computes every output element and
publishes one immutable packed dense Result of the complete input shape, with full
certified coverage and global coordinates. The Result association records the
actual source ObjectIds. Empty reads no payload; static metadata checks still
apply.

Mathematical table selection is unchanged: knot/clamp/singleton converts one
entry; interpolation/extrapolation uses its adjacent pair, independently per
channel. Generic entries outside all evaluated stencils receive no additional
finite scan. Typed validation and upstream collection cover the complete table.
Errors in otherwise-unrequested queries or evaluated channels can fail the Run.
Any input/table/axis edit invalidates recorded output demand; dirty mapping
follows that recorded demand. Cache identity retains all input versions, profile,
metadata and parameters. There is no
per-channel Atom success isolation. Output dtype, shape and empty facets remain
unchanged; arbitrary input offsets, unaligned and signed/zero strides remain
legal. The complete immutable owner survives context destruction.

## Algorithm, resource and failure contract

For M input elements, work is O(L+M log L) plus exact arithmetic and typed
validation. Singleton lookup is constant time per query. `UniformAxis` stores
the directly reconstructed Float64 grid in a Root-owned, host-accounted 8*L-byte
buffer (up to 8 MiB), plus allocator/metadata overhead; fixed exact workspace
and O(rank) coordinate state are also admitted. Input/table reads use authorized zero-copy Root windows over source storage; the operation does not
create a dense copy of the complete table merely to collect it. Root accounts
actual retained owners and provided windows. The operator retains no per-output certificates, rows or
lookup table. Output costs dtype_bytes*M, regardless of requested coverage.

The complete axis uses endpoint-weighted RN64 coordinates. One caller-preserving
floating environment covers axis reconstruction and curve evaluation; unresolved
accelerated bounds still use the same exact fallback. Work/cancellation is checked
in axis generation, input reads, lookup and exact arithmetic, and before publishing.
Resource failure never authorizes skipping axis validation or weaker arithmetic.
Unpublished output/workspace is released; the output transaction is all-or-nothing.
Numeric InvalidDomain/ArithmeticOverflow failures have Run scope. Typed, upstream,
resource, stale, backend and cancellation failures retain their categories.
Whole does not expose per-value fallback counters; report those as unavailable.

## Acceptance and current implementation

Conceptual fixture: table=[0,0.25,1], axis=[0,1,0.5], input=[0,0.25,0.5,1]
-> values=[0,0.125,0.25,1]. Reversing table and using axis=[1,0,-0.5] gives
the same function. A singleton table=[7], axis=[2,2,+0] accepts input=2 under
reject and maps any finite input to 7 under clamp/linear_extrapolate.

Use independent exact coordinate reconstruction and rational interpolation,
with bit inspection for axis-step equality, knots, zero signs and output rounding.
Test all domain modes, descending axes, mismatched step, collapsed coordinates,
L limits, source-type mixing, endpoint narrowing overflow versus finite interior
results, huge cancellation and subnormal queries/results. Three CPU profiles
must have identical logical values and numerical failure classifications.

Public implementation delivery binds input/table/axis, supplies dtype/policy,
and requests named values through Compiler/ExecutionContext with actual run
commands. Verify complete table/input support, mathematically unused generic NaN entries,
global-axis failures, typed closures, dirty mapping, source strides, cache-off,
resource/cancellation and post-context owner lifetime. Also connect each applicable
CRV-04 scalar template directly to table/axis; its allowed constant/repeated grid
may still be rejected by this consumer's stricter axis validity.

Current [field.apply_lut_1d](../../../../plugins/ops/01-numeric/field_apply_lut_1d.cpp)
instead takes a rank-2 field and same-dtype table, static increasing domain and
reject/clip policy, validating the whole table under Whole execution. Current
[lut.apply_1d](../../../../plugins/ops/01-numeric/lut_apply_1d.cpp) uses typed
Float32 input/table with semantic sample origin/step. Neither is this generic
three-input dynamic-axis, versioned contract. No runtime tests of the new keys
are inferred from those legacy operations. The implemented target is validated
as recorded below.

### Maintained implementation and verification

`plugins/ops/01-numeric/lut1d_application.cpp` registers the six scalar/channels
profile keys as Whole Result operations. `UniformAxis` validates the three axis
values and exact step, then stores the complete endpoint-weighted RN64 grid in a
Root allocation charged to Metadata. The program validates every query before
table arithmetic, and descending segments reorder x and y together for the
positive-denominator `ExactCurve` formula. It reads input and table data through
authorized Result windows and publishes one complete immutable packed Result.

The Result manual fixture passes seven groups under Strict and locally available
Apple profiles; the independent Fraction driver passes 1,416 bit-equal cases per
profile. Scalar coverage includes ascending and descending axes, input/table
mixed dtypes, reversed unaligned inputs, zero-stride layouts, caller and worker
floating-point modes, clamp and extrapolation, singleton axes/tables, selected
negative zero, invalid queries and axes, typed validation, and upstream failure.
The fixture also checks Empty, static descriptor limits, cache association after
source replacement, static preparation reuse, sparse requests with full Whole
coverage, the six CRV-04 baking chains, resource rejection, cancellation, and
Result/read-window lifetime.

The maximum L=1,048,576 dynamic grid succeeds with its 8*L-byte allocation
charged to Metadata and Payload below 8*L, confirming no dense table copy. A
sparse 2^39-channel output fails at the LUT node with
`ResourceExhausted/CapacityLimit`. Output and an authorized read window retain a
single 16-byte Payload owner after source/context teardown; releasing both
returns all Root resources to zero. The full maximum physical channel-table
allocation and x86 numerical execution were not run. The focused root selection
passed the manual LUT1D and existing math integration tests 2/2 in 4.74 seconds.
The installed consumer compiled this same source against package 0.32.0 and
passed 1/1 under Strict in 0.38 seconds; its direct Apple run passed all seven
groups. The independent oracle, test identities and reproduction commands are
summarized in the [family verification section](CRV-05_apply_lut1d.md#maintained-implementation-and-validation)
and [numeric workflow
README](../../../../examples/numeric_workflow/README.md#lut1d-application-crv-05).
Earlier performance measurements use the package 0.18 Value adapter and are
historical, not Result performance evidence.

- [Family decisions](CRV-05_apply_lut1d.md).
- [Baking templates](CRV-04_bake_lut1d.md).
- [Operator template](../../00-foundation/spec-template.md).
