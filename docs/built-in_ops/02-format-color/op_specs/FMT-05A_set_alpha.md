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

The default registry provides `alpha.set_<profile>` for the three CPU profiles.
The operation uses fixed invocation ABI 8; the specification remains Proposed. It preserves
straight color samples while assigning or inserting the selected group's alpha
channel.

Inherit the complete [FMT-05 contract](FMT-05_alpha_editing_contract.md).
The key takes one or two single-tensor Results. It preserves straight
color bytes and establishes the selected group's alpha at a declared channel
inside the result tensor. Nonscalar batch prefixes must match exactly; scalar
alpha is unbatched `[1]`. The complete sample rank, including batch and cell axes,
is at most 8 and full sample count is at most 2^40.

## Ports and static parameters

`input` is a described straight image tensor, possibly without alpha or a
component Gray tensor without a channel axis. Optional `alpha` supplies the
explicit external plane/scalar source. One `values` output preserves dtype.
Both input and supplied alpha use the same Float32/Float64 dtype. Alpha means
normalized [0,1]; no cast, normalization, resampling or premultiplication occurs.

Inherit group, required input_description, axis and layout.
Additional parameters are:

| Parameter | Definition |
| --- | --- |
| alpha_source | Required static internal channel selector, external_plane, or scalar. Internal selectors use separate exact index/name/role namespaces, resolved uniquely against original input. External/scalar requires the alpha port. |
| placement | preserve by default only when the group already has an alpha channel; otherwise explicit channel is required. |
| channel_index | Required only for placement=channel; nonnegative Int64 final output slot. Preserve forbids this field rather than silently ignoring it. |
| output_axis | Required only to insert a channel axis into a component Gray input; Int64 in [0,input_rank]. A channel-bearing input retains its axis and forbids insertion. |

An external plane has exactly S and the same batch prefix/dtype. A scalar is
unbatched shape `[1]` and explicitly expanded over S. External/scalar sources
are ordinary Results without domain labels; their requested values must satisfy
the alpha rule. No implicit squeeze, resampling or plane broadcasting occurs.

## Output inference and mathematical reference

For preserve, replace the private existing alpha slot in place. Reject a shared
slot, even if the proposed source happens to be that same channel. For channel,
remove the old slot if unreferenced, retain it otherwise, then insert new alpha
at the final specified index. An insertion never overwrites existing color/AOV
values. If the nonchannel shape is S, output shape is insert(S,a,Cout), with
Cout=C for a moved/replaced private slot and Cout=C+1 for no old slot or a retained
shared slot. Component Gray inserts an extent-two axis, with its original color
and the new alpha placed according to channel_index in [0,1].

Let L be the ordered surviving original channel indices and j the new alpha
position. Output slots other than j map in order to L. For nonchannel coordinate
u and same-dtype alpha source a(u):

```
Y[u,j] = a(u)
Y[u,k] = original input[u,L[k - (k>j)]]   for k != j
```

The notation uses logical channel slots; actual channel axis and fixed physical strides
follow the family. Preserve is the corresponding identity map with its alpha
slot replaced. Reads always use the original immutable source, including when
an original channel supplies new alpha and channel indices shift in the result.

The output contains the mapped samples and structural schema only. It has no
alpha-role or group metadata. Callers construct the resulting straight-image
configuration for downstream operations, with the new alpha at slot j and any
surviving indices remapped. Other groups' sample bits remain unchanged.

## Fixed reads, layout and caller preconditions

Each complete retained output tile declares only the original source for each
mapped output slot. New alpha slots read the declared internal/plane/scalar alpha;
color and unrelated slots read their original samples. Set adds no alpha read
solely to validate a copied color. Static descriptions and all input schemas are
still checked, while payload-unused ports retain only schema dependencies.

The caller supplies finite selected colors and normalized finite alpha for the
semantic values being represented; these obligations do not introduce runtime
validation tasks. Producing a color-only tile does not certify a new alpha source.
An output tile that also contains the alpha slot still requires its payload.

Auto and materialize copy to independent dense output tiles with zero scratch.
Every nonempty view task has schema-only dependencies and returns
`InvalidArgument / ViewUnavailable`. Empty has no task and retains static checks.
Public Results appear after successful ordered call retirement and retain their
allocations independently of the input and context.

## Independent acceptance

For RGBA [0.5,-2,4,0.5], scalar alpha=0.25 and preserve, expect
[0.5,-2,4,0.25]. With alpha=0, expect [0.5,-2,4,0], retaining hidden colors.
No division by the old alpha occurs, even if that discarded old sample is invalid.
Invalid represented alpha or selected color violates the caller premises.
A tile containing only copied colors/AOVs has no new-alpha payload dependency.

For old [R,A,G,B] with private A, placement=channel and channel_index=3 gives
[R,G,B,newA]. If old A is shared, the same final index gives [R,A,G,newA,B],
with the other group's alpha reference still at 1. Preserve on that shared slot
fails. Recompute every group index using an independent slot-list oracle.
For component Gray [7,9], inserting axis=1 and scalar 0.5 at channel_index=1
gives shape [2,2] with rows [7,0.5] and [9,0.5].

Select a color-only tile and verify that the alpha producer is omitted. Select
a tile containing the new alpha and verify that its producer is required. Check
exact copies, schema-only view failure, Empty, source failure and cancellation.
Conceptual workflow: image + explicit alpha source -> A -> image with the
selected alpha relationship. The registered key is `alpha.set_<profile>`; the
specification remains Proposed.
