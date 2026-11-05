---
spec_schema_version: 1
id: GRD-30
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# GRD-30: AM dot/line screens and color separations

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-30A](GRD-30A_halftone_dot.md) | Circular-dot halftone | `grade.halftone_dot` |
| [GRD-30B](GRD-30B_halftone_line.md) | Line-screen halftone | `grade.halftone_line` |
| [GRD-30C](GRD-30C_halftone_cmyk.md) | CMYK separation screens | `grade.halftone_cmyk` |
| [GRD-30D](GRD-30D_halftone_colorize.md) | Artistic color composition of ink masks | `grade.halftone_colorize` |

## Shared mathematics and behavior

A/B produce sampled ink coverage; C produces four independently screened CMYK ink planes; D combines supplied ink masks and RGB colors by ordered artistic coverage blending. Grid geometry and subpixel sampling are explicit. CMYK separation, physical paper/ink behavior and bit-packed file output are separate from these members.

## Current parameter encoding

Color selectors use `axis:Int64` with ordered, distinct `components:String` indices, or `input_description:String` with `group:String`. The axis is relative to the cell descriptor before any batch prefix. The shared work parameter is `max_work:Int64`, default 9223372036854775807. See [static selector encoding](GRD-21_saturation_contract.md#static-parameter-encoding).

Fixed static arrays use comma-separated String values in the declared row-major order, without brackets. Float64 entries use decimal numeric text; Int64 entries use signed decimal integers. Their declared lengths are checked during static preparation. Dynamic tensors in member signatures remain required ordered input ports.

A/B consume T[H,W] without color selectors or a batch prefix. C consumes one CMYK image with an arbitrary selected component axis.

| Member | Parameter | Current type and default |
| --- | --- | --- |
| A/B/C | origin_x / origin_y | Int64 / Int64, 0 / 0; global image origin |
| A/B/C | samples_per_axis | Int64, 4; legal range 1..16 |
| A/B/C | edge | String, reject; reject or clamp |
| A/B | basis | String, required four Float64 values |
| A/B | origin | String, required two Float64 values |
| C | screen | String, required four dot/line entries |
| C | bases | String, required sixteen Float64 values |
| C | origins | String, required eight Float64 values |

The basis columns are bx and by; row-major encoding is bx.x,by.x,bx.y,by.y. A/B screen origin is x,y. C repeats those encodings once per selected C/M/Y/K component. Raw C selects four ordered components and model:String=cmyk. Semantic C selects a complete CMYK group, including an ICC-native profile description; ink units are empty, 1 or relative. These inputs are already separated, and no RGB-to-CMYK conversion is performed.

D has axis:Int64 (default 2), required output_description:String and required group:String, together with max_work. The destination description resolves a complete linear RGB group with component indices 0,1,2, the output component axis, and a valid RGB basis. Colors and background remain dynamic Float64 inputs in that basis.

## Current native prototype

The screen forms its inverse basis and subpixel ranks in Float64. Exactly periodic geometry may use a finite phase table after proving its period. It separates the screen origin into an integer phase and a fractional offset, reduces the relative integer coordinate modulo the period, and then applies that offset. Ranks count only values strictly below coverage. The nonperiodic path rounds the difference between the exact integer coordinate and the Float64 screen origin once before adding subpixel offsets and applying the inverse basis. Zero/full coverage use the stated endpoint branches. D evaluates layers in ascending order, preserves zero-coverage state and selects the exact supplied color for a full-coverage layer.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
