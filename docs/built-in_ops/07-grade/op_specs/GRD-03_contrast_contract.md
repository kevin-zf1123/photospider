---
spec_schema_version: 1
id: GRD-03
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-03: Linear and log-axis contrast

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-03A](GRD-03A_contrast_linear.md) | Linear contrast around a pivot | `grade.contrast_linear` |
| [GRD-03B](GRD-03B_contrast_log2.md) | Contrast on the exposure axis | `grade.contrast_log2` |

## Shared mathematics and behavior

The pivot uses source units. The 0.18 suggested value applies naturally to relative linear-light data; encoded coordinates require a caller-selected pivot.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
