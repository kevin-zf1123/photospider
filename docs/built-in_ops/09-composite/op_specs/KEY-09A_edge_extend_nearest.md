---
spec_schema_version: 1
id: KEY-09A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: KEY-09
function: edge_extend_nearest
execution: tile
proposed_operation_keys:
  - keying.edge_extend_nearest
---

# KEY-09A: Extend original edge colors

Inherit the [shared contract](COM_common_contract.md) and [KEY contract](KEY_keying_contract.md). The proposed basic tile operator is `keying.edge_extend_nearest`.

## Interface

`image: T[H,W,C]`, `opaque_threshold: T[1]` -> `image: T[H,W,C]`.

Select straight RGB and alpha. 0<opaque_threshold<=1, suggested 1. Required radius:Int64 0..4096 in pixels.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

### Static parameters

Raw selection uses `axis:Int64` and `components:String`. The latter is the comma-separated canonical decimal encoding of the ordered COM component-index list, for example `"0,1,2"`: exactly three distinct in-range indices in RGB or Lab coordinate order. Alternatively, `input_description:String` and `group:String` resolve the selection through the existing FMT description rules; this form excludes `axis` and `components`. A declared spatial channel axis must agree with the resolved selection.

The current implementation resolves an explicit common linear RGB interpretation using `source_basis:String`, or the primaries and white in the selected description group. An explicitly supplied basis must agree with the description. There is no implicit basis and no color conversion; preset names and custom values use the existing FMT basis codec. Description groups use model `rgb`, transfer `linear`, and straight association.

The resolved color sample rank is three; removing the component axis yields `[H,W]`. The component extent is three except for edge extension, which also carries alpha and optional other channels.

`radius:Int64` is required and has the declared caller domain 0..4096.

Raw selection additionally requires `alpha_index:Int64`, in range and distinct from RGB. Description selection resolves the group alpha reference and excludes an additional alpha_index.

`max_work:Int64=9223372036854775807` is the nonnegative per-invocation operator-work limit. Run work admission and cancellation remain effective in addition to this limit.

## Mathematics

Where alpha<threshold choose an original donor with alpha>=threshold in the integer disk dx^2+dy^2<=radius^2, minimizing (distance^2,y,x). Copy donor RGB and retain target alpha. With no donor, copy target. Original pixels above threshold copy.

### Current implementation profile

The implementation constructs donors from original alpha values, computes the exact nearest donor with the declared distance and row-major ordering, and applies the integer squared-radius test. For eligible packed RGBA images with radius at most eight, 64-row output bands include a full vertical radius of original donor flags on either side. Uniform bands copy the source. When all nonempty flag rows have the same donor columns, nearest row and column are selected independently and the squared-radius test applies to their intersection. For the remaining bands, from radius two, a donor found in the centered 5×5 square is globally nearest because the maximum squared distance inside is eight and the minimum outside is nine. The selected donor must still lie inside the requested disk. Empty squares search the remaining disk in distance and coordinate order; sparse bands can fall back to the exact distance transform over the band and its halo. A fully certified nearest-donor row records a conservative integer radius covering every selected distance; the radius comparison may be shared by that row only when the requested radius covers the certificate. Other rows retain the per-pixel integer disk test. It copies only donor RGB; target alpha and other channels retain their original bits. Radius zero or absence of a donor preserves the target.

## Behavior

H(radius) support on original colors and alpha. Donors never include newly extended samples. Other channels copy target.

## Acceptance

Radius zero is identity. Extending opaque red into transparent neighbors changes their hidden RGB while retaining their alpha.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
