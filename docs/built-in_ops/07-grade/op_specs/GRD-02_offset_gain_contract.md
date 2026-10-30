---
spec_schema_version: 1
id: GRD-02
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-02: Affine and lift/gamma/gain

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-02A](GRD-02A_offset_gain.md) | Per-component affine adjustment | `grade.offset_gain` |
| [GRD-02B](GRD-02B_lift_gamma_gain.md) | Lift, gamma and gain | `grade.lift_gamma_gain` |

## Shared mathematics and behavior

Affine gain may be negative. LGG has nonnegative gain and positive gamma; its signed power and operation order define an artistic curve.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
