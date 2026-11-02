---
spec_schema_version: 1
id: GRD-15
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
---

# GRD-15: Matrix channel mixing

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-15A](GRD-15A_channel_mixer.md) | Matrix channel mixer | `grade.channel_mixer` |

## Shared mathematics and behavior

The matrix output has exactly D new components. Complete color meaning is explicitly supplied downstream. Alpha is preserved only by an explicit selected row or assembly.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
