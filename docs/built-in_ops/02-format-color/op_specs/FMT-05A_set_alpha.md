---
spec_schema_version: 1
id: FMT-05A
parent_id: FMT-05
function: set_alpha
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - alpha.set_strict
  - alpha.set_accelerated_apple_silicon
  - alpha.set_accelerated_x86_64
---

# FMT-05A: set or add an internal alpha plane

The registry provides `alpha.set_<profile>` for the three CPU profiles. The operation preserves straight color samples while assigning or inserting the selected group's alpha channel.

Inherits the [FMT-05 contract](FMT-05_alpha_editing_contract.md). The key takes one or two single-tensor Results. It establishes the selected group's alpha at a declared channel inside the result tensor. Nonscalar batch prefixes match exactly; scalar alpha is unbatched `[1]`. The complete sample rank, including batch and cell axes, is at most 8 and full sample count is at most 2^40.

## Interface

`input` is a described straight image tensor, possibly without alpha, or a component Gray tensor without a channel axis. Optional `alpha` supplies the explicit external plane/scalar source. One `values` output preserves dtype. Input and supplied alpha share the same Float32/Float64 dtype. Alpha means normalized [0,1]; no cast, normalization, resampling or premultiplication occurs.

Group, required input_description, axis and layout are inherited. Additional parameters:

| Parameter | Definition |
| --- | --- |
| alpha_source | Required static internal channel selector, external_plane, or scalar. Internal selectors use separate exact index/name/role namespaces, resolved uniquely against the original input. External/scalar requires the alpha port. |
| placement | preserve by default only when the group already has an alpha channel; otherwise explicit channel is required. |
| channel_index | Required only for placement=channel; nonnegative Int64 final output slot. Preserve forbids this field. |
| output_axis | Required only to insert a channel axis into a component Gray input; Int64 in [0,input_rank]. A channel-bearing input retains its axis and forbids insertion. |

An external plane has exactly S and the same batch prefix/dtype. A scalar is unbatched shape `[1]` and is expanded over S. External/scalar sources are ordinary Results without domain labels; their values satisfy the alpha rule as a caller premise. No squeeze, resampling or plane broadcasting occurs.

## Mathematics

For preserve, the private existing alpha slot is replaced in place. A shared slot is rejected, even if the proposed source is that same channel. For channel, the old slot is removed if unreferenced and retained otherwise, then new alpha is inserted at the final specified index. An insertion never overwrites existing color/AOV values. If the nonchannel shape is S, output shape is insert(S,a,Cout), with Cout=C for a moved/replaced private slot and Cout=C+1 for no old slot or a retained shared slot. Component Gray inserts an extent-two axis, with its original color and the new alpha placed according to channel_index in [0,1].

Let L be the ordered surviving original channel indices and j the new alpha position. Output slots other than j map in order to L. For nonchannel coordinate u and same-dtype alpha source a(u):

```
Y[u,j] = a(u)
Y[u,k] = original input[u,L[k - (k>j)]]   for k != j
```

Reads always use the original source, including when an original channel supplies new alpha and channel indices shift in the result. The output contains the mapped samples and structural schema only; callers construct the resulting straight-image configuration for downstream operations, with the new alpha at slot j and any surviving indices remapped. Other groups' sample bits are unchanged.

## Behavior

Each output slot depends on its mapped source: new alpha slots read the declared internal/plane/scalar alpha; color and unrelated slots read their original samples. Set adds no alpha read to validate a copied color. The caller supplies finite selected colors and normalized finite alpha; no runtime validation exists. Auto/materialize copy to independent dense output; a nonempty view returns `InvalidArgument / ViewUnavailable`. Empty retains static checks and emits no payload.

## Acceptance

For RGBA [0.5,-2,4,0.5], scalar alpha=0.25 and preserve, expect [0.5,-2,4,0.25]. With alpha=0, expect [0.5,-2,4,0], retaining hidden colors. No division by the old alpha occurs, even if that discarded old sample is invalid.

For old [R,A,G,B] with private A, placement=channel and channel_index=3 gives [R,G,B,newA]. If old A is shared, the same final index gives [R,A,G,newA,B], with the other group's alpha reference still at 1. Preserve on that shared slot fails. For component Gray [7,9], inserting axis=1 and scalar 0.5 at channel_index=1 gives shape [2,2] with rows [7,0.5] and [9,0.5].

## Conceptual DAG

image + explicit alpha source -> A -> image with the selected alpha relationship
