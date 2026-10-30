---
spec_schema_version: 1
id: GRD-28
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-28: Two-dimensional color-coordinate warps

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-28A](GRD-28A_color_warper_ab.md) | Opponent-plane color warp | `grade.color_warper_ab` |
| [GRD-28B](GRD-28B_color_warper_hc.md) | Periodic hue-chroma warp | `grade.color_warper_hc` |
| [GRD-28C](GRD-28C_color_warper_cl.md) | Chroma-lightness warp | `grade.color_warper_cl` |

## Shared mathematics and behavior

Bilinear mesh displacement is added to the original coordinates. edge=clamp holds the boundary displacement for out-of-grid lookups rather than clamping the source color itself. Axis/domain and mapped nonnegative-chroma constraints are unchecked caller premises.

For topology=positive_jacobian, define the map F(q)=q+d(q) in each cell's physical coordinate units. Require det(dF/dq)>0 at all four corners of every cell. A bilinear displacement map has a determinant affine in the cell coordinates, so these corner conditions guarantee positive local orientation inside that cell. The periodic hue member includes the closing cell in an unwrapped hue coordinate. This is an input-mesh premise, not a required runtime validator. topology=allow is the default and permits local folding. Neither mode asserts global one-to-one mapping or an inverse operator.


## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
