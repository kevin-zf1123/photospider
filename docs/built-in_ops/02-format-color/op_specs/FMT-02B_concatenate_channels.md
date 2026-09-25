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
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-02B: concatenate existing channel axes

Implementation: package 0.21.0 registers A/B/C CPU profiles with exact byte
mapping, tensor-description v2, canonical static parameters, and legal retained
views. See the [public API and runnable workflow](../../../kernel-architecture/Channel-and-Color-Operations.md#fmt-02-channel-assembly)
and [performance workflow](../../../../examples/channel_assembly_performance/README.md).
Decision status remains Proposed; implementation facts below supersede the
historical inspection's missing-runtime statements.


Inherit the [FMT-02 family contract](FMT-02_channel_assembly_contract.md).
The implemented keys are separate default-registry primitives.
This member concatenates ordered same-dtype channel blocks. Input axes may
differ; resolve each from effective metadata or explicit parameters. After
removing each channel axis, remaining dimensions must match in order and extent.
Required static `output_axis` fixes the result's channel-axis position.
Explicit target channel/group semantics authorize corresponding reinterpretation
as a standard B capability, without a separate source override/raw.

Let ci be input i's channel count and pi its prefix offset, with p0=0 and
p(i+1)=pi+ci. For output coordinate j, let k=j[output_axis]. Choose the unique i
with pi<=k<p(i+1), insert k-pi at source axis ai in erase(j,output_axis), and copy
that source element's exact bytes. Preserve order within every input block.
Neither metadata nor physical tiling changes this coordinate rule.

## Interface and regional specialization

Ordered repeated inputs `inputs[0..n)` produce one `values` tensor. n>=1; all
inputs and output have the same rank 1..8 and dtype. Parameters are static
`metadata_mode`, conditional `input_overrides`, optional `output_description`,
`layout`, optional per-input `input_axes`, and required `output_axis` in [0,r).
The family defines axis assertions, raw requirements, dtype/count/arity limits,
metadata consistency and the complete support matrix.

For requested Q and input i, intersect output channels with [pi,p(i+1)),
subtract pi, and move the channel coordinate from output_axis to ai. That
exact footprint is Data; an empty intersection reads no payload. Descriptor
checks still cover all connected inputs; no Control or additional sample
Validation payload is needed. Dirty mapping moves ai to output_axis and adds
pi, then intersects the observed output. Preserve gaps and global coordinates.

Use the family's algorithm, resource/error rules and performance cases. Prefix
construction is O(n); do not search all n inputs for each sample. Account source
aliases once at backing identity, retaining each port's logical contribution.
For one input with equal source/output axes, shape and samples are identical;
explicit output metadata or materialization may change description or backing.

## Independent analytic fixture

Input X0 has shape [2,1,2] with channel axis 0 and logical channel planes:

```
c0 = [[10,11]]
c1 = [[20,21]]
```

Input X1 has shape [1,2,1], channel axis 2, and logical samples
`[[[30],[31]]]`. Both nonchannel shapes are [1,2]. With output_axis=2, output
shape is [1,2,3] and samples are `[[[10,20,30],[11,21,31]]]`.
Requesting only (0,1,2) reads X1[0,1,0]=31 and no X0 payload. An X0-only change
cannot dirty that observation. Output channels 1..2 map to X0 channel 1 and
X1 channel 0 at their separate source-axis positions.

The independent oracle derives input channel intervals from prefix sums and
performs coordinate insertion/removal separately from the production mapper.
Input [1,1,1] cannot implicitly broadcast to X1's nonchannel shape. Input-list
order and each input's internal order must both be checked.

The conceptual workflow is `described color block + independent alpha block ->
FMT-02B -> explicitly grouped result`. Joining alpha copies it; it does not
associate the color values. Public executable commands and measured outcomes are linked above.
For rank-one inputs [7,9] and [11], axes are 0 and output is [7,9,11]. One input
[7,9] gives exact identity. An empty nonchannel shape does not introduce a
rank-zero output. Arbitrary source/destination mapping belongs to FMT-02C;
B retains ordered block concatenation. Also test three explicitly singleton-
channel Gray tensors concatenated with a target RGB group: bytes and order
remain unchanged, and source Gray descriptions remain unchanged.
