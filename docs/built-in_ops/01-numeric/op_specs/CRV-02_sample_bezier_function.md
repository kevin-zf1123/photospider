---
spec_schema_version: 1
id: CRV-02
function: sample_bezier_function
operation_family: curve.sample_bezier_function
proposed_operation_keys:
  - curve.sample_bezier_function_strict
  - curve.sample_bezier_function_accelerated_apple_silicon
  - curve.sample_bezier_function_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
spec_revision: 0.2.0
document_maturity: D1_draft
implementation_status: implemented
verification_status: manual_public_workflows_and_independent_oracle
repository_branch: ops-specs
repository_commit: 30478d33
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
---

# CRV-02: sample_bezier_function

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

This specification records the Bezier anchor/handle generator and its
numerical, execution and acceptance contracts. The public implementation is
available while the specification status remains Proposed; implementation
availability does not change the design acceptance status.

## Confirmed purpose and input model

Generate a one-dimensional numeric adjustment curve for downstream operations
and LUT construction. The maintainer selected Bezier anchors and control handles
as the editing/input model. The original `spine` wording means a one-dimensional
adjustment curve, without geometric-path or skeletal-animation scope.

Anchors lie on the curve; handle locations control shape and are not generally
interpolated points. Both quadratic (degree 2) and cubic (degree 3) Bezier curves
are required; higher degree is excluded. A static `degree` parameter selects
2 or 3 for the entire node. All segments have that same degree; mixed-degree
piecewise curves are outside this version.

Control data uses separate `anchors[K,2]` and
`handles[K-1,degree-1,2]` arrays, ordered as x/y in the last axis. For segment j:

| Degree | Bezier control points |
| --- | --- |
| 2 | `P0=anchors[j]`, `P1=anchors[j]+handles[j,0]`, `P2=anchors[j+1]` |
| 3 | `P0=anchors[j]`, `P1=anchors[j]+handles[j,0]`, `P2=anchors[j+1]+handles[j,1]`, `P3=anchors[j+1]` |

Thus the quadratic control offset is relative to the segment start; cubic
outgoing/incoming offsets are relative to the start/end respectively. Anchors
are shared across adjacent segments rather than duplicated in their input data.
The supported anchor count is `2 <= K <= 65536`. Each control-data array accepts
Float32 or Float64 independently; widening to Float64 is exact. Reconstructed
absolute control points use RN64 anchor-plus-offset additions, as defined below.
Only actually demanded numeric components are checked for finiteness.

### Complete port and parameter contract

All keys use this ordered input layout:

| Index/name | Descriptor | Runtime use |
| --- | --- | --- |
| 0 `anchors` | Float32/Float64 `[K,2]`, K in `[2,65536]` | Shared x/y anchors; x order is explicit input order |
| 1 `handles` | Float32/Float64 `[K-1,degree-1,2]` | Relative x/y offsets as above |
| 2 `start` | Float32/Float64 `[1]` | Sampling start |
| 3 `end` | Float32/Float64 `[1]` | Sampling end, undemanded for count=1 |

Array dtypes and scalar dtypes may differ. Accept immutable logical layouts,
including signed/zero strides, offsets and unaligned storage; do not reinterpret
physical row order as anchor order. Use generic Value-port metadata validation
for attached facets, validate actually read recognized typed data, and drop
facets on output. No unit conversion is performed.

| Static parameter | Type/domain | Authoring default |
| --- | --- | --- |
| `degree` | Int64, exactly 2 or 3 | Required |
| `count` | Int64, `[1,1048576]` | Required |
| `dtype` | String `float32` or `float64` | Constructor writes `float64` |
| `out_of_domain` | String `reject` or `clamp` | Constructor writes `reject` |

Direct WorkflowDocument nodes specify all four parameters. No implicit sorting,
anchor insertion, handle synthesis, endpoint selection or registry defaults are
used. Static shape mismatches, degree/handle-arity mismatches and invalid parameter
values are rejected before runtime input reads, including axis-only requests.

## Sampling and dynamic outputs

The maintainer selected NUM-01's sampling/output contract unchanged: dynamic
Float32/Float64 `[1]` start/end inputs, static count in `[1,1048576]`, increasing
or decreasing endpoint-inclusive grids, and N=1 at start with no end read.
Output `values[N]` selects Float32 or Float64, default Float64; output
`axis[3]` is Float64 `[start,end,step]` or `[start,start,0]` for N=1. Both are
generic arrays with empty facets, and units are interpreted by downstream users.
Coordinate rounding and local duplicate-coordinate checks follow NUM-01.

Anchors and handles are dynamic upstream Value inputs with compile-time shapes.
Changing their values or start/end reuses the compiled plan; changing degree,
K, count or output dtype requires compilation. The control data defines the
curve independently of the requested sampling interval.

The static `out_of_domain` policy defaults to `reject`, with explicit `clamp`
also supported. Reject fails an actually requested value whose x is outside
the first/last anchor x interval. Clamp returns the nearest endpoint's y.
Neither policy rewrites the requested axis, so `axis` always describes the
sampling positions originally requested. Tangent extrapolation is not selected.

## Dependency and validation decisions

Before producing any nonempty values request, validate the x topology of the
entire curve: all anchor/handle x values and reconstructed x control points,
strictly increasing anchor x, and x-monotonicity of every segment. This global
x validation is required even when only one output sample is requested.

Read and validate y data only for the segment actually selected by each query.
An invalid unused y value in another segment does not fail this request.
Coordinate-only neighboring-sample checks do not evaluate neighboring y values.
At an exact anchor x, or when domain clamping selects an endpoint, return that
anchor y directly with final dtype conversion. Do not read adjacent y handles
or unrelated y anchors. Global x topology validation still applies. Thus no
left/right segment tie-break changes an exact shared-knot value or its y support.

`axis` is generated from start/end/count alone; it does not inspect anchor or
handle payloads and does not certify curve validity. Static descriptors remain
checked before either output can execute. Empty requests read no runtime data.

## Numerical versions

The selected versions are strict, Apple Silicon CPU accelerated and x86-64 CPU
accelerated, with the distinct proposed keys above. No unsuffixed dispatcher
or compatibility alias is included in the target registration surface.
The accelerated error bound follows the shared final FP32 4 ULP contract
relative to the correctly rounded mathematical y reference. Float64 outputs
retain their dtype and use the FP32 absolute-error scale.
The inverse solver's stopping condition must bound y error; an x residual alone
does not establish this guarantee.

For strict, widen input values exactly to Float64, then reconstruct each absolute
control point by a correctly rounded Float64 anchor-plus-offset addition.
These finite binary64 control points define an exact mathematical Bezier curve.
At a sampled coordinate q, strict finds the unique mathematical root of
`Bx(t)=q` and correctly rounds its mathematical `By(t)` directly to the chosen
output dtype. Fixed-count approximate root iteration is not this contract.
Unlike NUM-01's explicit expression-evaluation sequence, CRV-02 treats the
inverse-plus-evaluation mapping as one mathematical function for final rounding.
The accelerated reference is this same curve and correctly rounded output.
If an accelerated inverse/evaluation path cannot guarantee its four-ULP bound,
it falls back to strict and reports that choice. Correct-rounding refinement
and fallback remain under the active capacity/work/stage budgets. Exhaustion
returns ResourceExhausted; it does not publish a value with unresolved accuracy.

Topology, coordinate generation and anchor-plus-offset reconstruction are common
strict computations for all profiles. Acceleration may change inverse/evaluation
methods within the final-output bound, but cannot accept a backward fold or
move an anchor. Strict returns the same bits on supported CPU platforms.
Each fixed accelerated profile must give consistent bits across sample ordering,
batch/SIMD width and optional joint execution; different platform profiles may
differ within the stated tolerance. GPU/Metal is outside this CPU scope.

Finite anchor hits and clamped endpoints are exact after output conversion in
all profiles, rather than merely within four ULP. A mathematically exact interior
zero has strict result +0; nonzero values rounding to zero retain their sign.
Preserve an anchor's signed-zero y on anchor hits. Gradual underflow is enabled;
no output-domain epsilon, y clamping or flush-to-zero is introduced. Restore the
caller floating-point environment. Output overflow fails; an approximation
cannot convert a nonfinite correctly rounded reference into a finite success.

A platform-specific accelerated key on an incompatible platform returns
BackendUnavailable. Its capability must be recognized rather than silently
dispatching another operator. Report operation/profile/version, actual CPU/ISA
path, evaluated sample count and aggregate strict-fallback sample/reason counts
through host-owned execution diagnostics, without adding a third data output.
Cache hits report reuse rather than fresh solver/fallback calls. Cancellation,
upstream failure and budget failures are not capability fallback conditions.

## Confirmed function interpretation

The curve must define a single-valued function of x. Each segment moves
monotonically forward in x; a fold that gives several curve intersections at
one x is rejected. For a query coordinate q, solve `Bx(t)=q` on the selected
segment, then evaluate `By(t)`. Uniform t sampling is not the requested mapping.
Each segment has strictly increasing x endpoints. Its derivative may be zero
at isolated points, including either endpoint; a stationary derivative alone
does not make the function invalid. Negative derivative anywhere on `[0,1]`
is rejected. There is no epsilon permitting a small backward fold.

### Mathematical segment and monotonicity contract

After RN64 control reconstruction, regard every control coordinate as an exact
binary rational. For degree d in {2,3} and `0<=t<=1`,

```text
B(t) = sum(k=0..d) choose(d,k) * (1-t)^(d-k) * t^k * Pk
```

Strictly increasing anchor x ensures nonconstant x on each segment. Validate
the derivative sign with exact arithmetic or certified directed bounds that
resolve the exact comparison:

- Quadratic: `Bx'(t)/2 = (1-t)*(P1.x-P0.x) + t*(P2.x-P1.x)`.
  Both endpoint differences must be nonnegative.
- Cubic: set `u=P1.x-P0.x`, `v=P2.x-P1.x`, `w=P3.x-P2.x`.
  The derivative divided by three is `g(t)=a*t*t+b*t+c`, where
  `a=u-2*v+w`, `b=2*(v-u)`, `c=u`. Check g(0), g(1), and its vertex
  `t=-b/(2*a)` when a>0 and the vertex lies strictly inside (0,1).
  All checked minima must be nonnegative.

The positivity test is on the polynomial, not just control-point order. Cubic
x controls `[0,3/4,1/4,1]` are monotone despite crossing control handles;
`[0,2,-1,1]` fails at t=1/2. The global anchor sequence and every segment
must pass before any values observation succeeds. Do not overflow intermediate
control differences or derivative coefficients when an exact test is possible.
Derivative-zero tangencies are accepted; a polynomial with these endpoints
and nonnegative derivative has no interval of constant x and hence one inverse.

Adjacent segments require position continuity only (C0), guaranteed by their
shared anchor. Sharp corners are allowed. Handles are independently supplied;
the generator does not align, link or modify them. Tangent continuity is a
caller-controlled construction rather than a validity requirement.

y may overshoot anchor values, be negative, or exceed one. Finite signed/HDR
control values and results are retained without y clamping, automatic monotonic
shape preservation, or handle correction. Range constraints belong to explicit
downstream operations. The out-of-domain clamp policy clamps x lookup to an
endpoint value; it is not a y-range clamp.

For a cubic example with control points `(0,0)`, `(0,1/4)`, `(0,3/4)`, `(1,1)`,
`Bx(1/2)=1/8` and `By(1/2)=1/2`. The query x=1/8 therefore returns y=1/2;
substituting t=x would incorrectly return 107/1024. This example was checked
with independent exact-rational de Casteljau evaluation during clarification.

## Reference execution algorithm

1. Statically validate parameters, input descriptors and output inference.
   On a nonempty request check platform capability, then obtain only the input
   components authorized for the requested output.
2. For axis, evaluate the inherited sampling contract and publish its 24-byte
   payload independently. Do not launch control validation or inverse work.
3. For values, validate the full x topology. Stream all anchor x and handle x
   components in logical order, reconstruct x control points with RN64 and
   perform exact derivative tests. A host-owned immutable validation/index
   object may share this work across active observations of the same frozen
   control data. It carries the complete source witness; a cached boolean alone
   does not authorize later results.
4. Form each demanded global sample coordinate and validate its adjacent
   coordinate separation according to NUM-01. Compare q with the anchor range.
   Reject outside q under reject, or select the endpoint under clamp.
5. If q matches an anchor, request only its y. Otherwise locate the unique
   interval with `anchor[j].x < q < anchor[j+1].x`, then request those two
   anchor y components and the selected segment's y handle offsets. Reconstruct
   only those absolute y control points using RN64 and reject nonfinite inputs
   or reconstructed points.
6. Solve the monotone equation and evaluate its y as follows, then perform
   the selected profile's publication and error checks.

### Inverse and y error control

Use a valid t bracket `[0,1]` for the selected segment. Exact-sign or directed
interval comparisons of `Bx(t)-q` preserve the bracket. Newton iterations are
an optimization only: derivative-zero cases, bracket escapes or unresolved
signs use bracket refinement instead. No raw cubic root formula or residual
threshold is prescribed as a correctness shortcut.

A strict reference can bisect at dyadic t, evaluate exact rational Bx signs,
and refine an outward-rounded enclosure of By over the remaining root bracket.
Restricting the Bezier polynomial to that bracket with interval de Casteljau
gives a valid convex-hull enclosure. Continue until the output rounding is
unique. Exact roots, exact zeros and midpoint/tie cases need exact or certified
algebraic resolution; an interval stuck across a rounding boundary must not
select an arbitrary side. Budget exhaustion terminates with ResourceExhausted.
The reference describes admissible semantics, not a claim that any fixed
iteration/precision cap succeeds for all valid controls.

Accelerated solvers can use CPU vectorized polynomial evaluation and safeguarded
root methods to propose candidates. A valid y enclosure must establish the
final <=4-ULP condition, or the sample falls back to strict. If an enclosure
includes several possible reference outputs, the candidate must be within four
FP32 steps (or the corresponding Float64 absolute-error bound) of every
possible correctly rounded reference output. A zero, FP32-subnormal-range or
out-of-FP32-range reference requires strict evaluation.
An x residual alone is especially insufficient near a stationary x derivative.
Root approximation and polynomial roundoff share one final y error budget.

No root solver is needed for exact anchor/clamp cases. An interior two-anchor
curve still follows its supplied quadratic/cubic handles; K=2 does not imply
linear interpolation. Output shape, domain and control constraints are not
silently simplified to the old linear/PCHIP operators.

## Demand, dirty mapping and returned ownership

Let X be all x components of anchors and handles, Yj the two anchor y values
and y offsets of segment j, and Ai the y component of anchor i. Let D contain
start, and end only when count>=2. Runtime input roles are:

| Observation | Data/control | Validation | No payload demand |
| --- | --- | --- | --- |
| Interior `values[i]` in segment j | D; ordered anchor x for selection; reconstructed local x and Yj for the curve | Full X, D/sampling conditions, selected Yj, numeric solution/output | Other y components |
| Exact anchor `values[i]` | D and selected Ai | Full X, D/sampling conditions, Ai | Every y handle and unrelated anchor y |
| Clamped endpoint value | D and chosen endpoint Ai | Full X, D/sampling conditions, Ai | Other y data |
| Outside-domain reject | D and domain endpoint x | Full X and D/sampling conditions; fail before y reads | All y data |
| Axis | D | Sampling conditions only | All control payloads |
| Empty | None | Static validation only | All runtime data |

Full X is a validation/control dependency, even if the query is far from a
changed segment. Represent it as exact x-column regions; transporting an
unauthorized y column as part of a convenient dense input read is not permitted.
Internal root-refinement samples are t coordinates, not additional output
samples and do not authorize new curve-segment y reads.

Input changes propagate through the retained witness:

- Any anchor/handle x change can invalidate every values observation, through
  global topology validation, but does not invalidate axis.
- A handle y change invalidates values that read its segment's handles;
  exact anchor and clamped endpoint values do not observe that handle.
- An anchor y change invalidates observations reading that anchor, including
  incident segment interiors, the exact knot and applicable clamped values.
- Start/end changes invalidate both requested outputs through sampling support;
  N=1 ignores end entirely. A change of degree, count, shapes or dtype recompiles.

Changed x can move a query to another segment; validation/control support ensures
that old y-only cache support does not hide this change. Preserve all transitive
upstream source/validation evidence when sharing or caching the x index. Support
is exact for this explicitly specified validation protocol; it is not a claim
that a curve's geometric local support alone determines all invalidation.

Return only demanded values at their original global indices and Regions, with
packed owned fragments, correct storage origins and element-size strides.
The full collection path yields `[N]` in sampling order. Axis uses the same Whole
Float64 `[3]` representation as NUM-01. Both outputs have empty facets, immutable
allocator-owned bytes and no implicit zero fill. Value owners may outlive the
ExecutionContext. Release unpublished state on failure/cancellation and published
storage only after its final owner is released.

Values and axis are independent generic outputs without an ObjectId pairing
certificate. A downstream sampling consumer connects them from the intended
generator/input bundle and follows its own compatibility validation. Completed
cache identity includes operation/profile/version, all static parameters and
actual witnessed input bits; backend/ISA/library choices that change values
belong to accelerated profile identity. Strict and accelerated cache entries are
not interchangeable, and failed work is not a successful result entry.

## Resources, work and cancellation

Let S=K-1, d be degree, M the requested values and b the output element size.
The float64-sized upper bound for both complete control inputs is
`16*K + 16*(d-1)*(K-1)` bytes. For cubic at K=65536 it is 3,145,696 bytes,
slightly below 3 MiB. Only the x half is globally required; y reads follow
the support table. Do not mistake total source size for a required packed copy.

| Resource | Bound or required accounting |
| --- | --- |
| Returned values | `b*M`; full count limit is 4 MiB Float32 or 8 MiB Float64 |
| Returned axis | 24 bytes if requested |
| Packed Float64 global x copy, if retained | `8*K + 8*(d-1)*(K-1)`, below 1.5 MiB at maximum cubic K |
| Search index | At most K Float64 anchor x values (8K bytes) plus declared bounded metadata, if separate from the x copy |
| Selected-segment Float64 controls | `16*(d+1)` bytes per admitted active sample, plus source/coordinate and bracket state |
| Exact topology/root/rounding arithmetic | Actual admitted limb/interval capacities and work, including simultaneous old/new buffers on refinement |
| Relations, upstream owners, stages and shared validation | Existing managed root limits; account separately from the payload-only bounds |

Global topology validation costs O(K*d) polynomial checks, with exact-arithmetic
cost charged separately. With a retained validated anchor index, selection is
O(log K) per query, or O(K+M) for an ordered-query sweep. Root/evaluation work is
O(sum of actual refinement work over requested samples). There is no constant
iteration bound implied by the error contract. If sharing/index retention is
not available, repeated validation/search costs must be reported and charged;
correctness does not depend on optional completed-result caching.

Use existing host workers/admission. No private thread pool, mandatory disk
backing, untracked high-precision allocator or hidden mutable registry state is
introduced. Bound concurrent samples by admitted scratch/interval capacity.
Work is debited before loops/refinement stages; unavailable capacity/work/stages
return ResourceExhausted, with no partial successful array or guessed y value.
Managed bounds exclude the documented host allocator/runtime/OS overhead and
do not constitute an RSS guarantee.

Poll cancellation before reads, at least every 64 topology segments, at every
inverse/precision refinement boundary and before publication. SIMD batches
cannot evaluate unrequested output tails. Host read, work and allocation errors
remain sticky and preserve their original origin; fallback cannot erase them.

## Error contract

| Trigger | Phase and Status |
| --- | --- |
| Missing/invalid static degree, count, dtype or domain policy | Compile/direct preflight; InvalidArgument |
| Incorrect ranks/shapes, K outside range, handle arity inconsistent with degree, unsupported dtype | Compile/direct preflight; TypeMismatch |
| Nonfinite x data, nonfinite reconstructed x control, unordered/equal anchor x, backward segment | Before values publication; OperationFailed / InvalidDomain or ArithmeticOverflow |
| Requested x outside domain under reject | This values observation; OperationFailed / InvalidDomain, before y reads |
| Nonfinite actually demanded y or reconstructed y control | This values observation; OperationFailed / InvalidDomain or ArithmeticOverflow |
| Sample-coordinate collapse or unrepresentable sampling step | Inherited NUM-01 request failure |
| Nonfinite correctly rounded output or output dtype overflow | OperationFailed / ArithmeticOverflow |
| Wrong accelerated platform | BackendUnavailable, before payload work |
| Capacity/work/stage exhaustion | ResourceExhausted with corresponding existing reason |
| Cancellation, stale request or upstream failure | Preserve existing code, reason, source identity and scope |

All semantic gates apply equally to strict and accelerated. Different available
budgets or refinement costs may produce different operational failures; an
accelerated numerical approximation cannot bypass an invalid x topology or y
domain. Report actual output atom, sample x and segment/anchor/component index
where applicable. Global X validation is completed before any values success;
do not label a late local failure as a domain-wide revocation of earlier output.
Axis-only success is independent. Ordinary execute remains fail-fast, while
supported atom APIs retain independent outcomes under the host's contracts.

## Acceptance contract

Use an independent exact-rational/interval Bezier oracle, independent of the
production root solver. Form the same RN64 absolute controls and sampled q,
then establish the unique mathematical inverse and correctly rounded y.
Known dyadic roots and algebraic cases complement hard-to-round tests; an
oracle using only a fixed double-precision t approximation is insufficient.
For strict, compare final bits. For accelerated Float32, compare ordered finite
FP32 distance with maximum four; for Float64, compare the direct error against
the shared FP32-scaled bound. Apply strict outside its admitted range. Exact anchor and
clamp cases still compare bits. No global absolute epsilon replaces these tests.

Rounding enclosures must resolve reference results before claiming oracle
success. Solver residuals, dense visual plots and a second call to production
code are not independent numerical acceptance. Measured test coverage must not
be labeled a CertifiedBound QualityReport without the required kernel evidence.

| ID | Required fixture / observable result |
| --- | --- |
| B01 | Quadratic anchors `[(0,0),(1,0)]`, handle offset `(0,1)`: q=1/4 has t=1/2 and y=1/2; endpoints return zero |
| B02 | Cubic anchors `[(0,0),(1,1)]`, outgoing offset `(0,1/4)`, incoming offset `(-1,-1/4)`: q=1/8 yields y=1/2; direct t=q incorrectly gives 107/1024 |
| B03 | Degree-2 and degree-3 representations of y=x, increasing/decreasing samples: start=1,end=0,N=3 gives values `[1,0.5,0]`, axis `[1,0,-0.5]` |
| B04 | N=1 ignores a failing end producer; axis is `[start,start,0]`; controls are still validated for values, and never read for axis-only |
| B05 | Mixed Float32/64 anchors, handles and endpoint scalars; mutate handles and endpoints in the same compiled plan and check exact profile-specific outputs |
| B06 | Cubic x controls `[0,0.75,0.25,1]` pass despite handle crossing; `[0,2,-1,1]` fail; zero endpoint/interior derivative without folding passes |
| B07 | Two quadratic segments with anchors `[(0,0),(0.5,1),(1,0)]` and offsets `[(0.25,0.5),(0.25,-0.5)]`: sharp C0 join is accepted; exact q=0.5 returns 1 without y-handle reads |
| B08 | Quadratic anchors `[(0,0),(1,0)]`, handle offset `(0.5,4)`: q=0.5 gives 2; negating handle y gives -2, without clipping |
| B09 | Below/above-domain reject reports InvalidDomain before y reads; clamp reads only the selected endpoint y and preserves requested axis |
| B10 | Invalid remote x topology fails a local values request; remote unneeded y NaN does not; selected segment interior y NaN fails; exact anchor ignores invalid y handles |
| B11 | All count/K/degree/shape boundaries and one-past-limit cases; missing parameters, wrong arity/dtype, nonfinite reconstructed controls, unaligned/strided views |
| B12 | Nonzero/disjoint ROI, reverse order, SIMD tails and joint on/off reproduce requested outputs without reading unrequested y components |
| B13 | Every x mutation invalidates old values witnesses; remote y mutation preserves unrelated observations; axis ignores all controls; count=1 ignores end changes |
| B14 | Strict rounding near flat x tangencies, steep y, exact zeros and halfway output cases; acceleration must bound y error or report strict fallback |
| B15 | Small-ROI versus full-output budgets, forced topology/root capacity/work/stage failures, cancellation and recovery, cache-off and release of unpublished objects |
| B16 | Multiple consumers, output Values surviving context destruction, final-owner release and no mutable input/registry leakage across concurrent runs |
| B17 | Strict cross-platform result bits; accelerated <=4 ULP on each declared CPU profile; incompatible-platform rejection and actual fallback diagnostics |

### Maintained public workflow

The public fixture is implemented through `sample_bezier_function_node` in
`photospider/numeric/bezier.hpp`:

```text
anchors: Float64[2,2] = [[0,0], [1,1]]
handles: Float64[1,2,2] = [[[0,0.25], [-1,-0.25]]]
start: Float64[1] = [0]
end: Float64[1] = [1]

curve.sample_bezier_function_strict
  ordered inputs = [anchors, handles, start, end]
  degree = 3, count = 9, dtype = "float64", out_of_domain = "reject"

request values indices {0,1,8} -> {0:0, 1:0.5, 8:1}
request axis -> [0,1,0.125]
```

It runs through WorkflowDocument input declarations/bindings, Compiler and
ExecutionContext and checks read coverage, axis bytes, failures and binding
changes. The maintained command and oracle are in the numeric workflow README;
this specification does not replace the public workflow with an internal solver
call.

Benchmark both degrees and all three implementations on K=2,64,4096 and the
supported maximum, using N=256,65536,1048576 and sparse ROIs. Report hardware,
OS/compiler/ISA, solver/profile version, topology sharing/index behavior, input
read counts, actual root/precision work, fallback counts, managed peak and timing
distribution. Include nearly stationary x and y-cancellation fixtures. A numeric
pass alone does not establish accelerated speedup; measure it against strict.

## Current implementation and verification

The maintained implementation registers the three profile keys in
`plugins/ops/01-numeric/bezier_function.cpp`. It performs exact x topology and
monotonicity validation, RN64 control reconstruction, an inverse solver with
up to 8192 fractional bits of dyadic refinement, exact polynomial/Horner
evaluation and direct output rounding. The polynomial workspace is 40,960 bits with a 96-slot arena;
the static live-slot bound is at most 44. All profiles use the same exact path,
with no numerical fallback and 0 ULP against the independent result. The host
reports actual `strict_math_calls` for Bx sign attempts only; topology, interval
and integer polynomial GCD work are not counted as those calls.

Dynamic output associations scale with the requested value count. The default
`maximum_boxes` is 65,536, and each `NeedBatch` reserves metadata of
`4096 + 16384*M` bytes, so dense 65,536 or 1,048,576 value requests are not
promised under default limits. Sparse ROI requests remain the supported way to
exercise large logical outputs. The native benchmark matrix records those
`ResourceExhausted` outcomes separately from successful timing results. Explicit
execution budgets are required for reproducible manual runs.

Native Apple M5 Clang 21 strict/Apple and Ubuntu WSL i9-12900 Clang 18
strict/x86 runs passed five public manual groups and 386 independent
Fraction/de Casteljau/GCD-Sturm cases per profile. Installed 0.15 strict and
Apple consumers passed. The manual target is EXCLUDE_FROM_ALL and has no CTest
or integration registration. The latest root-call counter also passed the
native/WSL manual checks. Native measurements and their resource limits are recorded below through the linked
implementation notes. WSL supplies correctness evidence only. See
[the numeric workflow README](../../../../examples/numeric_workflow/README.md)
and [math implementation](../math-implementation.md) for commands and algorithm
notes.

## Existing implementation comparison

At `30478d33`, the registry provides `curve.sample_linear` and
`curve.sample_monotone`, with generic Float32/Float64 `[K,2]` controls, K>=2.
Both interpolate the input points, require strictly increasing control x, use
static increasing `domain_min/domain_max`, and return one Whole `[count]`
generic array. `curve.sample_monotone` computes PCHIP slopes, not Bezier handles.
These operations do not implement the selected CRV-02 input model.

Sources inspected:

- [Linear registration](../../../../plugins/ops/01-numeric/curve_sample_linear.cpp).
- [PCHIP registration](../../../../plugins/ops/01-numeric/curve_sample_monotone.cpp).
- [Current shared curve implementation](../../../../plugins/ops/01-numeric/curve_common.hpp).
- [Current integration tests](../../../../tests/integration/test_basic_operations.cpp).
- [Current implementation contract](../../../kernel-architecture/Basic-Operations.md).

## Related specifications

- [CRV-02 category entry](../curves.md).
- [NUM-01 generator contract](NUM-01_sample_expression.md): inherited sampling,
  dtype, dynamic-axis and ownership conventions; CRV-02 defines its own
  inverse-plus-evaluation strict/accelerated numerical contract above.
- [Common execution contracts](../../00-foundation/contracts.md).
- [Per-operator template](../../00-foundation/spec-template.md).
