---
spec_schema_version: 1
id: GRD-21
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
---

# GRD-21: Saturation and versioned vibrance

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-21A](GRD-21A_saturation_linear.md) | Linear-RGB saturation | `grade.saturation_linear` |
| [GRD-21B](GRD-21B_saturation_opponent.md) | Opponent-space saturation | `grade.saturation_opponent` |
| [GRD-21C](GRD-21C_vibrance_oklab.md) | OKLab vibrance | `grade.vibrance_oklab` |

## Static parameter encoding

`axis:Int64` is the raw cell component-axis index; callers do not add a batch prefix. `components:String` is an ordered CSV list of unique canonical nonnegative integers. `input_description:String` and `group:String` resolve a semantic selection and are mutually exclusive with raw `axis`/`components`. `source_basis`, `model`, `units` and `hue_unit` are String assertions. Dynamic tensors in member signatures remain runtime ports. `max_work:Int64` defaults to 9223372036854775807. Color models, units and source-basis requirements remain those of each member.

## Shared mathematics and behavior

Linear saturation moves RGB along a ray around working-basis Y. Opponent saturation scales a,b. Vibrance uses the explicitly defined low-chroma gain. Optional skin protection is an external effect mask.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
