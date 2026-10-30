---
spec_schema_version: 1
id: FIL-17B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: FIL-17
function: local_laplacian_reference
proposed_operation_keys:
- filter.local_laplacian_reference_strict
- filter.local_laplacian_reference_accelerated_apple_silicon
- filter.local_laplacian_reference_accelerated_x86_64
numeric_reference: S+B
oracle_scope: mathematical_reference
research_sources:
- S07
---

# FIL-17B: local_laplacian_reference

Per-coefficient Local Laplacian reference.

Inherits [FIL-17](FIL-17_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.local_laplacian_reference_strict`, `filter.local_laplacian_reference_accelerated_apple_silicon`, `filter.local_laplacian_reference_accelerated_x86_64`.

`input` → `output` with the original dtype and complete sample shape. Each combination of nonspatial axes is an independent plane; no color interpretation is inferred.

### Parameters

Required Int64 `y_axis` and `x_axis` address distinct axes of the complete sample shape. Required Int64 `levels` is nonnegative and cannot reduce beyond the first 1×1 level. Required Float64 `edge_threshold` σ>0, `detail_exponent` α>0, and `edge_slope` β≥0 are finite caller premises. The fixed FIL-15 binomial-5, reflect_half profile determines radius and RN stages; there is no color mode. `levels=0` is a bit-copy.

## Mathematics

Numerical class: **S+B**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

First build the Gaussian pyramid G from input. At each level l<L and location q, let g=G_l[q]. For every full-resolution value v, let d=v-g and remap `r_g(v)=g+sign(d)*f(abs(d))`, where f(z)=σ*(z/σ)^α for z≤σ and σ+β*(z-σ) otherwise; sign(0)=0. Round each remapped value to RN_t, build its FIL-15B pyramid, and extract the detail at l,q as target L_lq. Reuse the original GL as the coarsest base. Reconstruct with FIL-15C. For α=β=1, explicitly bit-copy as identity; do not claim losslessness after a round trip.

## Behavior

Except for identity, the output depends on the complete plane. Each reference g comes from the original G; remapping covers the full source image. A fixed halo cannot replace this whole-image reconstruction definition.

The result equals the literal full-pyramid construction for every coefficient, preserving every remap, reduce, detail, expand and reconstruction RN stage. No range sampling or approximate interpolation of guide values is used.

## Acceptance

α=β=1 is identity; constants are preserved; β=0 saturates large edges; compare small images against the per-coefficient reference; ordinary band gain produces a different result.

## References

[S07 · Paris, Hasinoff, Kautz: Local Laplacian Filters](../research-sources.md#s07)
