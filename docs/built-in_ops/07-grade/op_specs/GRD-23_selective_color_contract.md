---
spec_schema_version: 1
id: GRD-23
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
---

# GRD-23: Soft color-region selection and correction

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-23A](GRD-23A_select_color_region.md) | Soft chromatic-region selection | `grade.select_color_region` |
| [GRD-23B](GRD-23B_selective_opponent_adjust.md) | Adjust selected opponent colors | `grade.selective_opponent_adjust` |
| [GRD-23C](GRD-23C_select_neutral_region.md) | Soft neutral-region selection | `grade.select_neutral_region` |

## Static parameter encoding

Color selectors and `max_work` use the [GRD-21 parameter encoding](GRD-21_saturation_contract.md#static-parameter-encoding). GRD-23B additionally accepts `overlap:String=normalize|sum`, default `normalize`. Hue units for GRD-23A may be resolved from the selected description; an explicit `hue_unit` asserts agreement. Raw polar coordinates require explicit `hue_unit=pi_multiple`.

## Shared mathematics and behavior

A selects chromatic regions; C selects low-chroma neutral regions including black, gray and white. B applies supplied selections to original opponent coordinates. All masks use the same unmodified image, so overlapping edits do not feed back into selection.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
