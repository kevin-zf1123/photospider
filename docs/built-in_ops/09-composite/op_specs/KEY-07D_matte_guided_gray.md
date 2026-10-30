---
spec_schema_version: 1
id: KEY-07D
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
parent_id: KEY-07
function: matte_guided_gray
execution: tile
proposed_operation_keys:
  - keying.matte_guided_gray
---

# KEY-07D: Gray-guided matte refinement

Inherit the [shared contract](COM_common_contract.md) and [KEY contract](KEY_keying_contract.md). The proposed basic tile operator is `keying.matte_guided_gray`.

## Interface

`matte: T[H,W]`, `guide: T[H,W]`, `epsilon: T[1]` -> `matte: T[H,W]`.

Matte in [0,1]; finite guide in explicit scalar units. Required radius:Int64 0..4096. epsilon>0 in squared guide units.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

In each cropped square window compute means mg,mp, population variance vg and covariance cov(g,p). Set a=cov/(vg+epsilon), b=mp-a*mg. At p average a,b over its cropped square window and return clamp(mean(a)*guide_p+mean(b),0,1). Radius zero copies matte.

## Behavior

H(2*radius) support from the two window stages. Means and coefficients are Float64 numerical intermediates; the strict oracle evaluates the complete mathematical expression before final output rounding.

## Acceptance

Radius zero copies. A constant matte remains constant within floating tolerance. Use a nonuniform guide to check edge-aware behavior.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
