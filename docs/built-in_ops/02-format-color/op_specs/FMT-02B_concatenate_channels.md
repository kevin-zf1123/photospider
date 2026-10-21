---
spec_schema_version: 1
id: FMT-02B
parent_id: FMT-02
function: concatenate_channels
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu
clarification_status: complete
implemented_operation_keys:
  - channel.concatenate_strict
  - channel.concatenate_accelerated_apple_silicon
  - channel.concatenate_accelerated_x86_64
---

# FMT-02B: concatenate existing channel axes

The default C++ `OperationRegistry` registers this member's `strict`, `accelerated_apple_silicon` and `accelerated_x86_64` CPU variants as Result operations. These built-ins execute through fixed invocation ABI 8. Public authoring helpers are installed in `photospider/ops/format/channel_assembly.hpp` and `channel_editing.hpp`. The FMT specification remains Proposed independently of the registered implementation.

Inherit the [FMT-02 family contract](FMT-02_channel_assembly_contract.md).
The implemented keys are separate default-registry primitives.
This member concatenates ordered same-dtype channel blocks. Input axes may
differ; resolve each from the corresponding explicit `input_descriptions` entry or axis parameter. After
removing each channel axis, remaining dimensions must match in order and extent.
Required static `output_axis` fixes the result's channel-axis position.
The output carries only structure and copied samples. A later consumer receives its own explicit description.

Let ci be input i's channel count and pi its prefix offset, with p0=0 and
p(i+1)=pi+ci. For output coordinate j, let k=j[output_axis]. Choose the unique i
with pi<=k<p(i+1), insert k-pi at source axis ai in erase(j,output_axis), and copy
that source element's exact bytes. Preserve order within every input block.
Neither metadata nor physical tiling changes this coordinate rule.

## Interface and regional specialization

Ordered inputs `inputs[0..n)` are single-tensor Results and produce one
`values` tensor. The family allows 1..1024 inputs. All inputs and output have
the same cell rank 1..8 and dtype; batch plus cell rank is at most 8. Parameters
are static
optional `input_descriptions`, `layout`, optional per-input cell-axis
`input_axes`, and required cell-axis
`output_axis` in [0,r). These indices exclude the Result batch prefix; complete
sample rank, including batch and cell axes, is at most 8.
The family defines axis assertions, raw requirements, dtype/count/arity limits,
description/descriptor consistency and the complete support matrix.

For each complete selected output tile, intersect channels with input i's prefix interval, subtract pi and move the channel coordinate to ai. Lowering unions the source-grid tiles for those mapped regions. Empty intersections are schema-only. Auto/materialize copy to independent output tiles; nonempty view fails without source payload reads. Runtime uses a binary span lookup rather than scanning every input for every sample. The family defines cancellation, ownership and bounded storage.

## Independent analytic fixture

Input X0 has shape [2,1,2] with channel axis 0 and logical channel planes:

```
c0 = [[10,11]]
c1 = [[20,21]]
```

Input X1 has shape [1,2,1], channel axis 2, and logical samples
`[[[30],[31]]]`. Both nonchannel shapes are [1,2]. With output_axis=2, output
shape is [1,2,3] and samples are `[[[10,20,30],[11,21,31]]]`.
At (0,1,2), the value comes from X1[0,1,0]=31. X0 payload is omitted only when normalized output tiles contain no X0-backed channels. Output channels 1..2 map to X0 channel 1 and
X1 channel 0 at their separate source-axis positions.

The independent oracle derives input channel intervals from prefix sums and
performs coordinate insertion/removal separately from the production mapper.
Input [1,1,1] cannot implicitly broadcast to X1's nonchannel shape. Input-list
order and each input's internal order must both be checked.

The conceptual workflow is `described color block + independent alpha block ->
FMT-02B -> result -> explicitly configured consumer`. Joining alpha copies it; it does not
associate the color values.
For rank-one inputs [7,9] and [11], axes are 0 and output is [7,9,11]. One input
[7,9] gives exact identity. An empty nonchannel shape does not introduce a
rank-zero output. Arbitrary source/destination mapping belongs to FMT-02C;
B retains ordered block concatenation. Also test three explicitly singleton-
channel Gray tensors concatenated and passed to a consumer configured with an RGB group: bytes and order remain unchanged. The source descriptions remain caller-owned configuration.
