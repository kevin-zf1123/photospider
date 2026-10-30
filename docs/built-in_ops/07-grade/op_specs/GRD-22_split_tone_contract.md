---
spec_schema_version: 1
id: GRD-22
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-22: Split tones and color wheels

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-22A](GRD-22A_split_tone_opponent.md) | Tonal opponent-color tinting | `grade.split_tone_opponent` |
| [GRD-22B](GRD-22B_color_wheels_rgb.md) | RGB tonal wheels | `grade.color_wheels_rgb` |

## Shared mathematics and behavior

Split tone adds opponent chroma and can tint gray. RGB wheels combine per-zone/component EV and offset. LGG controls use GRD-02B and are not implicit wheel modes.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
