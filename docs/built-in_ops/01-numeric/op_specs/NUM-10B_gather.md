---
spec_schema_version: 1
id: NUM-10B
parent_id: NUM-10
function: gather
proposed_operation_keys:
  - array.gather_strict
  - array.gather_accelerated_apple_silicon
  - array.gather_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-10B: gather

Gather selects source slices along required static Int64 `axis` using a dynamic Int64 indices tensor `[M]`. Inputs each contain one tensor under any schema/member key. Complete sample shapes include batch axes; source rank is 1..8 and all logical counts are at most 2^40. Source/output dtype is UInt8, Int64, Float32 or Float64. Output `values` uses `photospider.tensor`/`samples`, with ordinary source axes except `output.shape[axis]=M`.

For global output coordinate o, read source s with non-axis coordinates unchanged and `s[axis]=indices[o[axis]]`. Repeated indices preserve order. Raw source bits, including signaling NaNs, signed zeros and infinities, are copied without arithmetic. This is a single-axis gather.

## Fixed dependencies and unchecked indices

Nonempty selection creates one shared fixed task that reads the complete index vector and writes a private dense Int64 `[M,2]` table of `{original_position,source_index}` pairs. All indices lie in the complete source axis as an unchecked caller premise. The task performs no index validation or publication scan. It uses `16*M` bytes of private payload and no arithmetic scratch.

Each selected output tile has one task reading that table and source tiles matching the output's non-axis region with the axis expanded to its full source extent. This possible-source coverage can exceed the coordinates used by a particular indices binding. The task writes its own dense output allocation. Empty selection emits no tasks or payload reads. Source affine views may be negative-stride, zero-stride or unaligned after physical validation.

Prepare proves the exact source and private table view signatures. Normalized source positions and their association are unchecked caller premises, including for imported programs. Topology and allocations remain fixed when indices change. Index construction checks cancellation every 256 entries; output copying every 64 samples. Resource/cancellation failures prevent successful call publication. Successful Results retain output storage beyond context lifetime; no runtime payload allocation, per-sample diagnostic or content cache is used.

Temporary checks cover nonleading axes, repeats, valid unselected indices, precise source tile closure, dynamic rebinding, sparse/empty selection, raw-bit preservation, source layouts, artifacts and concurrent Runs. See [fixed indexing execution](../indexing-execution.md). This contract overrides the [NUM baseline](NUM_common_contract.md); specification status remains Proposed.
