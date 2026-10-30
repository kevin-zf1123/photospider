---
spec_schema_version: 1
id: GRD-07
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-07: RGB 3D LUT application

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-07A](GRD-07A_lut3d_trilinear.md) | Trilinear RGB LUT | `grade.lut3d_trilinear` |
| [GRD-07B](GRD-07B_lut3d_tetrahedral.md) | Tetrahedral RGB LUT | `grade.lut3d_tetrahedral` |

## Shared mathematics and behavior

The LUT axes are RGB in that order. Each axis is an endpoint-defined uniform grid with [start,end] controls. Interpolation methods have separate identities. Shapers and transfer functions are explicit surrounding operators.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
