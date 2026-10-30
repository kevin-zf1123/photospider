---
spec_schema_version: 1
id: GEN-06
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# GEN-06: Spread and gradient lookup

Spread, uniform raw-table lookup and model-specific color workflows are distinct. Each public node rounding stage is preserved. Color ramps retain original hue and winding, including achromatic input; there is no implicit shortest-path selection or hue normalization. Model-specific rules inherit the CRV/FMT contracts.

## Interface

GEN-06A and B each register the three CPU profile keys listed in their specs. GEN-06C is the public C++ workflow helper `ps::generation::gradient_color`; it expands to GEN-06A and one CRV-06 member and introduces no fused operation key. Numerical profiles follow [GEN-05](GEN-05_gradient_coordinates_contract.md#interface).

GEN-06A and B consume one-tensor Results with independent Float32/Float64 input dtypes, complete `sample_shape()` values including batch axes, positive extents and at most 2^40 logical elements per tensor. GEN-06A accepts rank 1..8; GEN-06B accepts query rank 1..7 and appends one atomic component axis. Their `values` output uses `photospider.tensor` v1 / `samples` and carries no color metadata. GEN-06A preserves the logical sample shape and dtype; GEN-06B selects its output dtype independently. Outputs represent the complete logical shape with explicit axes and do not retain the input batch-axis partition. GEN-06C inherits CRV's complete sample-shape and descriptor contracts.

GEN-06A's String `method` defaults to `pad`; `repeat` and `reflect` are also admitted. GEN-06B's optional String `dtype` defaults to `float64`, alternatively `float32`. GEN-06A has no `dtype` or canvas parameter. GEN-06C preserves the supplied CRV parameters and accepts its spread method and spread profile separately.

GEN-06A depends on the matching query samples only. GEN-06B depends on the matching query samples and the complete row range of the table; only the mathematically selected one or two rows are evaluated. Finite samples, query membership and destination representability are unchecked caller premises.

## Members

| ID | Specialization |
| --- | --- |
| [GEN-06A](GEN-06A_spread_coordinate.md) | spread coordinate |
| [GEN-06B](GEN-06B_lookup_numeric_gradient.md) | lookup numeric gradient |
| [GEN-06C](GEN-06C_gradient_color.md) | gradient color |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md).
