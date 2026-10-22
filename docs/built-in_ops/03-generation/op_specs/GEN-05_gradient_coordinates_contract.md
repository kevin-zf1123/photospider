---
spec_schema_version: 1
id: GEN-05
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: partially_implemented
---

# GEN-05: Gradient coordinates

Coordinates are dimensionless numeric fields; spread and color lookup are separate.
Pixel positions use global centers. Distinct finite linear endpoints and finite positive radii are unchecked caller
premises; no SVG fallback or numerical-domain diagnostic is implied. Angular/repeat final
rounding keeps an upper endpoint rounded to 1. Two-circle validity is separate from
alpha.

## Registered CPU interface for GEN-05A through E

A through E are implemented; F and G retain their separate unimplemented status.
Each registered member has `strict`, `accelerated_apple_silicon`, and
`accelerated_x86_64` keys. Strict runs on supported CPU hosts; the accelerated
profiles require their named architecture. These members do not register a GPU key.

Each ordered input is a Result with one Float32 or Float64 tensor and no batch
axes; input dtypes may differ. Controls have the exact shapes in the member spec.
The named output `t` is `photospider.tensor` v1 / `samples`, shape `[height,width]`,
with spatial axes 0/1, no color description and dtype selected independently.

| Static parameter | Type | Default / meaning |
| --- | --- | --- |
| `height`, `width` | Int64 | Required positive extents; product at most 2^40 |
| `origin_x`, `origin_y` | Int64 | 0; exact global pixel-center origin |
| `dtype` | String | `float64`; alternatively `float32` |
| `max_work` | Int64 | INT64_MAX; per-invocation operational work cap |
| `center_policy` (C only) | String | `zero`, the only admitted policy |
| `max_refinements` (C only) | Int64 | 4096; full positive Int64 refinement cap |

Strict evaluates the complete exact expression before destination rounding.
The Float32 interval path also proves correct rounding; accelerated Float64
uses the [NUM final FP32 four-ULP contract](../../01-numeric/op_specs/NUM_accelerated_contract.md)
with strict fallback outside its admitted range. Exact zeros, discrete branches
and copy paths retain their exact semantics. A failed work/cancellation check is
propagated, never converted into a fallback result.

There is one fixed Host CPU invocation per selected output tile. It reads all
control tiles on each input's own grid, writes that complete retained output tile,
and uses admitted fixed arithmetic scratch. Empty requests perform static checks
without sample reads. Schema, dtype, parameter type and enum mismatches are
structural failures; finite geometry and numerical domains are unchecked.
Resource capacity and work exhaustion remain `ResourceExhausted`, cancellation
remains `Cancelled`. C uses 128..4096-bit directed refinement; exhausting its
refinement allowance without a rounding certificate returns `OperationFailed`
with generation domain code `0x47454e0001`.

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

## Shared execution and numerical rules

Inherit [GEN common](GEN_common_contract.md), and applicable
[random](NOI_random_contract.md) and [path geometry](PTH_geometry_contract.md) rules.
Members declare Regional, Halo or complete-input from actual mathematical dependencies,
independently of static structure checks. GPU capability is member/backend-specific.
Same version/profile means bit-identical output; numerical error, geometry error and
topology guarantees are separate. Source associations are unchecked caller premises; producers preserve declared ownership and lifetime by construction.

## Acceptance and implementation boundary

Use member-specific independent fixtures plus actual public workflow execution. The
[oracle coverage](../oracle-coverage.md) identifies finite exact helpers, Measured
diagnostics and remaining unsupported cases. It cannot establish public schema support,
GPU admission or complete domain certification. Other member/backend limitations remain explicit; CPU validation does not establish GPU or cross-architecture execution coverage.
