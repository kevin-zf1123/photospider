---
spec_schema_version: 1
id: NUM-03B
parent_id: NUM-03
function: broadcast
operation_family: numeric.broadcast
proposed_operation_keys:
  - numeric.broadcast_strict
  - numeric.broadcast_accelerated_apple_silicon
  - numeric.broadcast_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-03B: broadcast

Broadcast expands a single-tensor Result through an explicit injective mapping from every complete source axis to a distinct target axis. The complete source `sample_shape()` includes batch axes and has rank 1..8. Required static String `shape` lists positive target extents with rank 1..8 and product at most 2^40. Required static String `axis_map` lists one canonical nonnegative target-axis index per source axis. Output rank cannot be smaller than source rank. Each source extent must be one or equal its mapped target extent.

For source shape I, target shape O, map a and global output coordinate o:

```text
s[j] = 0        when I[j] == 1
s[j] = o[a[j]]  otherwise
values[o] = bitcopy(input[s])
```

Unmapped target axes replicate source values. Permutation and singleton expansion may occur together. No periodic tiling, axis merging, implicit alignment or numeric conversion is provided. All seven numeric dtypes preserve their raw bits, including signaling NaNs and integer extrema. Output `values` uses `photospider.tensor`/`samples` with ordinary target axes and unchanged dtype. The layout parameter and `ArrayLayout` enum are removed; the helper accepts profile after axis_map.

## Fixed source mapping and execution

Each selected output tile has one CPU task and a separate dense allocation. Its source rectangle has `[0,1)` on singleton source axes and the output tile's mapped interval on other axes. The task declares all input tiles intersecting that rectangle, using the source's batch/atomic grid. This tile granularity may admit neighboring samples. Empty selection creates no task or payload reads.

Prepare checks metadata, derives the source rectangle from the canonical output tile and validates the actual write plus all required reads. Runtime uses fixed affine input views, bounded stack coordinates and a 32-byte byte-copy buffer, with no scratch payload or runtime allocation. Negative/zero source strides and unaligned offsets are supported. Cancellation is sampled at entry and every 64 copied samples. Output owners are independent of source owners and outlive context destruction; operational failures prevent successful call publication.

Static validation rejects malformed shapes/maps, rank reduction, duplicate/out-of-range mapping axes and incompatible mapped extents. Unsupported profiles fail before invocation. No per-sample diagnostics, work counters, owner-dependent view path or completed-result cache is used.

## Acceptance

For source `[10,20,30]`, target `[2,3,4]` and map `[1]`, output `[i,j,k]` equals source[j]. For source `[2,3]`, target `[3,4,2]` and map `[2,0]`, output `[j,k,i]` equals source `[i,j]`. Temporary checks compare an independent coordinate map and exact source tile union, covering seven dtypes, batch/atomic grids, singleton expansion, rank eight, sparse/empty requests, source layouts, artifacts and concurrent Runs. Include a sparse huge target whose source remains small. See [fixed array execution](../arrays-execution.md). This execution contract overrides the [NUM baseline](NUM_common_contract.md); specification status remains Proposed.
