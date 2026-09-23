---
spec_schema_version: 1
id: FMT-05A
parent_id: FMT-05
function: set_alpha
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - alpha.set_strict
  - alpha.set_accelerated_apple_silicon
  - alpha.set_accelerated_x86_64
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-05A: set or add an internal alpha plane

Inherit the complete [FMT-05 contract](FMT-05_alpha_editing_contract.md).
These are proposed native entries, not aliases of existing registered operations.
This member preserves straight color bytes and establishes the selected group's
alpha at a declared channel inside the result tensor.

## Ports and static parameters

`input` is a described straight image tensor, possibly without alpha or a
component Gray tensor without a channel axis. Optional `alpha` supplies the
explicit external plane/scalar source. One `values` output preserves dtype.
Both input and supplied alpha use the same Float32/Float64 dtype. Alpha means
normalized [0,1]; no cast, normalization, resampling or premultiplication occurs.

Inherit group, metadata_mode, conditional metadata_override, axis and layout.
Additional parameters are:

| Parameter | Definition |
| --- | --- |
| alpha_source | Required static internal channel selector, external_plane, or scalar. Internal selectors use separate exact index/name/role namespaces, resolved uniquely against original input. External/scalar requires the alpha port. |
| placement | preserve by default only when the group already has an alpha channel; otherwise explicit channel is required. |
| channel_index | Required only for placement=channel; nonnegative Int64 final output slot. Preserve forbids this field rather than silently ignoring it. |
| output_axis | Required only to insert a channel axis into a component Gray input; Int64 in [0,input_rank]. A channel-bearing input retains its axis and forbids insertion. |

An external plane has exactly S and compatible declared coordinates. A scalar
has exactly [1] and is explicitly expanded over S for this Run. No implicit
squeeze or ordinary plane broadcasting occurs. Source component semantics may
be assigned to the target alpha role without a separate raw flag, following
FMT-02 target assignment. This does not authorize spatial-grid conflicts or
non-normalized alpha samples. Use an explicit source metadata adaptation for
conflicting external coordinates before connecting that source.

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

The notation uses logical channel slots; actual channel axis and planar addresses
follow the family. Preserve is the corresponding identity map with its alpha
slot replaced. Reads always use the original immutable source, including when
an original channel supplies new alpha and channel indices shift in the result.

Retain straight group interpretation and remap every surviving reference. The
selected group references j. A moved/replaced alpha retains applicable old alpha
component meaning; a new alpha receives its declared alpha meaning without a
guessed unique name. Do not propagate a conflicting source Gray/RGB role as the
output alpha role. Other groups retain their original samples and references
through remapping; no external persistent relation remains.

## Demand, layout and validation

Requested new alpha reads only a(u), requiring finite [0,1]. Requested selected
color reads that original color and a(u), requiring finite color and valid alpha;
then copies the color bits, including nonzero hidden color at a=0. Unrelated
channels read only their original mapped samples. No old-alpha read is required
unless it is explicitly selected as a source or retained/requested elsewhere.
A's new-alpha dirty map includes the validation dependency of selected colors.

Auto/view/materialize obey the family. A view still checks these required samples
before publishing success. Failed validation cannot become a successful copy.
External owners cannot be silently combined into a multi-owner image. Only
requested output coverage is published; alpha read for R validation does not by
itself publish the output alpha plane. Use the family's resource, bounded-copy,
cancellation, cache and diagnostic rules; arithmetic profiles do not relax copies
or finite/range predicates.

## Independent acceptance

For RGBA [0.5,-2,4,0.5], scalar alpha=0.25 and preserve, expect
[0.5,-2,4,0.25]. With alpha=0, expect [0.5,-2,4,0], retaining hidden colors.
No division by the old alpha occurs, even if that discarded old sample is invalid.
A requested invalid new alpha fails; requesting only an unrelated AOV succeeds
without accessing the new alpha. Requested NaN color fails even at new alpha=0.

For old [R,A,G,B] with private A, placement=channel and channel_index=3 gives
[R,G,B,newA]. If old A is shared, the same final index gives [R,A,G,newA,B],
with the other group's alpha reference still at 1. Preserve on that shared slot
fails. Recompute every group index using an independent slot-list oracle.
For component Gray [7,9], inserting axis=1 and scalar 0.5 at channel_index=1
gives shape [2,2] with rows [7,0.5] and [9,0.5].

Request only a selected color and change scalar alpha from valid to invalid:
color bytes are unchanged but the observation must fail revalidation. Test view
and materialize equivalence, ViewUnavailable, source failure and cancellation.
Conceptual workflow: image + explicit alpha source -> A -> straight image.
Implementation must provide actual public commands and sample/metadata checks;
these fixtures do not claim an implemented registry entry.
