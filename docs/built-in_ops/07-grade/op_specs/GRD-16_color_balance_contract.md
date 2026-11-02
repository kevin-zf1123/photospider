---
spec_schema_version: 1
id: GRD-16
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
---

# GRD-16: Tonal color balance

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-16A](GRD-16A_color_balance_rgb.md) | Tonal RGB color balance | `grade.color_balance_rgb` |
| [GRD-16B](GRD-16B_color_balance_opponent.md) | Tonal opponent-color balance | `grade.color_balance_opponent` |

## Shared mathematics and behavior

Zone weights are supplied from GRD-18A or another explicit producer. Consumers normalize their positive sum. RGB offsets operate in source coordinates; opponent offsets retain native lightness.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
