---
spec_schema_version: 1
id: GRD-26
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-26: Coordinate-driven vignette

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-26A](GRD-26A_vignette_ev.md) | Generate an elliptical vignette EV field | `grade.vignette_ev` |
| [GRD-26B](GRD-26B_vignette.md) | Apply an elliptical vignette | `grade.vignette` |

## Shared mathematics and behavior

The explicit coordinate field fixes origin and units. Pixel coordinates use global pixel centers; image resizing changes the pattern only through the caller-supplied field or geometry. Positive edge EV is legal. Center/feather controls have the stated ellipse/smoothstep meaning.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
