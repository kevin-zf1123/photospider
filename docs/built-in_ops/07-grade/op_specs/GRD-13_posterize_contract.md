---
spec_schema_version: 1
id: GRD-13
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-13: Uniform posterization

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-13A](GRD-13A_posterize.md) | Uniform posterization | `grade.posterize` |

## Shared mathematics and behavior

Uniform quantization returns floating reconstruction levels. Dither is GRD-29; storage integer conversion and bit packing are separate operations.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
