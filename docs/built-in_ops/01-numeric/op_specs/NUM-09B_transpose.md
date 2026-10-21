---
spec_schema_version: 1
id: NUM-09B
parent_id: NUM-09
function: transpose
proposed_operation_keys:
  - array.transpose_strict
  - array.transpose_accelerated_apple_silicon
  - array.transpose_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-09B: transpose

Transpose permutes logical axes of a single-tensor Result with any schema/member key. Its complete `sample_shape()` includes batch axes, has rank 1..8, positive extents and at most 2^40 elements. Required static String `permutation` is a canonical comma-separated list containing each input axis exactly once. For permutation p and input shape I, output shape is `O[j]=I[p[j]]`, and each global output coordinate o reads source `s[p[j]]=o[j]`.

Output port `values` uses `photospider.tensor` v1/member `samples`, with ordinary axes and unchanged dtype. UInt8, Int8, UInt16, Int16, Int64, Float32 and Float64 are supported. Raw bits, including signaling NaNs and signed zeros, are preserved identically by all three CPU profiles. No image/color roles or arithmetic conversions are inferred.

## Fixed task mapping

Each selected output tile becomes a CPU gather task with an independent dense output allocation. Its input region is the axis-permuted output rectangle; the task reads every input tile intersecting that region, computed using the input's own grid. Input batch and atomic metadata may produce a different partition from the ordinary output grid. Empty selection emits no tasks or payload reads. Final Result coverage follows selected tiles in global coordinates.

The `layout` node parameter and SDK `TransformLayout` enum are removed. The helper accepts the profile immediately after `permutation`. All outputs use the fixed allocation contract; physical source strides do not choose a runtime view path. Negative/zero strides and unaligned source offsets remain supported after bounds validation.

## Resources, errors and acceptance

Inherit [reshape's preparation, ownership and cancellation contract](NUM-09A_reshape.md). Prepare recomputes the exact source tile set and checks all read/write slots, dtype and zero scratch. Runtime copies selected samples using O(rank) coordinate work and bounded stack state. Invalid permutations, schemas and static metadata fail before execution. Operational failures prevent successful publication; output owners outlive context destruction.

For source `[2,3,4]` with values `100*i+10*j+k` and permutation `[2,0,1]`, output `[4,2,3]` has `values[k,i,j]=100*i+10*j+k`. Temporary checks cover identity, rank-2/rank-3 permutations, rank eight, sparse requests, batch/atomic grids, all raw dtypes and source stride cases. Compare exact declared reads against an independent coordinate mapping. See [fixed layout transforms](../layouts-execution.md). This execution contract overrides the [NUM baseline](NUM_common_contract.md); specification status remains Proposed.
