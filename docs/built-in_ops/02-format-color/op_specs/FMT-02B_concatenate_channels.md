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

Inherits the [FMT-02 family contract](FMT-02_channel_assembly_contract.md). This member concatenates ordered same-dtype channel blocks. Input axes may differ; each is resolved from the corresponding explicit `input_descriptions` entry or axis parameter. After removing each channel axis, the remaining dimensions match in order and extent. The output carries only structure and copied samples; a later consumer receives its own explicit description.

## Interface

Ordered inputs `inputs[0..n)` (1..1024) are single-tensor Results and produce one `values` tensor. All inputs and output have the same cell rank 1..8 and dtype; batch plus cell rank is at most 8. Parameters are static optional `input_descriptions`, `layout`, optional per-input cell-axis `input_axes`, and required cell-axis `output_axis` in [0,r). These indices exclude the Result batch prefix.

## Mathematics

Let ci be input i's channel count and pi its prefix offset, with p0=0 and p(i+1)=pi+ci. For output coordinate j, let k=j[output_axis]. Choose the unique i with pi<=k<p(i+1), insert k-pi at source axis ai in erase(j,output_axis), and copy that source element's exact bytes. Order within every input block is preserved.

## Behavior

Output sample j depends on one sample of one input; inputs whose channel interval is not requested contribute only their static schema. Auto/materialize copy to independent output tiles; a nonempty view fails.

## Acceptance

Input X0 has shape [2,1,2] with channel axis 0 and logical channel planes:

```
c0 = [[10,11]]
c1 = [[20,21]]
```

Input X1 has shape [1,2,1], channel axis 2, and logical samples `[[[30],[31]]]`. Both nonchannel shapes are [1,2]. With output_axis=2, output shape is [1,2,3] and samples are `[[[10,20,30],[11,21,31]]]`. At (0,1,2), the value comes from X1[0,1,0]=31. Output channels 1..2 map to X0 channel 1 and X1 channel 0.

Input [1,1,1] cannot implicitly broadcast to X1's nonchannel shape. Input-list order and each input's internal order are both checked.

For rank-one inputs [7,9] and [11], axes are 0 and output is [7,9,11]. One input [7,9] gives exact identity. An empty nonchannel shape does not introduce a rank-zero output. Three explicitly singleton-channel Gray tensors concatenated and passed to a consumer configured with an RGB group keep bytes and order unchanged.

## Conceptual DAG

`described color block + independent alpha block -> FMT-02B -> result -> explicitly configured consumer`. Joining alpha copies it; it does not associate the color values.
