---
spec_schema_version: 1
id: GRD-06
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8_9
clarification_status: complete
---

# GRD-06: Feature-to-color gradient mapping

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. A/B are separately callable tile operators. C defines a composition and a fixed binary realization with independent proposed registrations and shared observable behavior.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-06A](GRD-06A_gradient_map.md) | Map a feature through an RGB ramp | `grade.gradient_map` |
| [GRD-06B](GRD-06B_gradient_map_image.md) | Image gradient mapping | `grade.gradient_map_image` |
| [GRD-06C](GRD-06C_gradient_map_channels.md) | Generate a model-space image through independent channel LUTs | `grade.gradient_map_channels_combined`, `grade.gradient_map_channels_compiled` |

## Shared mathematics and behavior

A/B map one feature to three RGB values with shared color stops. C selects a raw input channel, bakes independent destination-component curves into a common-axis LUT, and generates a new image in an explicitly selected color model. C defines its own interpolation choices, dtype boundaries and unwrapped floating hue rules. Alpha ramps and effect mixing are independently composed.

## References and acceptance

Member pages define analytic cases and their input/output domains. A/B use the [strict oracle](../../../../oracle/ops/07-grade/README.md) with the common native floating criteria. C defines staged CRV/NUM reference equations and equivalence acceptance for its two registrations. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
