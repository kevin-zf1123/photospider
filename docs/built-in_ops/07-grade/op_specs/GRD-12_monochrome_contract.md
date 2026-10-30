---
spec_schema_version: 1
id: GRD-12
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-12: Artistic monochrome

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-12A](GRD-12A_monochrome_weights.md) | Artistic monochrome feature | `grade.monochrome_weights` |
| [GRD-12B](GRD-12B_monochrome_linear_y.md) | Linear-Y monochrome | `grade.monochrome_linear_y` |
| [GRD-12C](GRD-12C_monochrome_cielab.md) | CIELAB monochrome | `grade.monochrome_cielab` |
| [GRD-12D](GRD-12D_monochrome_oklab.md) | OKLab monochrome | `grade.monochrome_oklab` |

## Shared mathematics and behavior

A produces an artistic feature only. B/C/D return complete neutral RGB groups through linear Y, CIELAB or OKLab; alpha and other slots remain intact. Perceptual lightness is converted back through its inverse model before it becomes linear RGB.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
