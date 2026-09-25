# Generation specification

The maintainer accepted the [D01–D12 design decisions](decisions.md) on
2026-09-26. These are the final decisions from the generation-draft review.
Implementation and unresolved technical freeze gates remain separate.

- [Accepted decisions](decisions.md) — authoritative English contract.
- [Chinese translation](zh/decisions.zh.md) — reader translation.
- [Candidate catalog](catalog.md) — 30 families and 91 source candidates; no registry claim.
- [Technical freeze gates](freeze-gates.md) — dependencies before registration and implementation selection.
- [Generator overview](generators.md) and [path overview](paths.md).

The uploaded draft is source material, not a second authoritative specification.
In particular its blanket Whole policy, Philox4x32 packing, nextDown seams,
color-only meshes and capacity-failure max_count were superseded. Archive oracle
reports do not validate the changed contracts. Existing test implementations are
to be retired through separately validated implementation work.

## Complete specifications

All 124 source documents are revised in [op_specs](op_specs/GEN_common_contract.md):
91 original members, 30 family contracts and three shared contracts. Four additional
D12 member specs distinguish exact/grid results and Float64 publication. The fixed
Perlin permutation is included as algorithm data. Source-language detailed specs
are governed by the authoritative English decisions; unresolved technical details
are explicit registration gates, not silently inherited draft approvals.

Family indexes:

- [GEN-01](op_specs/GEN-01_constant_contract.md)
- [GEN-02](op_specs/GEN-02_coordinate_grid_contract.md)
- [GEN-03](op_specs/GEN-03_basic_shapes_contract.md)
- [GEN-04](op_specs/GEN-04_test_patterns_contract.md)
- [GEN-05](op_specs/GEN-05_gradient_coordinates_contract.md)
- [GEN-06](op_specs/GEN-06_gradient_lookup_contract.md)
- [GEN-07](op_specs/GEN-07_mesh_gradients_contract.md)
- [GEN-08](op_specs/GEN-08_point_distributions_contract.md)
- [NOI-01](op_specs/NOI-01_uniform_contract.md)
- [NOI-02](op_specs/NOI-02_gaussian_contract.md)
- [NOI-03](op_specs/NOI-03_correlated_contract.md)
- [NOI-04](op_specs/NOI-04_gradient_noise_contract.md)
- [NOI-05](op_specs/NOI-05_cellular_contract.md)
- [NOI-06](op_specs/NOI-06_fractal_contract.md)
- [NOI-07](op_specs/NOI-07_blue_noise_contract.md)
- [NOI-08](op_specs/NOI-08_poisson_contract.md)
- [NOI-09](op_specs/NOI-09_grain_contract.md)
- [NOI-10](op_specs/NOI-10_temporal_contract.md)
- [PTH-01](op_specs/PTH-01_construction_contract.md)
- [PTH-02](op_specs/PTH-02_evaluation_contract.md)
- [PTH-03](op_specs/PTH-03_arc_length_contract.md)
- [PTH-04](op_specs/PTH-04_resampling_contract.md)
- [PTH-05](op_specs/PTH-05_width_profiles_contract.md)
- [PTH-06](op_specs/PTH-06_transforms_contract.md)
- [PTH-07](op_specs/PTH-07_trim_dash_contract.md)
- [PTH-08](op_specs/PTH-08_editing_contract.md)
- [PTH-09](op_specs/PTH-09_fill_contract.md)
- [PTH-10](op_specs/PTH-10_stroke_contract.md)
- [PTH-11](op_specs/PTH-11_distance_contract.md)
- [PTH-12](op_specs/PTH-12_boolean_offset_contract.md)
