---
spec_schema_version: 1
id: NUM-10A
parent_id: NUM-10
function: concatenate
proposed_operation_keys:
  - array.concatenate_strict
  - array.concatenate_accelerated_apple_silicon
  - array.concatenate_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-10A: concatenate

Concatenate an ordered sequence of 2..256 single-tensor Results along required static Int64 `axis`. Tensor schema/member names may vary. Complete sample shapes include batch axes, have rank 1..8, positive extents and at most 2^40 elements. Inputs share UInt8/Int64/Float32/Float64 dtype, rank and all non-axis extents. Output axis length is the sum of input lengths, checked against the same output count limit. Output `values` uses `photospider.tensor`/`samples` with ordinary axes.

For input lengths L[k] and prefixes P[k], output coordinate o selects the unique k with `P[k] <= o[axis] < P[k]+L[k]`; its source coordinate is o with axis replaced by `o[axis]-P[k]`. All element bits are copied, including signaling NaNs. No casts or broadcasting occur. All three CPU profiles have identical bitwise semantics.

Each selected output tile lowers to one fixed CPU task. Its reads are the input tiles intersecting the output/slab intersections, using each input's own grid and local coordinates. Unselected slabs have no payload or producer dependency, although all static schemas are checked. Empty selection emits no task. Output tiles own separate dense allocations; the old layout policy is removed from direct nodes and the helper. The helper accepts profile immediately after axis.

Prepare checks the complete invocation signature. Runtime uses bounded coordinates and a 32-byte copy buffer with no payload scratch. Negative/zero input strides and unaligned offsets remain supported. Cancellation/resource failures prevent successful call publication; successful output storage outlives context destruction. No view fallback or content cache is used.

Temporary acceptance compares an independent ordered coordinate mapping, including nonleading axes, partial tiles crossing input boundaries, repeated references, 65/256 ports, all four dtypes, stride variants, empty selection, artifact roundtrips and concurrent Runs. A failing producer for an unselected slab must remain elided. See [fixed indexing execution](../indexing-execution.md). This fixed execution contract overrides the [NUM baseline](NUM_common_contract.md); specification status remains Proposed.
