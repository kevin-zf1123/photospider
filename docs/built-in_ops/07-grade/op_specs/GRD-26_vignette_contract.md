---
spec_schema_version: 1
id: GRD-26
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
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

## Current parameter encoding

Color selectors use `axis:Int64` with ordered, distinct `components:String` indices, or `input_description:String` with `group:String`. The axis is relative to the cell descriptor before any batch prefix. The shared work parameter is `max_work:Int64`, default 9223372036854775807. See [static selector encoding](GRD-21_saturation_contract.md#static-parameter-encoding).

Fixed static arrays use comma-separated String values in the declared row-major order, without brackets. Float64 entries use decimal numeric text; Int64 entries use signed decimal integers. Their declared lengths are checked during static preparation. Dynamic tensors in member signatures remain required ordered input ports.

| Parameter | Current type | Default / meaning |
| --- | --- | --- |
| center | String | Required two Float64 values, cx,cy |
| radii | String | Required two Float64 values, rx,ry |
| rotation_pi | Float64 | 0 |
| inner / outer | Float64 | 0 / 1 |
| center_ev / edge_ev | Float64 | 0 / -1 |

GRD-26A has no color selectors. GRD-26B selects one linear-Y Gray component or three linear RGB components. Both operate on one two-dimensional image. The coordinate input always has logical shape Float64[H,W,2] and component order x,y, independently of the source component axis. Those coordinates are already absolute; this prototype adds no image-origin parameter.

## Current native prototype

Rotation is reduced modulo two in rotation_pi units. Equal EV endpoints and the inner/outer regions select the specified endpoint values. The transition evaluates the stated smoothstep and convex interpolation of endpoint EVs in Float64. GRD-26B uses the same per-pixel Float64 EV as GRD-26A for every selected component before applying the exposure-field equation. Zero EV and unselected components preserve source bits.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
