---
spec_schema_version: 1
id: GEN-06
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# GEN-06: Spread and gradient lookup

Spread, uniform raw-table lookup and model-specific color workflows are distinct.
Preserve each public node rounding stage. Color ramps retain original hue and winding,
including achromatic input; no implicit shortest-path selection or hue normalization.
Model-specific rules inherit current CRV/FMT contracts.

## Implemented interfaces

A and B each register the three CPU profile keys listed in their specs. C is the
public C++ `ps::generation::gradient_color` workflow helper; it expands to A and
one existing CRV-06 member and introduces no fused runtime operation key.
Architecture admission and numerical profiles follow [GEN-05](GEN-05_gradient_coordinates_contract.md#registered-cpu-interface-for-gen-05a-through-e).

A and B consume one-tensor Results, with independent Float32/Float64 input dtypes,
complete `sample_shape()` values including batch axes, positive extents and at
most 2^40 logical elements per tensor.
A accepts rank 1..8; B accepts query rank 1..7 and appends one atomic component axis.
Their `values` output uses `photospider.tensor` v1 / `samples` and carries no color
metadata. A preserves logical sample shape/dtype; B selects its output dtype independently.
Outputs represent the complete logical shape with explicit axes, without retaining
the input batch-axis partition.
C inherits CRV's complete sample-shape and descriptor contracts.

A's String `method` defaults to `pad`; `repeat` and `reflect` are also admitted.
B's optional String `dtype` defaults to `float64`, alternatively `float32`.
Both accept Int64 `max_work`, default INT64_MAX. A has no `dtype` or canvas
parameter. C preserves the supplied CRV parameters and accepts its spread method
and spread profile separately.

A reads matching query tiles only. B reads matching query tiles and the complete
row range of the table on its own source grid; only the mathematically selected
one or two rows are evaluated. Full table dependency does not imply a payload
validity scan. Both execute one fixed Host CPU task per selected output tile;
Empty creates no sample task. Scratch is fixed and admitted before execution.
Structural errors, operational capacity/work failure and cancellation remain
separate. Finite samples, query membership and destination representability are
unchecked caller premises.

## Members

| ID | Specialization |
| --- | --- |
| [GEN-06A](GEN-06A_spread_coordinate.md) | spread coordinate |
| [GEN-06B](GEN-06B_lookup_numeric_gradient.md) | lookup numeric gradient |
| [GEN-06C](GEN-06C_gradient_color.md) | gradient color |

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
