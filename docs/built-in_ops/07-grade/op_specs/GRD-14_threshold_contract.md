---
spec_schema_version: 1
id: GRD-14
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-14: Hard and local-mean threshold

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-14A](GRD-14A_threshold.md) | Fixed threshold | `grade.threshold` |
| [GRD-14B](GRD-14B_threshold_local_mean.md) | Local-mean threshold | `grade.threshold_local_mean` |
| [GRD-14C](GRD-14C_threshold_local_gaussian.md) | Gaussian-weighted local threshold | `grade.threshold_local_gaussian` |
| [GRD-14D](GRD-14D_threshold_sauvola.md) | Sauvola local threshold | `grade.threshold_sauvola` |

## Shared mathematics and behavior

A is a point comparison. B/C/D use cropped square neighborhoods. A mask value of one means the source is at least the local threshold; callers invert the binary mask if dark foreground is desired. Gaussian and Sauvola definitions use explicit finite radius.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
