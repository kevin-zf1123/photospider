---
spec_schema_version: 1
id: PNT-05A
parent_id: PNT-05
function: local_inpaint_navier_stokes
operation_keys:
  - image.local_inpaint_navier_stokes_openCV
  - image.local_inpaint_navier_stokes_native_apple_silicon
category: 09-composite
kind: primitive
status: Specified
implementation_scope: builtin_operations
implementation_status: implemented
verification_status: result_native_and_opencv_finite_regressions_and_oracle
result_operation_abi: 2
operation_api: Result
workflow_schema: 4
operation_traits: 21
kernel_package: "0.30.0"
backend: CPU
region_rule: Whole
numeric_profile: opencv_4_12_ns_f32_planar_v1
---

# PNT-05A: local_inpaint_navier_stokes

## Implementation backends

The two public operations share the Result input/output, mask, Whole, Float32
reference, error and numerical acceptance contracts below. Both expose named
output port `image` and required static Int64 parameter `radius`.

| Public operation key | Implementation boundary |
| --- | --- |
| `image.local_inpaint_navier_stokes_openCV` | Optional OpenCV 4.12.0 `cv::inpaint(..., INPAINT_NS)` adapter on three Float32 planes |
| `image.local_inpaint_navier_stokes_native_apple_silicon` | Native CPU implementation; no OpenCV headers, symbols, calls or linkage in this path or a native-only consumer |

Both expose only named output `image` and required Int64 radius. The unsuffixed
operation is the semantic family, not a third registration or alias.
The native key identifies the in-tree CPU implementation; it does not select a
GPU backend. A licensed
standalone C++ port of the published algorithm is allowed; the OpenCV library
itself is prohibited in the native variant. A native-only build/consumer must
be possible with the OpenCV adapter disabled.

The OpenCV adapter is explicitly permitted to use OpenCV's internal allocations
and non-interruptible call. Account host-owned output/packing normally; estimate
and report external allocations separately. Check cancellation before/after
each channel call, never claim the internal allocations obey a hard host budget
or the native frontier cancellation bound. No process-global allocator changes.
This capability limitation applies to T15/T16 acceptance. The native variant
retains the full host-allocation and cancellation requirements. Expose this distinction in the completion matrix and benchmark.

The adapter and independent oracle must use the same OpenCV 4.12.0 build. Compile
core/imgproc/photo for arm64 with
`-fno-fast-math -frounding-math -ffp-contract=off`. Binary version equality alone
does not establish this numerical profile: a build allowing FP contraction can
exceed the tolerance in section 9. Process channels sequentially in R, G, B
order; native optimizations must retain deterministic reference tolerance.

This English specification defines the implementation and acceptance contract.
Actual coverage and limitations are recorded in the
[implementation documentation](../inpaint-ns-implementation.md).
The spelling is `navier_stokes`; no `navier_stoke` alias is provided.

## 1. Purpose and scope

Fill an explicit binary hole mask using known neighboring image colors, while
preserving every unmasked sample. Intended uses are scratches, small holes and
local defects. The profile fixes the OpenCV 4.12.0 single-channel Float32
`INPAINT_NS` behavior, applied independently in R, G, B order. This is a named
Navier–Stokes-inspired discrete algorithm, not an arbitrary PDE solver.

Telea, Poisson, PatchMatch, generated content, automatic defect detection,
allowed-source masks, transparent reconstruction, time coherence and commercial
pixel compatibility are outside scope. No implicit algorithm substitution.

## 2. Identity, prerequisites and evidence

The public keys are listed above; each has only the named output port `image`.
PNT-05 is the existing family; PNT-05A is this concrete profile.
The operations use Result operation ABI 2 with WorkflowDocument 4,
OperationTraits 22, and Photospider package 0.30.0. There is no GPU backend.
Accepted public Result and runtime contracts take precedence over research pages.

Normative discrete reference: OpenCV tag `4.12.0`,
[`modules/photo/src/inpaint.cpp`](https://github.com/opencv/opencv/blob/4.12.0/modules/photo/src/inpaint.cpp).
Retain the source license and attribution for any port. An independently called
OpenCV 4.12.0 installation with the floating-point build flags above is the test
oracle. Native computation buffers are host-controlled; the adapter has the
allocation and cancellation boundaries declared above.

Relevant current contracts: [Plugin ABI](../../../kernel-architecture/Plugin-ABI.md),
[named outputs](../../../kernel-architecture/Multi-Output-Operations.md),
[Region](../../../kernel-architecture/Region-Semantics.md), and
[dependency data](../../../kernel-architecture/Dependency-Data.md).

## 3. Inputs and layout

| Order / name | dtype / shape | Semantics | Sample domain |
| --- | --- | --- | --- |
| 0 / image | Result with one `photospider.image` v1 tensor member `pixels`; Float32 `[H,W,4]` and batch axes `[N,L]` | ordered RGBA; linear sRGB primaries, D65; premultiplied alpha; scene or display reference preserved | finite signed/HDR RGB; alpha exactly 1 |
| 1 / hole_mask | Result with one `photospider.image` v1 tensor member `pixels`; Float32 `[H,W]` and matching batch axes `[N,L]` | canonical typed coverage; non-color; same logical grid | exactly numeric 0 or 1; both signed zeros are known |

Both Results have exactly one tensor and no fields; both use sample batch axes
`[N,L]`. The image and mask batch extents and H/W must match exactly. Their
complete sample shapes are `[N,L,H,W,4]` and `[N,L,H,W]`. No broadcasting,
quantization, implicit conversion, thresholding or unassociation. Require
`3 <= H,W <= 32768` and `H*W <= 2^31-1`. Static preparation checks typed
schemas, facets, semantic profile and shape before continuation start. Runtime
validation checks all tensor samples. Padded extents, strides, byte arithmetic
and actual allocation must also fit checked arithmetic and the execution root.

Support authorized computed Result views with padding, byte offset, nonzero storage
origin, positive/negative/zero strides and shared owners. Read through the
ResultTensorInput window using checked origin-relative logical addressing. Direct
bindings retain the host's Result binding validation. No input writes,
borrowed-pointer retention, in-place publication or escaping mutable aliases.

Whole execution requests the full logical domain of both inputs with Data,
Validation, and Descriptor roles before numerical work. Validate every RGB
placeholder inside holes, all alpha, and every mask sample across every batch
plane. Mask 0.5 is invalid for this operator even though it is valid coverage.
An all-zero mask does not skip validation.

## 4. Parameters

| Parameter | Exact type | Unit | Range | Required | Constructor suggestion |
| --- | --- | --- | --- | --- | --- |
| radius | Int64 | logical pixel spacing | inclusive `[1,32]` | yes, static | 3, explicitly supplied |

Unknown keys, absence, Float64 3.0, 0, negative values and 33 are rejected. No
hidden defaults or clamping. There is no method, boundary, seed, iterations, dt,
solver_tolerance or opacity parameter. Radius participates in existing canonical
parameter identity and replanning.

## 5. Output inference and invariants

Static preparation validates both Result schemas, the matching `[N,L]` batch
axes and `[H,W]` spatial extents, the exact typed image/coverage facets, and the
radius before execution. It clones input 0's full schema to the output and selects
`CompleteBundle` publication. The output sample shape is `[N,L,H,W,4]` and it
preserves the scene/display reference. Static preparation reads no pixel payload.

For input I, mask M and result J, `M(n,l,y,x)=0` implies bitwise equality
`J(n,l,y,x,c)=I(n,l,y,x,c)` for every channel c. All alpha samples are
bitwise preserved at 1.
All-zero M returns bitwise identity after complete validation; immutable sharing
or copying are both permitted. All-one M fails with OperationFailed. Successful
results are finite and are not clamped to `[0,1]`. Hole placeholders must not
affect the result when known samples and M are unchanged.

## 6. Coordinates, boundaries and exceptional inputs

Indices are y,x,c; x increases rightward, y downward; pixel centers are
`(x+0.5,y+0.5)`. Neighborhoods use integer pixel offsets and the inclusive disk
`dx*dx+dy*dy <= radius*radius`. Only real canvas colors are candidates.

Preserve the pinned source's padded guard state, initial band construction,
excluded exterior band, arrival time initialization `1.0e6f`, gradient indexing
and edge branches. These are normative native boundary rules, not a selectable
clamp/reflect/wrap policy. Holes touching edges and corners are supported.
For every non-full binary mask, return the finite pinned-reference result or
the specified numerical/resource/cancellation failure; do not invent an
unfillable-component rejection. With only one known pixel, the pinned result
may retain zero work values where its frontier never reaches.

Empty or too-small image shapes are rejected. Empty output demand follows the
Result protocol: it publishes an empty Result without requesting payload or
entering numerical work. For nonempty demand, nonfinite RGB, alpha other than 1
and nonbinary masks fail even for all-zero masks.

## 7. Numerical algorithm

1. Check cancellation, descriptors, parameters and checked allocation sizes.
   Scan and validate both inputs completely and count K holes.
2. Handle validated K=0 identity and K=N failure.
3. Allocate output and scratch through the invocation allocator. Copy original
   samples to output so unedited bits and alpha are retained.
4. Convert M to an internal 0/255 UInt8 mask. For R, then G, then B, pack a
   Float32 work plane and set every hole sample to positive zero.
5. Initialize the pinned source's state/time/band. Use row-major initial band
   insertion, minimum arrival time first, insertion order for ties. Consider
   frontier neighbors in up, left, down, right order. Preserve
   `FastMarching_solve` and `icvNSInpaintFMM<float>` single-channel branches.
6. Preserve candidate loops, gradient expressions, mixed Float32/Float64
   evaluation points and constants `1e-20f`, `0.01`, `1e-6f`. Conceptually,
   `u(p)=sum(w(p,q)*u(q))/(1e-20f+sum(w(p,q)))`; this summary does not replace
   the normative source expressions, state predicates or iteration ordering.
7. Reject nonfinite arithmetic/results. Write back only hole RGB, sequentially
   reusing scratch. Check cancellation before publishing one immutable result.

OpenCV supports Float32 single-channel inpainting; its native three-channel
input path is UInt8. This profile must not quantize Float32 color to that path.
Direct unrestricted `cv::inpaint` calls do not satisfy host memory/cancellation
requirements. A private source port is permitted with retained license, bounded
host allocations and inserted cancellation observations. Do not change global
OpenCV allocation/thread settings or create a private thread pool.

## 8. Publication, dependency and invalidation

Route through named port `image`, publish one immutable `CompleteBundle` Result
after completion, and release unpublished buffers on failure. Descriptor support
for image and mask is recorded independently. The first Need requests both full
sample domains with Data, Validation and Descriptor roles; the output relation
conservatively records Data and Validation support from both complete inputs.
There is no regional algorithm or transitive halo because newly filled values
propagate across the plane. A downstream ROI may collect a region from the
complete output. A change to any input sample dirties the full output; relevant
schema, facet, resource or parameter changes invalidate or replan.

The image and mask tensors retain their matching `[N,L]` batch axes. Each
batch plane solves independently; a nonempty request still validates all planes
before processing any plane. Empty output demand creates an empty Result without
payload Need or numerical execution. Use existing operation and schema identity;
do not add a cache key that omits image resources or semantic metadata.

## 9. Precision and determinism

Shape, facets, unmasked samples and all alpha are exact; unmasked negative zero
must survive. Same build, snapshot and CPU configuration must repeat bitwise.
For each hole RGB sample against the pinned oracle:
`abs(actual-reference) <= 1e-6 + 1e-5*abs(reference)`.
NaN/Inf cannot pass comparison. This profile defines the acceptance threshold;
it is not an OpenCV guarantee. Report failures without relaxing it.

Use round-to-nearest and gradual underflow for computation and restore caller
floating-point state. No fast-math or FMA contraction altering reference order.
Report compiler/architecture differences; no untested cross-platform bit claim.
Algorithm/profile/build/backend identity, parameters, snapshots and semantic
metadata must enter the existing operation/runtime identity machinery.

## 10. Ownership, execution and resources

CPU required; GPU unsupported, following existing host backend policy. Operations
are reentrant with invocation-local state, immutable inputs and no implicit I/O,
clock, randomness or mutable cross-call state. No partial successful output.

Let `N=H*W` samples per batch plane, `B=N_batch*N_layer`,
`K=count(M=1)`, `P=(H+2)*(W+2)`, and `r=radius`. A sequential planar
reference design has `O(N+3*N*log(max(N,2))+3*K*(2*r+1)^2)` work and O(N) scratch.
Checked arithmetic and actual allocations determine admission; estimates are not
RSS bounds.

| Owner / buffer, per active plane | Bytes |
| --- | ---: |
| Result-owned output across batches | `16BN` |
| phase-allocator native scratch: mask, work plane, state, times, heap | `21N+5P` |
| phase-allocator OpenCV arrays: mask, source plane, target plane | `9N` total |
| operation workspace admission | `2 * input_bytes + 65536` |

The native scratch terms are mask `N`, work plane `4N`, padded state `P`,
padded arrival time `4P`, and 16-byte heap entries occupying `16N`. For 3x3
dimensions, `21N+5P < 35N`. The OpenCV adapter instead owns a mask of `N`
bytes and separate source and target planes of `4N` bytes each. Only one
plane's scratch is live at a time. Result output backing and retained hole-index
metadata use Root accounting; the phase allocator owns per-plane scratch.
OpenCV's internal matrices and queue are external allocations; `7P+32N+64KiB`
is an estimate for the pinned adapter and libc++ vector growth, not a hard bound.
Result input ancestry, metadata, and collectors retain their normal Root owners.

The callback prepays validation work based on Result window reads and polls
cancellation every 512 validation samples. Output copying polls every 1024
samples; packing/copy-back polls every 4096; native initialization polls every
1024 positions. Frontier work checks every 64 pops or 4096 candidate visits,
whichever comes first. Allocations, channel transitions and publication also
check cancellation. OpenCV cannot be interrupted inside a channel; its adapter
checks immediately before and after each call. Preserve Cancelled/Stale
priorities. Failed allocations return ResourceExhausted without reduced radius,
quantization or partial repair.

## 11. Acceptance matrix

Implementation documentation maps these IDs to actual tests/results and marks
absent evidence. A test name alone is not proof that every clause is covered.

| ID | Fixture / procedure | Required assertion |
| --- | --- | --- |
| T01 | zero mask with finite samples and signed-zero coverage | bit identity after complete validation |
| T02 | 5x5 constant `(0.25,0.5,0.75,1)`, center hole, r=1 | constant hole within tolerance, exact outside/alpha |
| T03 | change finite placeholders only | bit-identical output |
| T04 | gradients, asymmetric lines, checkerboards, scratches, holes | per-sample pinned Float32 oracle |
| T05 | four edges/corners, disjoint holes, one known pixel | pinned native result, no out-of-bounds |
| T06 | full mask | OperationFailed, no successful result |
| T07 | 0.5/out-of-range/NaN mask; nonopaque alpha; NaN/Inf RGB | host Need Validation errors retain priority; operator-specific invalid-value checks return OperationFailed for direct or generated bindings |
| T08 | radius 1/32, 0/33, absent/wrong type/unknown parameter | exact closed schema |
| T09 | size 3, 1/2, 32768, 32769 and UINT64_MAX extents | bounds and resource rejection without huge real allocations |
| T10 | nonzero ROI and different tile geometry | Whole crop equality; distant invalid input still fails global validation |
| T11 | padded/offset/origin/negative/zero-stride Result input windows | packed equivalence and immutable input Results |
| T12 | repetitions, changed caller rounding mode | deterministic output, restored floating state |
| T13 | signed/HDR constants and extreme values | no clamp; nonfinite computation fails |
| T14 | change known pixels, mask, radius, metadata and distant NaN | no stale successful reuse |
| T15 | allocation failure injection; minimum budget minus one | ResourceExhausted, released owners, later success |
| T16 | initialization/frontier/prepublication cancellation, host Stale | no partial success, released owners, host priorities |
| T17 | public registry -> compile -> bind -> execute -> named image | actual example and installed consumer, facets and numbers |
| T18 | independent concurrent invocations | no cross-call state or resource interference |

Oracle wrappers must independently use pinned OpenCV 4.12.0, zero hole work
values, process Float32 planes and restore unmasked samples. They must not call
the production helper. A source-port oracle detects adaptation mistakes but
does not independently prove the original algorithm; retain analytic T01–T03.

## 12. Public workflow and performance protocol

Required workflow: two immutable typed Result inputs (opaque linear RGBA and
binary coverage with matching `[N,L]` batch axes) -> either public operation
above with `radius=3` -> named `image` output. The public C++ example constructs,
binds and executes those Results; build/run commands are in the implementation
documentation.

Compare correctness before interpreting timings. Compared implementations must use
the same fixtures, host/compiler/options, CPU backend, thread count, input
layouts and output collection. Run timed experiments serially after competing
builds stop. Separate compilation time from operator execution.

Performance coverage: 1080p/4K, r=1/3/8/32, scratches, deterministic 1% holes,
25% block holes and edge holes. Warm up 5 times, sample 30 times for reported
p50/p95. Use nearest-rank percentile and steady_clock. Explicitly record any
smaller exploratory subset or time limit; never extrapolate unmeasured cells.
Report cold/warm scope, cache policy, materialization/compile inclusion, actual
controlled-memory peak, RSS definition and cancellation latency when measured.
There is no mandatory speed target. Invalid implementations receive no claim
of a semantically valid speed advantage.

## 13. Error model

| Condition / stage | Existing category | Behavior |
| --- | --- | --- |
| missing/unknown/type/range parameter, compilation/direct validation | InvalidArgument | no algorithm entry |
| dtype/shape/semantic mismatch in static Result preparation | TypeMismatch; existing direct-binding errors preserved | no conversion |
| invalid binding/layout/query | existing host error and priority | do not disguise as numerical failure |
| invalid typed value detected by host Need Validation | InvalidArgument | preserve host validation priority |
| nonbinary coverage, nonopaque alpha, nonfinite RGB or computation rejected by operator checks | OperationFailed | applies to direct and generated bindings; no threshold or clamp |
| full mask in any batch plane | OperationFailed | no successful result |
| allocation/budget/representable byte size exhausted | ResourceExhausted | release all unpublished state |
| cancellation / no longer current | Cancelled / host Stale | preserve host priority |
| GPU requested | BackendUnavailable; no GPU backend is registered | no alternative algorithm or CPU fallback |
| unknown callback exception or invalid publication | OperationFailed, retaining allocation error category | no escaping exception/partial success |

Collected execution has no partially successful ExecutionResult. Already delivered
stream fragments cannot be rolled back; one callback's final publication is not
a transaction guarantee for an entire stream. Subjective texture quality does
not authorize retries with another algorithm.

## 14. Completion and version policy

Completion requires registration/inference, licensed host-controlled CPU code,
traceable acceptance evidence and a public workflow/installed consumer.
Record partial coverage honestly. Changes to numerical order,
algorithm, binary policy, color/alpha, borders, validation/dependency extent,
determinism or failure semantics require reviewed semantic identity decisions.
Explanation-only revisions do not automatically require an ABI change.

## 15. Sources

The [OpenCV 4.12 inpaint API](https://docs.opencv.org/4.12.0/d7/d8b/group__photo__inpaint.html)
and tagged [OpenCV source](https://github.com/opencv/opencv/blob/4.12.0/modules/photo/src/inpaint.cpp)
define the numerical reference. The port license is installed at
`share/licenses/Photospider/inpaint_ns_license.txt`. See the
[keying and paint catalog](../keying-paint.md) and
[Result implementation](../inpaint-ns-implementation.md).
