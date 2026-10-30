---
spec_schema_version: 1
id: GRD-20
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-20: Feature curves and explicit target edits

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-20A](GRD-20A_feature_curve_periodic.md) | Periodic feature curve | `grade.feature_curve_periodic` |
| [GRD-20B](GRD-20B_feature_curve_linear.md) | Nonperiodic feature curve | `grade.feature_curve_linear` |
| [GRD-20C](GRD-20C_apply_hue_delta.md) | Apply a hue-increment field | `grade.apply_hue_delta` |
| [GRD-20D](GRD-20D_apply_chroma_scale.md) | Apply a chroma-scale field | `grade.apply_chroma_scale` |
| [GRD-20E](GRD-20E_apply_lightness_delta.md) | Apply a native lightness-increment field | `grade.apply_lightness_delta` |
| [GRD-20F](GRD-20F_reconstruct_luminance.md) | Set luminance by proportional RGB scaling | `grade.reconstruct_luminance` |
| [GRD-20G](GRD-20G_apply_saturation_scale.md) | Apply an HSL/HSV saturation-scale field | `grade.apply_saturation_scale` |
| [GRD-20H](GRD-20H_set_xyz_y.md) | Set XYZ Y while retaining X and Z | `grade.set_xyz_y` |
| [GRD-20I](GRD-20I_set_cielab_lightness.md) | Set CIELAB lightness while retaining a and b | `grade.set_cielab_lightness` |
| [GRD-20J](GRD-20J_set_oklab_lightness.md) | Set OKLab lightness while retaining a and b | `grade.set_oklab_lightness` |

## Shared mathematics and behavior

Periodic and nonperiodic source curves produce modulation with an explicit static unit: hue increment, chroma or saturation multiplier, model-lightness increment, or EV. C/D/E/G apply these fields; EV feeds GRD-01B. F/H/I/J are the four independent complete RGB brightness replacement routes. Static units are passed explicitly to consumers.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
