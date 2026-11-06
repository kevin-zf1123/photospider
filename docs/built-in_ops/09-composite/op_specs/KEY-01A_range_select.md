---
spec_schema_version: 1
id: KEY-01A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: KEY-01
function: range_select
execution: tile
proposed_operation_keys:
  - keying.range_select
---

# KEY-01A: Soft scalar range selection

Inherit the [shared contract](COM_common_contract.md) and [KEY contract](KEY_keying_contract.md). The proposed basic tile operator is `keying.range_select`.

## Interface

`signal: T[H,W]`, `low: T[1]`, `high: T[1]`, `softness: T[1]` -> `matte: T[H,W]`.

low<high, softness>=0 in the signal unit. Static invert:Bool=false. Controls are required; suggested softness=0.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

### Static parameters

`invert:Bool=false` selects the stated complementary matte. Scalar tensor controls remain required inputs.

`max_work:Int64=9223372036854775807` is the nonnegative per-invocation operator-work limit. Run work admission and cancellation remain effective in addition to this limit.

## Mathematics

For softness=0 return 1 iff low<=signal<=high, otherwise 0. For w=softness>0 return h(low-w,low,signal)*(1-h(high,high+w,signal)), where h(a,b,x)=smooth(clamp((x-a)/(b-a),0,1)) and smooth(t)=t*t*(3-2*t). invert returns one minus this value.

### Current implementation profile

Zero softness uses the inclusive hard interval. For positive softness the implementation computes the distance outside the plateau, divides it by the supplied width and applies the complementary smoothstep. This keeps the inner endpoints at one even when adding the width to an endpoint would round back to that endpoint. Packed spans and strided scalar views use the same scalar rounding boundaries.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload.

## Acceptance

At zero softness both interval endpoints are included. At positive softness the outer endpoints give 0 and the inner plateau gives 1.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
