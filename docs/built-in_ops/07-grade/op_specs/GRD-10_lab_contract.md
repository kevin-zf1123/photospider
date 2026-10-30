---
spec_schema_version: 1
id: GRD-10
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-10: Cartesian opponent-color adjustments

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-10A](GRD-10A_lab.md) | CIELAB affine adjustment | `grade.lab` |
| [GRD-10B](GRD-10B_oklab.md) | OKLab affine adjustment | `grade.oklab` |

## Shared mathematics and behavior

Each coordinate is adjusted independently. A request for lightness alone has no opponent-coordinate sample dependency.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
