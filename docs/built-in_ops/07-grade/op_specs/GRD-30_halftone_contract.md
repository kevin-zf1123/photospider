---
spec_schema_version: 1
id: GRD-30
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-30: AM dot/line screens and color separations

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-30A](GRD-30A_halftone_dot.md) | Circular-dot halftone | `grade.halftone_dot` |
| [GRD-30B](GRD-30B_halftone_line.md) | Line-screen halftone | `grade.halftone_line` |
| [GRD-30C](GRD-30C_halftone_cmyk.md) | CMYK separation screens | `grade.halftone_cmyk` |
| [GRD-30D](GRD-30D_halftone_colorize.md) | Artistic color composition of ink masks | `grade.halftone_colorize` |

## Shared mathematics and behavior

A/B produce sampled ink coverage; C produces four independently screened CMYK ink planes; D combines supplied ink masks and RGB colors by ordered artistic coverage blending. Grid geometry and subpixel sampling are explicit. CMYK separation, physical paper/ink behavior and bit-packed file output are separate from these members.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
