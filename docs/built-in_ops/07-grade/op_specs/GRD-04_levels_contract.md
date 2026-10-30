---
spec_schema_version: 1
id: GRD-04
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-04: Bounded and signed levels

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-04A](GRD-04A_levels_bounded.md) | Bounded levels | `grade.levels_bounded` |
| [GRD-04B](GRD-04B_levels_signed.md) | Signed levels | `grade.levels_signed` |

## Shared mathematics and behavior

Bounded levels clamps the normalized input. Signed levels retains extensions using signed power. black<white is a caller premise; no epsilon modifies their separation.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
