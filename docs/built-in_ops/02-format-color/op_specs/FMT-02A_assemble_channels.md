---
spec_schema_version: 1
id: FMT-02A
parent_id: FMT-02
function: assemble_channels
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - channel.assemble_strict
  - channel.assemble_accelerated_apple_silicon
  - channel.assemble_accelerated_x86_64
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-02A: assemble single components along a new channel axis

Inherit the [FMT-02 family contract](FMT-02_channel_assembly_contract.md).
The proposed keys target the default registry; they are not registered aliases.
This member assembles ordered equal-shape, same-dtype single-component inputs.
Inputs have no effective existing channel axis. Required static `axis` inserts
the output channel dimension without squeezing any input dimension. Complete
output groups are explicit; component descriptions are remapped under the
family's metadata rules. Explicit target per-channel/group semantics authorize
reinterpretation as a standard A capability; Gray/Black-White components do not
require an additional source override/raw merely to serve as RGB or alpha.
This Proposed member is not the legacy `channel.merge`.

For n input tensors Xi of shape S and rank r, output Y has shape insert(S,a,n)
and rank r+1. Its coordinate-copy rule is:

```
Y[j] = X[j[a]][erase(j,a)]
```

The result preserves element bytes. No implicit cast, broadcast, resampling,
transfer, color conversion or alpha association occurs. `auto`, `view` and
`materialize` follow the family/storage contract; independently owned planes do
not automatically form a legal zero-copy output image.

## Interface and regional specialization

Ordered repeated inputs `inputs[0..n)` produce one `values` tensor. n>=1;
input rank is 1..7 and output rank is 2..8. The family supplies target dtypes,
checked extent/count limits, host arity admission and the complete support matrix.
Parameters are static `metadata_mode`, conditional `input_overrides`, optional
`output_description`, `layout` and required Int64 `axis` in [0,r]. There is no
keepdims or implicit squeeze. Same dtype and exact nonchannel sizes are required.

For requested Q, input i Data is exactly erase(Q intersect channel i, a).
Its metadata contributes to Descriptor support; there is no Control or sample
Validation payload. Dirty input coordinates insert i at axis a, intersected with
observed output. Unrequested planes are not read or given pixel-domain validation.
Use the family's reference algorithm, resource accounting, errors and performance
cases. Materialized images reserve the full result span but publish only Q.

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
Requesting only (1,0,1) with axis=2 returns G[1,0]=22 and requests no R/B
payload. Changing R[1,0] does not dirty this observation; changing G[1,0] does.
An independent oracle enumerates output coordinates, chooses the input with
j[a], removes that axis and compares element bytes using a separate source
address evaluator. It must not call the production assembly mapper.

The conceptual workflow is `FMT-01 split -> independent component processing ->
FMT-02A -> named tensor`. Actual executable commands and public workflow results
belong to implementation delivery. No runtime tests were run for this draft.
For n=1 and X0=[7,9], axis=1 yields shape [2,1] with samples [[7],[9]].
Extracting that singleton channel with keepdims=false recovers [7,9] exactly.
Full split/reassemble identity also requires original ordering and explicit
original grouping. Byte identity does not guarantee view representability for
independently processed or copied inputs. Arbitrary source/destination mapping
belongs to FMT-02C; A retains its ordered assembly behavior.

For a semantic-assignment fixture, describe R/G/B above as three independent
Gray planes, explicitly declare the target RGB group, and require the same
output bytes. A Float32 Black/White plane [0,1] explicitly assigned to an alpha
output remains [0,1]. Output semantics do not perform range conversion. Verify
source descriptions remain unchanged and no old sample-validity proof is reused.
