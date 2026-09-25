# Generation candidate catalog

Source inventory: `photospider-ops-03-generation-drafts.zip`, reviewed 2026-09-26.
The [accepted decisions](decisions.md) replace conflicting source proposals.
These 91 identifiers are retained candidates, not 91 registered keys. Source D1/D2 labels describe draft maturity only; neither chooses
an implementation batch. Names below identify source concepts, not final API keys. Every source member now links to its revised concrete specification; residual technical gates remain explicit.
All entries inherit D01 version/profile determinism and D02 dependency-based Region
rules. Additional solver, grid and output variants are reviewed before registration.
No old Whole, RNG packing or oracle-completion claim is imported by this inventory.

| Candidate | Source concept | Source maturity | Required decision alignment |
| --- | --- | --- | --- |
| [GEN-01A](op_specs/GEN-01A_constant_tensor.md) | constant_tensor | D1_draft | D01–D04: member semantics and execution review |
| [GEN-01B](op_specs/GEN-01B_constant_image.md) | constant_image | D1_draft | D01–D04: member semantics and execution review |
| [GEN-02A](op_specs/GEN-02A_coordinates_xy.md) | coordinates_xy | D1_draft | D01–D04: member semantics and execution review |
| [GEN-02B](op_specs/GEN-02B_coordinate_axis.md) | coordinate_axis | D1_draft | D01–D04: member semantics and execution review |
| [GEN-03A](op_specs/GEN-03A_rectangle_coverage.md) | rectangle_coverage | D1_draft | D01–D04: member semantics and execution review |
| [GEN-03B](op_specs/GEN-03B_ellipse_coverage.md) | ellipse_coverage | D2_draft | D01–D04: member semantics and execution review |
| [GEN-03C](op_specs/GEN-03C_polygon_coverage.md) | polygon_coverage | D1_draft | D01–D04: member semantics and execution review |
| [GEN-03D](op_specs/GEN-03D_star_path.md) | star_path | D2_draft | D01–D04: member semantics and execution review |
| [GEN-03E](op_specs/GEN-03E_rectangle_sdf.md) | rectangle_sdf | D1_draft | D01–D04: member semantics and execution review |
| [GEN-03F](op_specs/GEN-03F_ellipse_sdf.md) | ellipse_sdf | D2_draft | D01–D04: member semantics and execution review |
| [GEN-04A](op_specs/GEN-04A_checker_pattern.md) | checker_pattern | D1_draft | D01–D04: member semantics and execution review |
| [GEN-04B](op_specs/GEN-04B_grid_pattern.md) | grid_pattern | D1_draft | D01–D04: member semantics and execution review |
| [GEN-04C](op_specs/GEN-04C_ramp_pattern.md) | ramp_pattern | D1_draft | D01–D04: member semantics and execution review |
| [GEN-04D](op_specs/GEN-04D_impulse_pattern.md) | impulse_pattern | D1_draft | D01–D04: member semantics and execution review |
| [GEN-04E](op_specs/GEN-04E_zone_plate.md) | zone_plate | D1_draft | D01–D04: member semantics and execution review |
| [GEN-04F](op_specs/GEN-04F_siemens_star.md) | siemens_star | D1_draft | D01–D04: member semantics and execution review |
| [GEN-04G](op_specs/GEN-04G_linear_rgb_bars.md) | linear_rgb_bars | D1_draft | D01–D04: member semantics and execution review |
| [GEN-05A](op_specs/GEN-05A_linear_gradient_coordinate.md) | linear_gradient_coordinate | D1_draft | D06: normal seam rounding; maximum valid root where applicable |
| [GEN-05B](op_specs/GEN-05B_radial_gradient_coordinate.md) | radial_gradient_coordinate | D1_draft | D06: normal seam rounding; maximum valid root where applicable |
| [GEN-05C](op_specs/GEN-05C_angular_gradient_coordinate.md) | angular_gradient_coordinate | D1_draft | D06: normal seam rounding; maximum valid root where applicable |
| [GEN-05D](op_specs/GEN-05D_diamond_gradient_coordinate.md) | diamond_gradient_coordinate | D1_draft | D06: normal seam rounding; maximum valid root where applicable |
| [GEN-05E](op_specs/GEN-05E_box_gradient_coordinate.md) | box_gradient_coordinate | D1_draft | D06: normal seam rounding; maximum valid root where applicable |
| [GEN-05F](op_specs/GEN-05F_path_distance_coordinate.md) | path_distance_coordinate | D2_draft | D06: normal seam rounding; maximum valid root where applicable |
| [GEN-05G](op_specs/GEN-05G_two_circle_radial_coordinate.md) | two_circle_radial_coordinate | D2_draft | D06: normal seam rounding; maximum valid root where applicable |
| [GEN-06A](op_specs/GEN-06A_spread_coordinate.md) | spread_coordinate | D1_draft | D03, D06: model rules and explicit coordinate spread |
| [GEN-06B](op_specs/GEN-06B_lookup_numeric_gradient.md) | lookup_numeric_gradient | D1_draft | D03, D06: model rules and explicit coordinate spread |
| [GEN-06C](op_specs/GEN-06C_gradient_color.md) | gradient_color | D1_draft | D03, D06: model rules and explicit coordinate spread |
| [GEN-07A](op_specs/GEN-07A_bilinear_rectangle_gradient.md) | bilinear_rectangle_gradient | D1_draft | D03, D07: generic numeric components; no special alpha |
| [GEN-07B](op_specs/GEN-07B_triangle_mesh_gradient.md) | triangle_mesh_gradient | D2_draft | D03, D07: generic numeric components; no special alpha |
| [GEN-07C](op_specs/GEN-07C_bicubic_parameter_patch.md) | bicubic_parameter_patch | D2_draft | D03, D07: generic numeric components; no special alpha |
| [GEN-08A](op_specs/GEN-08A_grid_points.md) | grid_points | D1_draft | D08: Philox4x64 address/mapping freeze; D09 stop semantics |
| [GEN-08B](op_specs/GEN-08B_jittered_grid_points.md) | jittered_grid_points | D1_draft | D08: Philox4x64 address/mapping freeze; D09 stop semantics |
| [GEN-08C](op_specs/GEN-08C_poisson_rejection_points.md) | poisson_rejection_points | D1_draft | D08: Philox4x64 address/mapping freeze; D09 stop semantics |
| [GEN-08D](op_specs/GEN-08D_bridson_points.md) | bridson_points | D2_draft | D08: Philox4x64 address/mapping freeze; D09 stop semantics |
| [NOI-01A](op_specs/NOI-01A_uniform_philox.md) | uniform_philox | D1_draft | D08: apply new RNG only where this member consumes it |
| [NOI-02A](op_specs/NOI-02A_gaussian_box_muller.md) | gaussian_box_muller | D1_draft | D08: apply new RNG only where this member consumes it |
| [NOI-03A](op_specs/NOI-03A_correlate_kernel.md) | correlate_kernel | D1_draft | D08: apply new RNG only where this member consumes it |
| [NOI-03B](op_specs/NOI-03B_correlated_gaussian.md) | correlated_gaussian | D1_draft | D08: apply new RNG only where this member consumes it |
| [NOI-04A](op_specs/NOI-04A_perlin2002_3d.md) | perlin2002_3d | D1_draft | D08: apply new RNG only where this member consumes it |
| [NOI-04B](op_specs/NOI-04B_gradient2d_philox.md) | gradient2d_philox | D1_draft | D08: apply new RNG only where this member consumes it |
| [NOI-05A](op_specs/NOI-05A_cellular2d_l2.md) | cellular2d_l2 | D1_draft | D08: apply new RNG only where this member consumes it |
| [NOI-05B](op_specs/NOI-05B_nearest_point_distances.md) | nearest_point_distances | D1_draft | D08: apply new RNG only where this member consumes it |
| [NOI-06A](op_specs/NOI-06A_fbm_sum.md) | fbm_sum | D1_draft | D08: apply new RNG only where this member consumes it |
| [NOI-06B](op_specs/NOI-06B_turbulence_sum.md) | turbulence_sum | D1_draft | D08: apply new RNG only where this member consumes it |
| [NOI-06C](op_specs/NOI-06C_ridged_sum.md) | ridged_sum | D1_draft | D08: apply new RNG only where this member consumes it |
| [NOI-06D](op_specs/NOI-06D_footprint_fbm.md) | footprint_fbm | D2_draft | D08: apply new RNG only where this member consumes it |
| [NOI-07A](op_specs/NOI-07A_blue_rank_tile.md) | blue_rank_tile | D2_draft | D10: raw lookup first; named production resource gated |
| [NOI-07B](op_specs/NOI-07B_blue_threshold_points.md) | blue_threshold_points | D2_draft | D10: raw lookup first; named production resource gated |
| [NOI-08A](op_specs/NOI-08A_poisson_icdf.md) | poisson_icdf | D2_draft | D08, D11: RNG migration and model-specific rules |
| [NOI-08B](op_specs/NOI-08B_shot_electrons.md) | shot_electrons | D2_draft | D08, D11: RNG migration and model-specific rules |
| [NOI-09A](op_specs/NOI-09A_speckle_integer_looks.md) | speckle_integer_looks | D1_draft | D08, D11: RNG migration and model-specific rules |
| [NOI-09B](op_specs/NOI-09B_artistic_linear_grain.md) | artistic_linear_grain | D2_draft | D08, D11: RNG migration and model-specific rules |
| [NOI-10A](op_specs/NOI-10A_advected_gradient2d.md) | advected_gradient2d | D1_draft | D08: apply new RNG only where this member consumes it |
| [NOI-10B](op_specs/NOI-10B_temporal_perlin3d.md) | temporal_perlin3d | D1_draft | D08: apply new RNG only where this member consumes it |
| [NOI-10C](op_specs/NOI-10C_stbn_rank_volume.md) | stbn_rank_volume | D2_draft | D10: raw lookup first; named production resource gated |
| [PTH-01A](op_specs/PTH-01A_make_core_path.md) | make_core_path | D1_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-01B](op_specs/PTH-01B_concat_paths.md) | concat_paths | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-01C](op_specs/PTH-01C_split_subpaths.md) | split_subpaths | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-01D](op_specs/PTH-01D_reverse_paths.md) | reverse_paths | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-01E](op_specs/PTH-01E_make_primitive_path.md) | make_primitive_path | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-02A](op_specs/PTH-02A_evaluate_bezier_path.md) | evaluate_bezier_path | D1_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-02B](op_specs/PTH-02B_evaluate_elliptic_arc.md) | evaluate_elliptic_arc | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-02C](op_specs/PTH-02C_evaluate_hermite_path.md) | evaluate_hermite_path | D1_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-02D](op_specs/PTH-02D_evaluate_bspline_path.md) | evaluate_bspline_path | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-02E](op_specs/PTH-02E_evaluate_pathset.md) | evaluate_pathset | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-03A](op_specs/PTH-03A_bezier_arc_length.md) | bezier_arc_length | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-03B](op_specs/PTH-03B_primitive_arc_length.md) | primitive_arc_length | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-04A](op_specs/PTH-04A_resample_uniform_t.md) | resample_uniform_t | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-04B](op_specs/PTH-04B_resample_uniform_arc.md) | resample_uniform_arc | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-04C](op_specs/PTH-04C_resample_arc_spacing.md) | resample_arc_spacing | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-04D](op_specs/PTH-04D_flatten_bezier_path.md) | flatten_bezier_path | D1_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-05A](op_specs/PTH-05A_width_profile_linear.md) | width_profile_linear | D1_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-05B](op_specs/PTH-05B_width_profile_pchip.md) | width_profile_pchip | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-05C](op_specs/PTH-05C_attach_linear_width.md) | attach_linear_width | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-06A](op_specs/PTH-06A_transform_centerline.md) | transform_centerline | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-06B](op_specs/PTH-06B_transform_stroke_outline.md) | transform_stroke_outline | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-07A](op_specs/PTH-07A_trim_path.md) | trim_path | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-07B](op_specs/PTH-07B_dash_path.md) | dash_path | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-08A](op_specs/PTH-08A_simplify_polyline.md) | simplify_polyline | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-08B](op_specs/PTH-08B_fit_cubic_segments.md) | fit_cubic_segments | D2_draft | D12: separate operator per solver; continuous verifier gate |
| [PTH-08C](op_specs/PTH-08C_smooth_chaikin.md) | smooth_chaikin | D1_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-09A](op_specs/PTH-09A_fill_polygon_area.md) | fill_polygon_area | D1_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-09B](op_specs/PTH-09B_fill_bezier_flatten.md) | fill_bezier_flatten | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-10A](op_specs/PTH-10A_stroke_constant_area.md) | stroke_constant_area | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-10B](op_specs/PTH-10B_stroke_variable_round_area.md) | stroke_variable_round_area | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-10C](op_specs/PTH-10C_stroke_outline_flatten.md) | stroke_outline_flatten | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-11A](op_specs/PTH-11A_centerline_distance.md) | centerline_distance | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-11B](op_specs/PTH-11B_fill_region_sdf.md) | fill_region_sdf | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-11C](op_specs/PTH-11C_stroke_region_sdf.md) | stroke_region_sdf | D2_draft | D04, D05, D12: geometry, associations and publication |
| [PTH-12A](op_specs/PTH-12A_boolean_polygon_regions.md) | boolean_polygon_regions | D2_draft | D05, D12: exact/grid and exact/Float64 output variants |
| [PTH-12B](op_specs/PTH-12B_offset_euclidean_region.md) | offset_euclidean_region | D2_draft | D05, D12: exact/grid and exact/Float64 output variants |

## Added D12 variants

- [PTH-12C Exact polygon Boolean Result](op_specs/PTH-12C_boolean_exact_result.md)
- [PTH-12D Explicit-grid polygon Boolean Result](op_specs/PTH-12D_boolean_grid_result.md)
- [PTH-12E Publish exact geometry as Float64 PathSet](op_specs/PTH-12E_publish_exact_geometry.md)
- [PTH-12F Explicit-grid Boolean with Float64 output](op_specs/PTH-12F_boolean_grid_float64.md)
