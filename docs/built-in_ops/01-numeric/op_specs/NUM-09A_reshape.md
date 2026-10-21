---
spec_schema_version: 1
id: NUM-09A
parent_id: NUM-09
function: reshape
proposed_operation_keys:
  - array.reshape_strict
  - array.reshape_accelerated_apple_silicon
  - array.reshape_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-09A: reshape

Reshape preserves the row-major logical element sequence of a single-tensor Result with any schema/member key. The source's complete `sample_shape()`, including batch axes, has rank 1..8, positive extents and at most 2^40 elements. Required static String `shape` is a canonical comma-separated list of positive extents with rank 1..8 and the same product. There is no inferred dimension or order parameter.

Output port `values` uses `photospider.tensor` v1 with member `samples`; the target shape consists of ordinary axes. UInt8, Int8, UInt16, Int16, Int64, Float32 and Float64 are supported with unchanged dtype. Every raw element bit is preserved, including signaling NaNs, payloads and signed zeros. All three CPU profiles have identical bitwise semantics. Profile availability is checked before invocation.

## Mapping and fixed execution

For source shape I, target shape O and global output coordinate o, let `k = sum_j o[j] * product_{m>j} O[m]`. Source coordinate j is `floor(k / product_{m>j} I[m]) mod I[j]`. This mapping is independent of physical strides.

Each selected output tile lowers to one fixed CPU task. Lowering decomposes its row-major ordinal intervals into source rectangles and declares exactly the input tiles touched by the mapping. Input tile boundaries may admit neighboring samples. The output tile owns a separate dense allocation. Source affine views may have negative or zero strides and unaligned offsets, subject to physical bounds validation. Empty selection creates no task or payload reads.

The layout policy parameter is removed, including the SDK `TransformLayout` enum. Runtime owner geometry cannot change Plan topology or produce output aliases. The helper signature accepts `CpuNumericProfile` after `shape`. Saved tasks keep their source views and separate output allocation through execution. Final Result coverage consists of the selected output tiles in global coordinates.

## Resources, errors and acceptance

Preparation validates the complete fixed invocation signature and recomputes its source tile set. Runtime copies only selected output samples, with O(rank) coordinate work per sample, a bounded stack buffer and zero payload scratch. Cancellation is sampled at entry and every 64 copied samples. Invalid static shape/schema fails before execution; resource and cancellation failures prevent successful call publication. Output storage outlives execution context destruction. Fixed invocations do not provide per-sample diagnostics or content-cache/view fallback behavior.

For source `[2,3]` with logical values `[0,1,2,3,4,5]`, target `[3,2]` yields `[[0,1],[2,3],[4,5]]`. Temporary checks use independent integer flatten/unflatten and raw-bit comparison, including partial tiles, changed rank, source batch/atomic topology, negative/zero strides, unaligned offsets, artifact import and concurrent Runs. Compare declared input tiles against the exact mapped coordinate union. See [fixed layout transforms](../layouts-execution.md). This execution contract takes precedence over the [NUM baseline](NUM_common_contract.md); specification status remains Proposed.
