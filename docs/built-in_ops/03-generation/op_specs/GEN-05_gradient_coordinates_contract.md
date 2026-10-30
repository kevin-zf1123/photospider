---
spec_schema_version: 1
id: GEN-05
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# GEN-05: Gradient coordinates

Coordinates are dimensionless numeric fields; spread and color lookup are separate. Pixel positions use global centers. Distinct finite linear endpoints and finite positive radii are unchecked caller premises; no SVG fallback or numerical-domain diagnostic is implied. Angular/repeat final rounding keeps an upper endpoint rounded to 1. Two-circle validity is separate from alpha.

## Interface

GEN-05A through E and G are primitive operations, each with `strict`, `accelerated_apple_silicon` and `accelerated_x86_64` keys. GEN-05F is the public authoring helper `ps::generation::path_distance_coordinate`, which composes PTH-11 distance, scale broadcast and NUM division.

Each ordered input is a Result with one Float32 or Float64 tensor and no batch axes; input dtypes may differ. Controls have the exact shapes in the member spec. The named output `t` is `photospider.tensor` v1 / `samples` with shape `[height,width]`, spatial axes 0/1, no color description and an independently selected dtype. GEN-05G also publishes UInt8 `valid[height,width]`.

| Static parameter | Type | Default / meaning |
| --- | --- | --- |
| `height`, `width` | Int64 | Required positive extents; product at most 2^40 |
| `origin_x`, `origin_y` | Int64 | 0; exact global pixel-center origin |
| `dtype` | String | `float64`; alternatively `float32` |
| `center_policy` (GEN-05C only) | String | `zero`, the only admitted policy |
| `max_refinements` (GEN-05C and G only) | Int64 | 4096; full positive Int64 refinement cap |

Schema, dtype, parameter type and enum mismatches are structural failures; finite geometry and numerical domains are unchecked.

## Mathematics

For GEN-05A through E, the complete exact expression is evaluated before destination rounding. Accelerated Float64 uses the [NUM final FP32 four-ULP contract](../../01-numeric/op_specs/NUM_accelerated_contract.md) with strict behavior outside its admitted range. Exact zeros, discrete branches and copy paths keep their exact semantics. GEN-05C exhausting its refinement allowance without a rounding certificate returns `OperationFailed` with generation domain code `0x47454e0001`. GEN-05G uses certified exact quadratic/radical selection and rounding in all three profiles, as specified by [GEN-05G](GEN-05G_two_circle_radial_coordinate.md); its no-solution policy does not absorb arithmetic or resource failures.

## Members

| ID | Specialization |
| --- | --- |
| [GEN-05A](GEN-05A_linear_gradient_coordinate.md) | linear gradient coordinate |
| [GEN-05B](GEN-05B_radial_gradient_coordinate.md) | radial gradient coordinate |
| [GEN-05C](GEN-05C_angular_gradient_coordinate.md) | angular gradient coordinate |
| [GEN-05D](GEN-05D_diamond_gradient_coordinate.md) | diamond gradient coordinate |
| [GEN-05E](GEN-05E_box_gradient_coordinate.md) | box gradient coordinate |
| [GEN-05F](GEN-05F_path_distance_coordinate.md) | path distance coordinate |
| [GEN-05G](GEN-05G_two_circle_radial_coordinate.md) | two circle radial coordinate |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md).
