---
spec_schema_version: 1
id: KEY-03A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: KEY-03
function: difference_rgb_l2
execution: tile
proposed_operation_keys:
  - keying.difference_rgb_l2
---

# KEY-03A: RGB difference key

Inherit the [shared contract](COM_common_contract.md) and [KEY contract](KEY_keying_contract.md). The proposed basic tile operator is `keying.difference_rgb_l2`.

## Interface

`image: T[H,W,3]`, `plate: T[H,W,3]`, `inner: T[1]`, `outer: T[1]` -> `matte: T[H,W]`.

Matching relative linear RGB, already spatially aligned. 0<=inner<outer in RGB coordinate units.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

### Static parameters

Raw selection uses `axis:Int64` and `components:String`. The latter is the comma-separated canonical decimal encoding of the ordered COM component-index list, for example `"0,1,2"`: exactly three distinct in-range indices in RGB or Lab coordinate order. Alternatively, `input_description:String` and `group:String` resolve the selection through the existing FMT description rules; this form excludes `axis` and `components`. A declared spatial channel axis must agree with the resolved selection.

The current implementation resolves an explicit common linear RGB interpretation using `source_basis:String`, or the primaries and white in the selected description group. An explicitly supplied basis must agree with the description. There is no implicit basis and no color conversion; preset names and custom values use the existing FMT basis codec. Description groups use model `rgb`, transfer `linear`, and straight association.

The resolved color sample rank is three; removing the component axis yields `[H,W]`. The component extent is three except for edge extension, which also carries alpha and optional other channels.

`max_work:Int64=9223372036854775807` is the nonnegative per-invocation operator-work limit. Run work admission and cancellation remain effective in addition to this limit.

## Mathematics

Publish d=sqrt(sum_c(image_c-plate_c)^2) to T. Return h(inner,outer,d) from KEY-01 using the stored distance.

### Current implementation profile

The implementation forms the native-T RGB squared distance, rounds its square root to T, and then evaluates the declared smoothstep. Both images use the same resolved RGB interpretation.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Each distance uses both complete RGB tuples.

## Acceptance

Matching images give zero matte. A pure red difference 0.5 has distance 0.5.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
