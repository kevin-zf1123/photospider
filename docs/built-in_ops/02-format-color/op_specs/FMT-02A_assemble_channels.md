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

The default C++ `OperationRegistry` registers this member's `strict`, `accelerated_apple_silicon` and `accelerated_x86_64` CPU variants as Result operations. These built-ins execute through fixed invocation ABI 8. Public authoring helpers are installed in `photospider/ops/format/channel_assembly.hpp` and `channel_editing.hpp`. The FMT specification remains Proposed independently of the registered implementation.

Inherit the [FMT-02 family contract](FMT-02_channel_assembly_contract.md).
The implemented keys are separate default-registry primitives.
This member assembles ordered equal-shape, same-dtype single-component inputs.
An input whose `input_descriptions` entry declares a channel axis is rejected.
Required static `axis` inserts the output channel dimension without squeezing
any input dimension. The output carries no description; Gray/Black-White
components serve as RGB or alpha for a later consumer whose own description says
so. This Proposed member is not the legacy `channel.merge`.

For n input tensors Xi of shape S and rank r, output Y has shape insert(S,a,n)
and rank r+1. Its coordinate-copy rule is:

```
Y[j] = X[j[a]][erase(j,a)]
```

The operation copies element bytes exactly, without casting, broadcast, resampling, transfer, color conversion or alpha association. The `auto`, `view` and `materialize` policies follow the family storage contract.

Auto/materialize write independent dense output tiles. Nonempty view fails with schema-only dependencies, as specified by the family.

## Interface and regional specialization

Ordered inputs `inputs[0..n)` are single-tensor Results and produce one
`values` tensor. The family allows 1..1024 inputs. Input cell rank is 1..7 and output cell rank is 2..8; batch plus cell rank is at most 8. The family supplies target dtypes,
checked extent/count limits, host arity admission and the complete support matrix.
Parameters are static optional `input_descriptions`, `layout` and required
cell-axis Int64 `axis` in [0,r].
The axis excludes the Result batch prefix; batch plus cell rank must be at most 8. There is no
keepdims or implicit squeeze. Same dtype and exact nonchannel sizes are required.

For each complete selected output tile, input i logically supplies its channel-i intersection with the output axis removed. Lowering closes that mapped region through the source grid. Unselected ports are schema-only; static validation covers every connected input. Source and output grid closure may cover additional channels, so sample-local equations do not imply single-channel physical grants.

## Independent analytic fixture

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

With axis=0, shape is [3,2,2] and the logical planes appear in input-list order.
At output (1,0,1), the copied value is G[1,0]=22. R/B payloads are omitted only when the selected output tiles contain no R/B destinations.
An independent oracle enumerates output coordinates, chooses the input with
j[a], removes that axis and compares element bytes using a separate source
address evaluator. It must not call the production assembly mapper.

The conceptual workflow is `FMT-01 split -> independent component processing ->
FMT-02A -> named tensor`.
For n=1 and X0=[7,9], axis=1 yields shape [2,1] with samples [[7],[9]].
Extracting that singleton channel with keepdims=false recovers [7,9] exactly.
Full split/reassemble identity also requires original ordering; any grouping is
restated by the next consumer's description. Outputs retain independently owned copies. Arbitrary source/destination mapping
belongs to FMT-02C; A retains its ordered assembly behavior.

For an interpretation fixture, assemble R/G/B above (which the caller regards
as three independent Gray planes) and pass the result to a consumer whose
`input_description` declares an RGB group on indices 0..2; require the same
bytes as the plain assembly. A Float32 Black/White plane [0,1] assembled into a
slot that a later consumer describes as alpha remains [0,1]; no range conversion
occurs.
