---
spec_schema_version: 1
id: FMT-01A
parent_id: FMT-01
function: extract_channel_index
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
proposed_operation_keys:
  - channel.extract_index_strict
  - channel.extract_index_accelerated_apple_silicon
  - channel.extract_index_accelerated_x86_64
repository_branch: ops-specs
repository_commit: d49d1840
inspection_commit: d49d1840
---

# FMT-01A: extract a channel by index

Runtime update: the CPU registrations and public split helper are implemented.
See the [implementation and runnable workflow](../../../kernel-architecture/Channel-and-Color-Operations.md#fmt-01-channel-extraction)
for current storage behavior, validation commands and the measured performance scope.
Proposed is retained as the specification decision status.


Inherit the complete [FMT-01 family contract](FMT-01_channel_extraction_contract.md)
for ports, shape inference, metadata modes, exact mapping, regional support,
layout, ownership, resources, errors and acceptance. These are proposed keys;
the removed `channel.extract` Whole typed-HWC node is a historical baseline,
not an alias or a conforming implementation of these entries.
Registration targets the default operation registry. The family supplies the
support matrix, reference/optimized algorithm, performance acceptance plan and
shared implementation dependencies; the selector and fixtures below specialize it.

## Interface and meaning

Input `input` is a tensor; output `values` is a same-dtype single-component tensor.
The common static parameters are `metadata_mode`, optional `axis`, conditional
`metadata_override`, `keepdims` and `layout`. Add required static Int64 `index`:
0<=index<input.shape[resolved_axis]. Index is zero-based, has no default, and
accepts neither negative-from-end shorthand nor clamping/wrapping. It is a
channel position, not a channel count, layer number or numeric input port.

All target FMT dtypes and positive rank-1..8 shapes are covered, subject to the
shared element/resource bounds. Keepdims=false removes the selected axis and
requires rank>=2; true retains it with extent 1. Untagged data works with an
explicit axis. A raw invocation ignores semantic axis selection and needs axis;
an ordinary conflicting designation fails unless override is explicit.

Set k=index in the family coordinate formula. Return each selected element
bit-for-bit, including nonfinite float values. No sample-domain color validation
is required: this node selects stored bytes, even if a selected alpha is outside
[0,1]. Applicable descriptions remain unvalidated component descriptions.
An already failing required producer still cannot supply a successful value.

## Dependency and storage specialization

For requested output Q, input Data is exactly Q with the selected source axis
inserted/fixed at index. There is no Control payload or additional sample
Validation domain. Structural metadata participates in descriptor inference and
propagation. Data changes on other source channels do not dirty this output;
selected-channel changes project to the same nonchannel positions. Both request
and dirty rules preserve nonzero global origins and disjoint gaps.

An input can expose requested components through owned views/read windows.
`materialize` copies only requested elements; image results use the kernel's
full-image virtual reservation with page backing provided on demand. `auto`
performs only the family-defined fallback. Resource accounting separates logical
Nq*d sample volume from the complete virtual span, the union of backed pages,
retained source pages and window/metadata overhead. Mapping/copy work is
O(Nq*rank+F*rank), plus page preparation and actual upstream work. Tile geometry
comes from the DAG and does not vary by extraction node.

## Independent fixtures and public workflow requirement

Use source shape [2,2,4], explicit axis=2, and logical samples:

```
[[[10,20,30,.25], [11,21,31,.5]],
 [[12,22,32,.75], [13,23,33,1]]]
```

For index=1, keepdims=false, output shape [2,2] and values are
`[[20,21],[22,23]]`. Keepdims=true gives shape [2,2,1] with the same four values.
A request only for output (1,0) reads only source (1,0,1) and returns 22.
Changing source (1,0,2) must not invalidate this result; changing (1,0,1) must.
For source [4], index=2, keepdims=true yields [1] containing its third value;
the same call with keepdims=false fails preflight without reading samples.

The conceptual public workflow is `bound tensor -> FMT-01A -> named result`.
Implementation delivery must supply an actual WorkflowDocument/Compiler/
ExecutionContext example with bindings, parameters, an offset ROI and checked
output. Do not publish this conceptual node as runnable until its registry entry
and shared metadata migration exist. Legacy extraction tests are reusable inputs,
not evidence that this target has executed.

The independent oracle enumerates output integer coordinates, inserts index at
axis and compares source/output byte sequences using a separate stride evaluator.
It must not call the production selector or metadata projector. Cover:

- Axis-first/middle/last and rank-eight shapes, index 0/C-1, C=1, keepdims both
  ways, and equal semantic data under different logical axis descriptions of planar storage.
- Every supported dtype and exact signed-zero/NaN payload/Inf bit fixtures;
  unrelated malformed color samples remain unread with a regional raw producer.
- Full-domain requests versus offset/disjoint ROI/tile partitions, varying the
  common tile configuration between DAGs while keeping one geometry within each
  DAG. Include tile edges and reject independently mismatched image block settings.
- Generic negative/zero strides, unaligned samples and owner-fragment boundaries;
  planar-image tile/page boundaries within one address-space owner; forced
  view availability, auto fallback, requested-page materialization and no full-input gather.
- Index/axis errors, metadata assertions/overrides, low capacity/work/stage,
  bounded cancellation, cache-off and view survival after context destruction.

No new numerical computation or tolerance is introduced: all successful profiles
compare exact bytes. No implementation or runtime tests were run for this spec.
