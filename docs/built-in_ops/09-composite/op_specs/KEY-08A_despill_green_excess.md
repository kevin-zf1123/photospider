---
spec_schema_version: 1
id: KEY-08A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: KEY-08
function: despill_green_excess
execution: tile
proposed_operation_keys:
  - keying.despill_green_excess
---

# KEY-08A: Suppress green excess

Inherit the [shared contract](COM_common_contract.md) and [KEY contract](KEY_keying_contract.md). The proposed basic tile operator is `keying.despill_green_excess`.

## Interface

`foreground: T[S,C]`, `mask: T[S,1]`, `strength: T[1]` -> `foreground: T[S,C]`.

Select linear RGB and optional alpha/AOV slots. mask,strength in [0,1]; suggested strength=0.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

### Static parameters

Raw selection uses `axis:Int64` and `components:String`. The latter is the comma-separated canonical decimal encoding of the ordered COM component-index list, for example `"0,1,2"`: exactly three distinct in-range indices in RGB or Lab coordinate order. Alternatively, `input_description:String` and `group:String` resolve the selection through the existing FMT description rules; this form excludes `axis` and `components`. A declared spatial channel axis must agree with the resolved selection.

The current implementation resolves an explicit common linear RGB interpretation using `source_basis:String`, or the primaries and white in the selected description group. An explicitly supplied basis must agree with the description. There is no implicit basis and no color conversion; preset names and custom values use the existing FMT basis codec. Description groups use model `rgb`, transfer `linear`, and straight association.

The logical `[S,C]` layout uses the resolved component axis. The mask has the same sample shape with that component extent replaced by one. Other foreground components remain in place; these operators do not accept an alpha selector.

`max_work:Int64=9223372036854775807` is the nonnegative per-invocation operator-work limit. Run work admission and cancellation remain effective in addition to this limit.

## Mathematics

e=max(0,G-max(R,B)); G_out=G-strength*mask*e. Copy R,B,alpha and unrelated slots. A zero correction factor copies the source.

### Current implementation profile

The implementation evaluates the native-T factor and green excess, modifying only the selected G component. Zero correction and all unmodified components retain source bits. Packed RGB/RGBA spans have vector paths, with a general strided path for other layouts. A rank-one color tuple uses the scalar path and visits exactly one sample.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload.

## Acceptance

For RGB=(0.25,1,0.5), unit mask/strength gives G=0.5. Zero strength is identity.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
