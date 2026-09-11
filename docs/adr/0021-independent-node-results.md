# ADR 0021: Independent node results and Atomic joint execution

- Status: Accepted by the maintainer on 2026-09-12
- Delivery: [#302](https://github.com/kevin-zf1123/photospider/issues/302), leaves #303–#313
- Decision baseline: `ops@ffc5d0e297b9d0ea136975413d3443e23e6fa458`, package 0.8.0 / operation ABI 8

## Context and evidence

The compiler currently accepts only the `value` output port and indexes metadata
and steps by node. Execution records and several caches also assume one result
per node. PerAtomOutcome is declared but rejected at registration; Atomic staged
sessions allow one observation. Existing immutable Values, exact Footprints,
association certificates, independent flight waiters and storage-owner accounting
remain the foundations. These source observations do not establish the new
runtime behavior.

[MLIR operations and values](https://mlir.llvm.org/docs/LangRef/#operations)
provide a reference for distinguishing an operation from its typed results.
[ITU-R BT.709-6](https://www.itu.int/dms_pubrec/itu-r/rec/bt/r-rec-bt.709-6-201506-i!!pdf-e.pdf)
defines the OETF and YCbCr matrix used below. The box filter, edge policy and
floating-point representation below are explicit Photospider choices.

## Results, inference and version axes

One Value has one dtype, nonzero static shape and coordinate domain. A node has
an ordered nonempty set of independently named outputs, represented in C++ by
OperationOutputTraits and SemanticOutput. ValueRef is (node_id, output_index).
Names are unique and resolve to declaration-order indices; singleton operations
explicitly declare `value`. Each output owns its metadata, relevant input ports,
Region, observation and failure contract. Inference is static and checked;
ceil-div, subtraction and fractional-radius extents never invoke numeric code.

Semantic IR is multi-output. Each demanded result lowers to one single-output
PlanStep. Unselected pure outputs create no demand; Empty, Whole and unresolved
sets differ. Existing singleton side effects remain observable; multi-output
operations must be deterministic and side-effect-free. RequestRecord remains a
terminal whole-request result. Consumer legality and EffectiveAtomic refer to
the selected result and its relevant ancestry, never an unrelated sibling.

C++ and C invocations identify the selected result. Projected inputs retain
original port indices; invalid Values are not missing-input placeholders. Records,
subscriptions, dirty routing, Whole results, diagnostics and flights distinguish
results. Content keys include the selected output and contract, metadata, static
parameters and actual input dependencies, but no global node or graph ids.
Unknown certificate rows are not empty dependencies. Fetch unions never replace
per-output Data/Control/Validation/Descriptor associations.

The target package is 0.9.0 and operation ABI/traits 9, with no ABI 8 adapter.
Repository callbacks and installed consumers migrate together. WorkflowDocument
schema 2, provider ABI 1, C++17 and image-v2 whole-pixel semantics remain.
Semantic/physical-plan/plan-cache domains become v9; result-region identity becomes
v5. The unchanged conservative optimizer and result-digest framing retain their
versions. Disposable old cache entries miss; this creates no recovery product.

## Atomic joint execution

An optional joint entrypoint accompanies singleton evaluation. Plan execution
groups are physical choices. Each group selects at most one Atomic observation
per output; members can have different shapes, coordinates and ROIs. RequestRecord
never enters a group. Register all known root and newly discovered input demands
before selecting ready members of the same node, snapshot, backend and joint
contract. Do not wait for future requests or coalesce across Runs; existing
per-observation flights still share work across Runs.

C and C++ joint members independently emit reads, successful values or errors.
Every selected observation has exactly one terminal outcome. Reject missing,
duplicate, unknown and out-of-coverage outcomes. Preserve per-member sticky state,
association rows and tokens. Deduplicate transport only; each success receives
its own validated certificate, flight completion and cache publication. Numeric
failure reaches only actual dependents. The public execute call retains its
original all-request success/error aggregation and deterministic error order.

Use singleton when no compatible joint implementation exists, fewer than two
members are ready or joint admission exceeds budget. After an unattributable
execution failure, retire joint temporary resources and retry each unfinished
member once through singleton, without regrouping. Cancellation, stale work and
structural protocol errors do not retry. A shared group stops only when every
member has no active waiter. One member's cancellation never overrides another's
live request. One admission slot and shared scratch reservation belong to the
group; real storage is charged once per backing owner and retained until its last
view/cache/result reference retires.

## Reusable operations

- `color.rgb_to_ycbcr420`: finite Float32 linear-sRGB RGB without alpha, samples
  in [0,1]. Apply BT.709 OETF then matrix. Y is HW; Cb and Cr are ceil(H/2) by
  ceil(W/2), with signed color differences nominally in [-0.5,0.5]. Use centered
  2x2 box chroma and average valid samples at odd edges. Plane roles, color and
  nominal sampling positions are explicit, separate from numerical support.
- `image.split_horizontal`: full/left/right, with 0 < split_x < W, independent
  ROI origins and immutable shared storage views when feasible.
- `image.convolve_channels`: HW r/g/b, independent Float32 HW kernels and integer
  anchors, odd/even asymmetric support and zero/clamp boundaries. Each output
  reads its own whole kernel and local image neighborhood. Regional
  `field.convolve` permits reuse of exported kernels.
- `image.gaussian_blur_with_kernel`: image plus parameter-derived Float32 kernel.
  Finite Float64 radius and sigma are in [0,64]. R=ceil(radius), shape=(2R+1)^2.
  At integer offsets define a(d)=clamp(radius+1-|d|,0,1), then normalize
  exp(-(x*x+y*y)/(2*sigma*sigma))*a(x)*a(y). Sigma=0 is a center impulse with the
  same shape. Integer radius gives ordinary truncated weights; fractional radius
  continuously introduces boundary weights. Radius=1.25 gives 5x5 and per-axis
  outer factor 0.25. Image evaluation uses the exported coefficients with fixed
  row-major binary64 accumulation. Kernel-only requests read no image samples;
  joint requests share generation. Existing Gaussian semantics do not change.

## Acceptance and delivery

Leaves #303–#313 implement contracts, ABI, compiler, execution identities,
PerAtomOutcome, joint scheduling, plane/420, crops, channel convolutions, Gaussian
outputs and installed workflows in order, with a separate validated commit each.
`examples/multi_output_workflow` must run through installed static/shared public
APIs with independent numeric oracles, source support and callback counts.

Test singleton versus joint, unselected siblings, same-shape cache separation,
different ROI/tile/order, control edits, unknown rows, late dirty increments,
frozen snapshots, mixed hit/flight, waiter cancellation, stale publication,
shared-owner retirement and budget fallback. Radius cases include 0, 0.25, 1,
1.25, 2, 64, adjacent floats around integers and invalid/non-finite parameters.
C fixtures cover protocol sizes/counts/coverage and lifetimes. Follow focused
local validation and existing six CI jobs rather than adding process-text tests.

After all leaves, a fresh independent comprehensive review precedes the PR to
ops, required finding fixes, final-HEAD CI and Codex bot review. Merge commits
preserve leaf history. Explicitly settle Issues after ops merge, synchronize
local ops and delete only the task branch. No main merge, daemon migration,
dynamic output lengths, new Metal algorithms or #206 channel-pruning completion
is implied. Accepted decisions are not claims of implemented or delivered behavior.
