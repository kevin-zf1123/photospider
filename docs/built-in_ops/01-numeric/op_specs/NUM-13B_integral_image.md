---
spec_schema_version: 1
id: NUM-13B
parent_id: NUM-13
function: integral_image
proposed_operation_keys:
  - numeric.integral_image_strict
  - numeric.integral_image_accelerated_apple_silicon
  - numeric.integral_image_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-13B: integral_image

The three `numeric.integral_image_<profile>` keys accept one Result tensor member under any member key. Output port `values` uses `photospider.tensor` with member `samples`. Required static String `axes` names exactly two distinct nonnegative input axes as comma-separated canonical integers. Normalize the pair to increasing order. Input rank is 2..8 with positive extents; both input/output logical counts are at most 2^40. The output takes the complete input `sample_shape()`, including batch axes, as ordinary axes and adds one to each selected extent. Other coordinates identify independent planes. No image/color semantics or physical units are inferred.

For selected axes a,b and output coordinate o, sum source coordinates s[a] in [0,o[a]) and s[b] in [0,o[b]), with other coordinates equal to o. Each result is an exact rectangle sum followed by one conversion. Required static String `dtype` uses [prefix_sum's same-domain rules](NUM-13A_prefix_sum.md); the public helper defaults to Int64 for integer inputs and Float64 for floating inputs.

## Numerical semantics and source coverage

Inherit [prefix_sum's arithmetic and unchecked integer precondition](NUM-13A_prefix_sum.md). A rectangle with either boundary zero yields integer zero or floating +0. Source NaN priority follows original logical row-major coordinates, independent of physical strides and tile layout. Preserve the sum contract's payload conversion, mixed-infinity and zero-sign rules. Rounded output and generated NaN never become source carry. Floating overflow remains an output infinity while later rectangles retain exact cancellation.

Requested tiles with equal non-scan-axis origins and extents form one CPU task. Its source is the rectangle from zero to the largest positive boundary required on each selected axis, restricted to the group's non-scan region. Every intersecting input tile is declared. Multiple selected rectangles may enlarge the bounding source rectangle. A zero-only tile, for which one selected axis contains only boundary zero, contributes no source extent. A group consisting only of zero rows/columns has no payload reads or arithmetic scratch. Empty selection emits no task.

Only declared output tiles are converted and written. Integer representability is required for every declared write. `ExecutionPlan::select_outputs` keeps complete compiled tasks, including all reads, writes and scratch of any retained multi-write task; the integer precondition covers all such writes. Fresh compilation can narrow the bounding rectangle. Final Result coverage follows the selected outputs.

## Algorithm, resources and errors

For each plane, process the lower-numbered selected axis as rows and the higher-numbered axis as columns. Merge the current row's exact prefix with the saved exact rectangle from previous rows at that column. The terms are disjoint. Save magnitude/sign, first source NaN, both infinity flags and all-negative-zero state before final conversion. Previous rows precede the current row for NaN priority; reset every column per plane.

Each source sample is read once per group. Scratch is `sizeof(ScanState) + W * sizeof(SumCarry)`, with W the required source prefix width on the higher-numbered selected axis. Zero-only groups use no scratch. Mutable state and dense output tiles are preallocated per invocation; prepared metadata and slot maps are immutable. Input affine views support negative and zero strides. No rounded-output recurrence, full intermediate output or runtime payload allocation is required.

Static schema, shape caps, axes and dtype errors fail before execution. Preparation rejects incorrect kernel contracts, missing/extra source reads, incompatible writes and scratch sizes. Cancellation checks cover output initialization, column resets, source reads and exact arithmetic. Operational failures prevent successful call retirement. Per-value NumericDiagnostics and sample work-limit counters are unavailable. Successful output owners outlive the execution context.

## Acceptance and implementation

For input `[[1,2],[3,4]]` and axes `"0,1"`, expect `[[0,0,0],[0,1,3],[0,4,10]]`. Use independent exact rectangle enumeration as the oracle. Cover nonadjacent axes, independent planes, NaN priority, negative/zero strides, zero-only unions, sparse bounding rectangles, concurrent Runs, empty selections, artifact roundtrips and cancellation. Distinguish fresh compilation from task-preserving selection when checking integer representability and upstream failure scope.

Four-corner subtraction of rounded floating outputs has its own rounding/cancellation error; each integral value is guaranteed independently. See [fixed scan execution](../scans-execution.md) for the six scan profile definitions and public helpers. This family overrides the [NUM baseline](NUM_common_contract.md) where fixed execution or unchecked numerical preconditions differ. Proposed specification status is unchanged.
