---
spec_schema_version: 1
id: GRD-27
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-27: ASC CDL mathematical styles

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-27A](GRD-27A_asc_cdl_fwd.md) | ASC CDL forward with clamping | `grade.asc_cdl_fwd` |
| [GRD-27B](GRD-27B_asc_cdl_rev.md) | ASC CDL reverse with clamping | `grade.asc_cdl_rev` |
| [GRD-27C](GRD-27C_asc_cdl_fwd_no_clamp.md) | ASC CDL forward without clamping | `grade.asc_cdl_fwd_no_clamp` |
| [GRD-27D](GRD-27D_asc_cdl_rev_no_clamp.md) | ASC CDL reverse without clamping | `grade.asc_cdl_rev_no_clamp` |

## Shared mathematics and behavior

The four ASC CDL styles follow [CLF 3.0 equations](../research-sources.md#clf). Constants are the CDL luma weights, not a working-basis Y row. No-clamp styles bypass power for a negative pre-power value. Forward/reverse are independently callable; clipping is information-losing.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
