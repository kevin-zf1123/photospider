---
spec_schema_version: 1
id: KEY-02A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: KEY-02
function: lab76_chroma_key
execution: tile
proposed_operation_keys:
  - keying.lab76_chroma_key
---

# KEY-02A: CIELAB full-color distance key

Inherit the [shared contract](COM_common_contract.md) and [KEY contract](KEY_keying_contract.md). The proposed basic tile operator is `keying.lab76_chroma_key`.

## Interface

`color: T[H,W,3]`, `key_color: T[3]`, `inner: T[1]`, `outer: T[1]` -> `matte: T[H,W]`.

Native CIELAB with the same explicit white; l=L*/100. 0<=inner<outer in DeltaE76 units.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

### Static parameters

Raw selection uses `axis:Int64` and `components:String`. The latter is the comma-separated canonical decimal encoding of the ordered COM component-index list, for example `"0,1,2"`: exactly three distinct in-range indices in RGB or Lab coordinate order. Alternatively, `input_description:String` and `group:String` resolve the selection through the existing FMT description rules; this form excludes `axis` and `components`. A declared spatial channel axis must agree with the resolved selection.

Resolve the explicit Lab white through `source_white:String`, or the selected `cielab` description group. An explicit white must agree with the description. Values use the existing FMT white codec; there is no implicit white or conversion. The key color always has fixed native `(l,a,b)` coordinate order.

The resolved color sample rank is three; removing the component axis yields `[H,W]`. The component extent is three except for edge extension, which also carries alpha and optional other channels.

`max_work:Int64=9223372036854775807` is the nonnegative per-invocation operator-work limit. Run work admission and cancellation remain effective in addition to this limit.

## Mathematics

Compute d=sqrt((100*(l-l_key))^2+(a-a_key)^2+(b-b_key)^2) and publish d to T. Return h(inner,outer,d), using the stored d and KEY-01 smoothstep. Near-key colors yield 0; distant colors yield 1.

### Current implementation profile

The implementation forms the native-T coordinate differences, including the factor 100 on native lightness, and rounds the Euclidean distance to T before smoothstep evaluation.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Each distance uses the full Lab tuple.

## Acceptance

The key color gives 0. At distance>=outer output 1. A change confined to l contributes 100 times the normalized-l difference.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
