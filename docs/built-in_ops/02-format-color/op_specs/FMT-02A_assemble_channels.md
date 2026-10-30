---
spec_schema_version: 1
id: FMT-02A
parent_id: FMT-02
function: assemble_channels
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu
clarification_status: complete
implemented_operation_keys:
  - channel.assemble_strict
  - channel.assemble_accelerated_apple_silicon
  - channel.assemble_accelerated_x86_64
---

# FMT-02A: assemble single components along a new channel axis

Inherits the [FMT-02 family contract](FMT-02_channel_assembly_contract.md). This member assembles ordered equal-shape, same-dtype single-component inputs. An input whose `input_descriptions` entry declares a channel axis is rejected. The output carries no description; Gray/Black-White components serve as RGB or alpha for a later consumer whose own description says so.

## Interface

Ordered inputs `inputs[0..n)` (1..1024) are single-tensor Results and produce one `values` tensor. Input cell rank is 1..7 and output cell rank is 2..8; batch plus cell rank is at most 8. Parameters are static optional `input_descriptions`, `layout` and required cell-axis Int64 `axis` in [0,r], excluding the Result batch prefix. There is no keepdims or implicit squeeze. Same dtype and exact nonchannel sizes are required.

## Mathematics

For n input tensors Xi of shape S and rank r, output Y has shape insert(S,a,n) and rank r+1:

```
Y[j] = X[j[a]][erase(j,a)]
```

The operation copies element bytes exactly, without casting, broadcast, resampling, transfer, color conversion or alpha association. Auto/materialize write independent dense output tiles; a nonempty view fails as specified by the family.

## Behavior

Output sample j depends on the single sample of input j[a] at erase(j,a). Inputs not referenced by a requested output contribute only their static schema.

## Acceptance

Use three Float32 inputs with shape [2,2]:

```
R = [[10,11], [12,13]]
G = [[20,21], [22,23]]
B = [[30,31], [32,33]]
```

With axis=2, output shape is [2,2,3] and the logical values are:

```
[[[10,20,30], [11,21,31]],
 [[12,22,32], [13,23,33]]]
```

With axis=0, shape is [3,2,2] and the logical planes appear in input-list order. At output (1,0,1), the copied value is G[1,0]=22.

For n=1 and X0=[7,9], axis=1 yields shape [2,1] with samples [[7],[9]]. Extracting that singleton channel with keepdims=false recovers [7,9] exactly.

Interpretation fixture: assemble R/G/B above (regarded as three independent Gray planes) and pass the result to a consumer whose `input_description` declares an RGB group on indices 0..2; the bytes equal the plain assembly. A Float32 Black/White plane [0,1] assembled into a slot that a later consumer describes as alpha remains [0,1]; no range conversion occurs.

## Conceptual DAG

`FMT-01 split -> independent component processing -> FMT-02A -> named tensor`
