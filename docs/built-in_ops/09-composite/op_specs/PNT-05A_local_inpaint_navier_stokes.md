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
spec_revision: 0.4.0
implementation_scope: builtin_operations
implementation_status: see_implementation_documentation
verification_status: see_acceptance_evidence
repository_commit: 66b16339ad2e18a27f22e9e291103b6939f67f1c
operation_abi: 9
backend: CPU
region_rule: Whole
numeric_profile: opencv_4_12_ns_f32_planar_v1
reference_source_blob: 2f2f368fa13da0bc1426b71862205048c6ea0f94
---

# PNT-05A: local_inpaint_navier_stokes

## Implementation backends

The two public operations share the input/output, mask, Whole, Float32 reference,
error and numerical acceptance contracts below.

| Public operation key | Implementation boundary |
| --- | --- |
| `image.local_inpaint_navier_stokes_openCV` | OpenCV 4.12.0 `cv::inpaint(..., INPAINT_NS)` on three Float32 planes, with the specified validation, hole zeroing and copy-back |
| `image.local_inpaint_navier_stokes_native_apple_silicon` | native Apple Silicon implementation in C++/C/ASM/Metal; no OpenCV headers, symbols, calls or linkage in the native implementation or a native-only consumer |

Both expose only named output `image` and required Int64 radius. The unsuffixed
operation is the semantic family, not a third registration or alias.
Native refers to execution on Apple Silicon; Metal or ASM is optional. A licensed
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
Use existing operation ABI/traits 9 and the current registry, compiler, allocator
and execution APIs. No shared ABI extension is required by this specification.
Accepted public ABI and runtime contracts take precedence over research pages.

Normative discrete reference: OpenCV tag `4.12.0`,
[`modules/photo/src/inpaint.cpp`](https://github.com/opencv/opencv/blob/4.12.0/modules/photo/src/inpaint.cpp),
blob `2f2f368fa13da0bc1426b71862205048c6ea0f94`. Retain the source license and
attribution for any port. An independently called pinned OpenCV installation
is the test oracle. Native computation buffers are host-controlled; the adapter has the allocation and
cancellation boundaries declared above.

Relevant current contracts: [Plugin ABI](../../../kernel-architecture/Plugin-ABI.md),
[named outputs](../../../kernel-architecture/Multi-Output-Operations.md),
[Region](../../../kernel-architecture/Region-Semantics.md), and
[dependency data](../../../kernel-architecture/Dependency-Data.md).

## 3. Inputs and layout

| Order / name | dtype / shape | Semantics | Sample domain |
| --- | --- | --- | --- |
| 0 / image | Float32 `[H,W,4]` | typed Image/image-v2; ordered RGBA; linear sRGB primaries, D65; premultiplied alpha; scene or display reference preserved | finite signed/HDR RGB; alpha exactly 1 |
| 1 / hole_mask | Float32 `[H,W]` | canonical typed coverage; non-color; same logical grid | exactly numeric 0 or 1; both signed zeros are known |

Both inputs are required. No broadcasting, quantization, implicit conversion,
thresholding or unassociation. `3 <= H,W <= 32768`, `H*W <= 2^31-1`.
Check padded extents, strides and byte arithmetic separately from these shape
bounds; actual materialization must also fit the host budget.

Support every host-valid computed view: padding, byte offset, nonzero storage
origin, positive/negative/zero strides and shared owners. Read via checked
origin-relative logical addressing. Direct binding keeps its existing stricter
host rules; exotic views can be produced by a fixture operation. No input writes,
borrowed-pointer retention, in-place publication or escaping mutable aliases.

Validate the full logical domain of both inputs, including RGB placeholders
inside holes and samples outside a requested ROI. Mask 0.5 is invalid for this
operator even though it is valid coverage. All-zero masks do not skip validation.

## 4. Parameters

| Parameter | Exact type | Unit | Range | Required | Constructor suggestion |
| --- | --- | --- | --- | --- | --- |
| radius | Int64 | logical pixel spacing | inclusive `[1,32]` | yes, static | 3, explicitly supplied |

Unknown keys, absence, Float64 3.0, 0, negative values and 33 are rejected. No
hidden defaults or clamping. There is no method, boundary, seed, iterations, dt,
solver_tolerance or opacity parameter. Radius participates in existing canonical
parameter identity and replanning.

## 5. Output inference and invariants

Infer Float32 `[H,W,4]` from input 0 and preserve all input image semantic facets,
including reference. Inference reads descriptors and static parameters only.
The callback validates the profile's exact channel order and image semantics.
The result descriptor/Region must match actual coverage and include all channels.

For input I, mask M and result J, `M(p)=0` implies bitwise equality
`J(p,c)=I(p,c)` for all channels. All alpha samples are bitwise preserved at 1.
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

Empty or too-small image shapes are rejected. Empty/invalid output queries keep
the existing host Region validation; the callback does not invent empty images.
Nonfinite RGB, alpha other than 1 and nonbinary masks fail even for noop masks.

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

Route through named port `image`, publish once after completion, freeze output,
and release all unpublished buffers on failure. No correspondence, STMap or
pointer dictionary is returned.

For each legal nonempty query Q, both numerical and validation demand are
conservatively Whole: `need_image(Q)=All([H,W,4])` and
`need_hole_mask(Q)=All([H,W])`. The runtime can materialize Whole and collect an
ROI. Radius is not a transitive halo because freshly filled values propagate.
Changing any input sample invalidates the full output conservatively; relevant
metadata/parameters invalidate or replan. A distant NaN can change success into
failure. Use existing runtime identities; no custom incomplete cache key.
No promise of cross-run cache hits or precise dependency certificates is made.

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

Let `N=H*W`, `K=count(M=1)`, `P=(H+2)*(W+2)`, `r=radius`. A sequential planar
reference design has `O(N+3*N*log(max(N,2))+3*K*(2*r+1)^2)` work and O(N) scratch.
Checked arithmetic and real allocations determine admission; estimated_bytes
alone cannot account for untracked library allocations.

| Owner / buffer | Conservative proposed bytes |
| --- | ---: |
| dense materialized inputs, if newly allocated | 20N |
| output | 16N |
| input/output work planes, reused per channel | 8N |
| UInt8 mask | N |
| padded state/band/mask/time | 7P |
| bounded heap, entry `{float T,int32 y,int32 x,int32 order}` | 16P |

Assert heap-entry size if using this layout, prove at-most-once insertion and
checked tie-order capacity. Other bounded host layouts are allowed when the
same semantics hold and the actual accounting is reported.
The proposed model is `45N+23P+64 KiB`, excluding collector. At 1080p and 4K it
is approximately 134.7 and 538.2 MiB. A simultaneous full collector adds 16N.
These are estimates, not measured peaks or RSS limits. Count actual capacities,
alignment, extra packing, upstream owners and collectors, deduplicating owners.
Caller-owned inputs are retained bytes, not fresh allocation charges.

Observe cancellation at most every 4096 validation/copy/initialization samples;
in frontier processing, at most every 64 pops or 4096 candidate-loop visits,
whichever comes first. Also check around large allocations, channel transitions
and publication. Preserve host Cancelled/Stale priorities. Failed allocations
return ResourceExhausted without reduced radius, quantization or partial repair.

## 11. Acceptance matrix

Implementation documentation maps these IDs to actual tests/results and marks
absent evidence. A test name alone is not proof that every clause is covered.

| ID | Fixture / procedure | Required assertion |
| --- | --- | --- |
| T01 | zero mask, including signed zero and invalid samples | bit identity; validation before noop |
| T02 | 5x5 constant `(0.25,0.5,0.75,1)`, center hole, r=1 | constant hole within tolerance, exact outside/alpha |
| T03 | change finite placeholders only | bit-identical output |
| T04 | gradients, asymmetric lines, checkerboards, scratches, holes | per-sample pinned Float32 oracle |
| T05 | four edges/corners, disjoint holes, one known pixel | pinned native result, no out-of-bounds |
| T06 | full mask | OperationFailed, no successful result |
| T07 | 0.5/out-of-range/NaN mask; nonopaque alpha; NaN/Inf RGB | correct errors by entry point |
| T08 | radius 1/32, 0/33, absent/wrong type/unknown parameter | exact closed schema |
| T09 | size 3, 1/2, upper and overflowing extents | bounds and resource rejection without huge real allocations |
| T10 | nonzero ROI and different tile geometry | Whole crop equality and global validation |
| T11 | padded/offset/origin/negative/zero-stride computed inputs | packed equivalence and immutable inputs |
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

Required workflow: immutable opaque linear RGBA and binary coverage inputs ->
either public operation above with `radius=3` -> named `image`. Maintain a runnable
public C++ example and record build/run/check commands in implementation
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
| dtype/shape/semantic mismatch, inference | TypeMismatch; existing direct-binding errors preserved | no conversion |
| invalid binding/layout/query | existing host error and priority | do not disguise as numerical failure |
| valid coverage 0.5 or nonopaque samples at callback | OperationFailed | no threshold/clamp |
| invalid bound coverage/image samples | host InvalidArgument when detected first | preserve earlier failure |
| full mask, nonfinite RGB or computation | OperationFailed | no successful result |
| allocation/budget/representable byte size exhausted | ResourceExhausted | release all unpublished state |
| cancellation / no longer current | Cancelled / host Stale | preserve host priority |
| unsupported GPU | BackendUnavailable / existing fallback policy | no algorithm substitution |
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

## 15. Sources and revision record

The user-supplied `op-spec-template.md`, `PNT-05A_local_inpaint_navier_stoke.md`
and `photospider-op-spec-reference.zip`, plus the referenced conversation
“规格书内容与示例”, informed revision 0.2.0. Their process suggestions are
reference material; task authorization comes from the current user request.
The corrected spelling and discrete numerical profile are normative.
Reference material alone does not establish runtime or performance success.

Additional primary reference: [OpenCV 4.12 inpaint API](https://docs.opencv.org/4.12.0/d7/d8b/group__photo__inpaint.html).
Repository family: [keying and paint](../keying-paint.md).
2026-09-13: froze corrected name, scoped profile, exact acceptance conditions,
public workflow requirement and numerical reference from the supplied material.
2026-09-13, revision 0.4.0: present the two backends as built-in operations,
correct the semantic-family/public-key distinction, and move comparison history
to the private development report. Numerical acceptance is unchanged.
