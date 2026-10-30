---
spec_schema_version: 1
id: GRD-05
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-05: Component curves and luminance workflow

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-05A](GRD-05A_curves_linear.md) | Component curves with linear interpolation | `grade.curves_linear` |
| [GRD-05B](GRD-05B_curves_pchip.md) | Component curves with PCHIP interpolation | `grade.curves_pchip` |
| [GRD-05C](GRD-05C_curves_luminance.md) | Proportional RGB luminance curve | `grade.curves_luminance` |
| [GRD-05D](GRD-05D_curves_xyz_y.md) | XYZ Y curve | `grade.curves_xyz_y` |
| [GRD-05E](GRD-05E_curves_cielab_l.md) | CIELAB lightness curve | `grade.curves_cielab_l` |
| [GRD-05F](GRD-05F_curves_oklab_l.md) | OKLab lightness curve | `grade.curves_oklab_l` |

## Shared mathematics and behavior

A/B operate on selected component values. C/D/E/F are separate complete RGB brightness operators for proportional RGB, XYZ Y, CIELAB l and OKLab L. Master-before-channel and master-after-channel curves are explicit distinct compositions.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
