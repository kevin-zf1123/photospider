---
spec_schema_version: 1
id: PTH-06A
parent_id: PTH-06
function: transform_centerline
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.transform_centerline_v1_strict
---

# PTH-06A: transform centerline

Inherits [PTH-06](PTH-06_transforms_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.transform_centerline_v1_strict`.

PathSet,finite affine[2,3]; same-authority transformed PathSet where representable.

### Parameters and domain

attribute_policy; width_policy=preserve_px. The caller guarantees that the selected authority can represent the transformed geometry, including any degeneration, and that required rounded coordinates and arc axes remain in its domain.

Accepts CoreVerbs and Primitives PathSets. Points and vector controls are transformed according to their authority; an identity transform preserves the input bits. Associated width and mapping Results remain explicit workflow inputs and are not geometry attributes.

## Mathematics

Control points RN64(Ap+b), derivatives/arc axes RN64(Av). Core/Bezier/Hermite can degenerate within their representation domain. Arc axes must remain representable under the caller premises; explicitly flatten an arc before applying a transform that would leave that domain. No runtime singularity scan or failure is required. Splines retain knots/weights. Identity copies bits. Non-similarity arc-domain bindings require valid remapping or reject/drop; width numeric px value remains unchanged.

## Behavior

Support class: complete input. Representable output connections are a caller premise.

## Acceptance

Identity signed zeros, affine line/Bezier, representable nonuniform transforms and legal Core/Bezier/Hermite degeneration.
