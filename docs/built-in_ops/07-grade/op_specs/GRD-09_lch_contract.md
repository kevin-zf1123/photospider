---
spec_schema_version: 1
id: GRD-09
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-09: CIELCh and OKLCh adjustments

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-09A](GRD-09A_cielch.md) | CIELCh adjustment | `grade.cielch` |
| [GRD-09B](GRD-09B_oklch.md) | OKLCh adjustment | `grade.oklch` |

## Shared mathematics and behavior

CIELCh and OKLCh retain different chroma scales. Gamut mapping is a separate operation and may change the model components after adjustment.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
