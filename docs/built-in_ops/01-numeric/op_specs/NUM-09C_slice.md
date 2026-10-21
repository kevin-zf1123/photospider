---
spec_schema_version: 1
id: NUM-09C
parent_id: NUM-09
function: slice
proposed_operation_keys:
  - array.slice_strict
  - array.slice_accelerated_apple_silicon
  - array.slice_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-09C: slice

Slice preserves rank and selects `s[j]=starts[j]+o[j]*steps[j]` from one source Result. The source has one tensor under any schema/member key; its complete `sample_shape()` includes batch axes. Dynamic `starts` and `steps` are Int64 `[rank]` Result inputs. Required static String `counts` is a canonical comma-separated list of positive output extents. Rank is 1..8 and both logical counts are at most 2^40. Each output extent must not exceed its source extent, since larger counts have no valid nonzero-step controls.

Output port `values` uses `photospider.tensor` v1/member `samples`, with ordinary axes and unchanged dtype. UInt8, Int8, UInt16, Int16, Int64, Float32 and Float64 are copied bit-for-bit, including signaling NaNs. No implicit cast, rank squeezing, negative-index reinterpretation or empty slice is provided. All three CPU profiles preserve identical bits.

## Dynamic domain and fixed source set

Every nonempty task checks the complete slice domain before source sample access. Starts must be nonnegative absolute indices below each source extent. For counts greater than one, steps must be nonzero and `start+(count-1)*step` must remain in bounds, computed with widened exact integer arithmetic. This check uses full counts even if the selected output tile would itself be in bounds. Invalid controls return `InvalidArgument` with `FailureReason::InvalidDomain` and prevent successful call publication.

A singleton axis ignores its step numerically. If every count is one, specialization excludes the step payload and producer; its declared static schema is still checked. Otherwise each task declares the complete steps vector, including entries belonging to singleton axes. Every nonempty task declares the complete starts vector. Empty output selection emits no task and reads no controls or source payload.

The Plan's source set covers all coordinates reachable by legal controls. For input extent I, count N and selected output interval `[u,u+e)`, a singleton requires `[0,I)`. Otherwise the possible source set is `[u,I-N+u+e) union [N-u-e,I-u)`. Positive-step possibilities are contained in step +1, and negative-step possibilities in step -1; those steps realize each respective interval. The multi-axis Cartesian product gives at most 256 boxes at rank eight. Declare all source tiles intersecting their union. This fixed set can exceed the source accessed by one Run, while excluding coordinates unreachable by any legal control binding.

## Allocation, preparation and acceptance

Each selected output tile has one fixed CPU gather task and a separate dense allocation. The old `layout` parameter and SDK `TransformLayout` enum are removed; the helper accepts the profile after `counts`. Physical source strides may be negative, zero or unaligned after bounds validation. Runtime controls select coordinates within declared reads and cannot change topology or ownership.

Prepare recomputes the possible-source tile set and verifies every read/write slot, schema and zero scratch. Runtime uses bounded stack coordinates, controls and a 32-byte buffer. Cancellation is sampled at entry and every 64 copied samples. Operational failures prevent successful call retirement. Output owners survive execution context destruction. No per-sample diagnostics, payload allocations or runtime view fallback are used.

For input `[0,1,2,3,4,5]`, starts `[4]`, steps `[-2]` and counts `"3"`, expect `[4,2,0]`. Temporary checks use independent signed integer coordinate enumeration. Verify complete-domain rejection for partial output, all-singleton step elision, dynamic control changes on one PreparedPlan, extreme ignored steps, all seven raw dtypes, sparse requests, possible-source tile equality, concurrent Runs and source stride cases. See [fixed layout transforms](../layouts-execution.md). This contract overrides the [NUM baseline](NUM_common_contract.md); specification status remains Proposed.
