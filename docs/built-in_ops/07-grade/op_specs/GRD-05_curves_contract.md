---
spec_schema_version: 1
id: GRD-05
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
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

For C–F, a described RGB group uses `transfer=linear`, reference `scene`, `display`, `scene_relative` or `display_relative`, and component units `1` or `relative`. A supplied nonempty `coordinates.scale` must be `relative`. Absolute units/scales and missing or unsupported references fail static preparation with TypeMismatch. Raw calls provide the ordered RGB slots and `source_basis` explicitly.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.

## Native fast numerical profile

All six `grade.curves_*` keys use native floating arithmetic. A/B follow the CRV-01 fast interpolation formulas; C–F combine them with native FMT matrix/model mathematics. Strict rounding and certified accelerated error bounds do not apply. Curve construction and all brightness features/mapped features use binary64. The output is converted once to the input dtype after reconstruction; selected copies retain their original bits.

A/B prepare per-selected-component linear or PCHIP coefficients from the current knot/ordinate payload. PCHIP uses scaled weighted harmonic slopes, limited endpoints and Horner FMA as described by [CRV-01 fast](../../01-numeric/op_specs/CRV-01_interpolate.md#native-fast-numerical-profile). Exact knots/clamps select the ordinate, and linear extrapolation uses the endpoint tangent.

Let `M` be the source RGB-to-XYZ matrix and `Y=(M*rgb)[1]`. C maps Y and reconstructs `rgb*(Y'/Y)`; at Y=0 its defined neutral result is `(Y',Y',Y')`. D preserves X and Z mathematically and evaluates `rgb + M^(-1)[:,1]*(Y'-Y)`. E converts XYZ to normalized CIELAB under the explicit source white, replaces l, and converts back. These are ordinary native formulas with binary64 feature and mapped-feature boundaries.

For F, let `A` be Bradford adaptation from the source white to D65, `U` the XYZ-to-LMS matrix, and `V` the LMS-root-to-OKLab matrix. Precompute `P=U*A*M`, `Q=M^(-1)*A^(-1)*U^(-1)` and `v=V^(-1)[:,0]`. Evaluate `r=cbrt(P*rgb)`, `L=(V*r)[0]`, `L'=curve(L)`, and `t=v*(L'-L)`. The equivalent reconstruction uses component-wise `d=t*(3*r*r+3*r*t+t*t)` followed by `rgb'=rgb+Q*d`. It preserves a/b mathematically while avoiding subtraction of nearby cube values. Native matrix composition/FMA may differ in final bits from an explicit multi-node composition. Cube roots use the native FMT fast algorithm.

Every brightness route checks equality of the mapped and original feature before reconstruction and then copies the original RGB bits. Unselected components and alpha always use bit copies. Curve outputs depend on the control tables supplied to the current Run.

| Static parameter | Contract |
| --- | --- |
| `axis`, `components` | Raw selected axis and ordered components for A/B; C–F require an ordered RGB triple and explicit basis. |
| `input_description`, `group` | Explicit semantic selection; C–F require relative linear RGB and a resolved source basis. |
| `source_basis` | C–F raw basis, or a consistency assertion for the described basis. |
| `edge` | `reject` (default), `clamp`, `linear_extrapolate`. |
| `method` | Required `linear` or `pchip` for C–F; A/B select their method by operation key. |

The native profile uses the GRD common comparison criteria, FP32 `2e-6 + 2e-5*abs(reference)` and FP64 `2e-12 + 2e-11*abs(reference)`, for its stated finite numerical domain. It does not add an exact-rounding or exceptional-value guarantee to computed results. Static domain checks, empty/bypass dependency pruning, bounded work and cancellation remain active.
