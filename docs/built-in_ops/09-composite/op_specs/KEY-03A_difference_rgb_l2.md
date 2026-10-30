---
spec_schema_version: 1
id: KEY-03A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
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

## Mathematics

Publish d=sqrt(sum_c(image_c-plate_c)^2) to T. Return h(inner,outer,d) from KEY-01 using the stored distance.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Each distance uses both complete RGB tuples.

## Acceptance

Matching images give zero matte. A pure red difference 0.5 has distance 0.5.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
