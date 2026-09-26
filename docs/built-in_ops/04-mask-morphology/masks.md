# 蒙版、选区与形态学

## 基本术语

| 术语 | 含义 |
| --- | --- |
| Coverage | Float32/Float64 的有限 `[0,1]` 软 mask。 |
| Binary | Float32/Float64 的精确 `0/1` 二值 mask。未来可能增加 Bool 表示，当前不规定其布局和接口。 |
| Field | 有限 Float32/Float64 数值场。 |
| Distance | 带度量、单位和几何依据的距离场；完整距离用规定的 Inf 表示无特征点。 |
| Labels | Int64 对象编号，0 表示背景。 |
| Region | 请求的输出坐标集合；局部邻域和全局依赖由各成员规定。 |

## 基础数值规定

数值舍入、strict/accelerated 精度与浮点环境规定统一继承
[NUM 共同契约](../01-numeric/op_specs/NUM_common_contract.md)和
[NUM accelerated 契约](../01-numeric/op_specs/NUM_accelerated_contract.md)。
MASK 不另设舍入模式或误差容限；各成员只明确公式、舍入阶段和数值域。
二值消费者不隐式阈值化。完整距离的无特征点 Inf 与有限计算溢出分开处理。
完整规定见[共同契约](op_specs/MASK_common_contract.md)。各规格的实现状态在文件头标明。

## 族目录

| 族 | 功能 | 成员数 | 成员 |
| --- | --- | ---: | --- |
| [MASK-01](op_specs/MASK-01_boolean_contract.md) | Boolean / 软逻辑 | 3 | binary_logic、fuzzy_logic、independent_coverage |
| [MASK-02](op_specs/MASK-02_invert_contract.md) | 反相 | 1 | invert |
| [MASK-03](op_specs/MASK-03_threshold_contract.md) | 阈值与软区间 | 5 | threshold、range_mask、soft_threshold、soft_range、nonzero_to_binary |
| [MASK-04](op_specs/MASK-04_color_range_contract.md) | 颜色范围 | 7 | coordinate_range、lab76_range、hue_range、lab2000_range、fit_color_groups_table、fit_color_groups_image、apply_color_groups |
| [MASK-05](op_specs/MASK-05_morphology_contract.md) | 膨胀 / 腐蚀 | 2 | dilate、erode |
| [MASK-06](op_specs/MASK-06_open_close_contract.md) | 开 / 闭运算 | 2 | opening、closing |
| [MASK-07](op_specs/MASK-07_offset_contract.md) | 几何 offset | 4 | offset_discrete、shift_distance_field、threshold_distance_field、offset_polygon_grid |
| [MASK-08](op_specs/MASK-08_gaussian_contract.md) | 高斯羽化 | 1 | gaussian_feather |
| [MASK-09](op_specs/MASK-09_distance_feather_contract.md) | 距离羽化 | 2 | distance_feather_linear、distance_feather_smoothstep |
| [MASK-10](op_specs/MASK-10_distance_transform_contract.md) | 距离场 / nearest | 4 | nearest_feature、signed_center_distance、truncated_nearest_feature、truncated_signed_distance |
| [MASK-11](op_specs/MASK-11_flood_contract.md) | flood / region grow | 3 | flood_fixed、flood_neighbor、flood_barrier |
| [MASK-12](op_specs/MASK-12_components_contract.md) | 连通分量与属性 | 7 | label_compact、label_min_pixel、component_count、component_areas、component_bboxes、component_bundle、filter_area_index |
| [MASK-13](op_specs/MASK-13_cleanup_contract.md) | 填洞 / 去小区域 | 3 | fill_holes、remove_small、fill_small_holes |
| [MASK-14](op_specs/MASK-14_residuals_contract.md) | 边界与形态学残差 | 5 | morph_gradient、inner_border、outer_border、white_top_hat、black_top_hat |
| [MASK-15](op_specs/MASK-15_topology_reconstruction_contract.md) | 骨架 / 重建 | 7 | thin_topological、thin_distance_ordered、reconstruct_dilate、reconstruct_erode、thin_zhang_suen、thin_guo_hall、medial_axis_maximal_balls |
| [MASK-16](op_specs/MASK-16_close_gap_contract.md) | 封口填充 | 3 | bridge_axis_gaps、fill_axis_gaps、fill_morphological_gaps |
| [MASK-17](op_specs/MASK-17_apply_contract.md) | 应用 / 限制样本 | 4 | affect_result、multiply_mask、restricted_mean、apply_alpha |

## 逐成员入口

| ID / spec | 类别 | 拟议 key / template | oracle entry |
| --- | --- | --- | --- |
| [MASK-01A — Binary Boolean operations](op_specs/MASK-01A_binary_logic.md) | primitive / D1_draft | `mask.binary_logic_strict` | `binary_logic` |
| [MASK-01B — Fuzzy mask operations](op_specs/MASK-01B_fuzzy_logic.md) | primitive / D1_draft | `mask.fuzzy_logic_strict` | `fuzzy_logic` |
| [MASK-01C — Independent-coverage operations](op_specs/MASK-01C_independent_coverage.md) | primitive / D1_draft | `mask.independent_coverage_strict` | `independent_coverage` |
| [MASK-02A — Invert coverage on the current canvas](op_specs/MASK-02A_invert.md) | primitive / D1_draft | `mask.invert_strict` | `invert` |
| [MASK-03A — Hard scalar threshold](op_specs/MASK-03A_threshold.md) | primitive / D1_draft | `mask.threshold_strict` | `threshold` |
| [MASK-03B — Hard interval mask](op_specs/MASK-03B_range_mask.md) | primitive / D1_draft | `mask.range_mask_strict` | `range_mask` |
| [MASK-03C — Soft centered threshold](op_specs/MASK-03C_soft_threshold.md) | primitive / D1_draft | `mask.soft_threshold_strict` | `soft_threshold` |
| [MASK-03D — Soft interval selector](op_specs/MASK-03D_soft_range.md) | primitive / D1_draft | `mask.soft_range_strict` | `soft_range` |
| [MASK-03E — Explicit UInt8 truthiness adapter](op_specs/MASK-03E_nonzero_to_binary.md) | primitive / D1_draft | `mask.nonzero_to_binary_strict` | `nonzero_to_binary` |
| [MASK-04A — Scaled coordinate-distance mask](op_specs/MASK-04A_coordinate_range.md) | primitive / D1_draft | `mask.coordinate_range_strict` | `coordinate_range` |
| [MASK-04B — Normalized-Lab DeltaE76 selector](op_specs/MASK-04B_lab76_range.md) | primitive / D1_draft | `mask.lab76_range_strict` | `lab76_range` |
| [MASK-04C — Periodic hue selector with explicit neutral policy](op_specs/MASK-04C_hue_range.md) | primitive / D1_draft | `mask.hue_range_strict` | `hue_range` |
| [MASK-04D — CIEDE2000 color range](op_specs/MASK-04D_lab2000_range.md) | primitive / D1_draft | `mask.lab2000_range_strict` | `lab2000_range` |
| [MASK-04E — Fit grouped color samples](op_specs/MASK-04E_fit_color_groups_table.md) | primitive / D1_draft | `mask.fit_color_groups_table_strict` | `fit_color_groups_table` |
| [MASK-04F — Fit image regions by group ID](op_specs/MASK-04F_fit_color_groups_image.md) | primitive / D1_draft | `mask.fit_color_groups_image_strict` | `fit_color_groups_image` |
| [MASK-04G — Apply grouped color selection](op_specs/MASK-04G_apply_color_groups.md) | primitive / D1_draft | `mask.apply_color_groups_strict` | `apply_color_groups` |
| [MASK-05A — Flat dilation](op_specs/MASK-05A_dilate.md) | primitive / D1_draft | `mask.dilate_strict` | `dilate` |
| [MASK-05B — Flat erosion](op_specs/MASK-05B_erode.md) | primitive / D1_draft | `mask.erode_strict` | `erode` |
| [MASK-06A — Open a soft or binary mask](op_specs/MASK-06A_opening.md) | primitive / D1_draft | `mask.opening_strict` | `opening` |
| [MASK-06B — Close a soft or binary mask](op_specs/MASK-06B_closing.md) | primitive / D1_draft | `mask.closing_strict` | `closing` |
| [MASK-07A — Physical-metric binary center offset](op_specs/MASK-07A_offset_discrete.md) | primitive / D1_draft | `mask.offset_discrete_strict` | `offset_discrete` |
| [MASK-07B — Shift a signed level set](op_specs/MASK-07B_shift_distance_field.md) | primitive / D1_draft | `mask.shift_distance_field_strict` | `shift_distance_field` |
| [MASK-07C — Threshold a signed distance/level set](op_specs/MASK-07C_threshold_distance_field.md) | primitive / D1_draft | `mask.threshold_distance_field_strict` | `threshold_distance_field` |
| [MASK-07D — Continuous polygon offset with fixed sample-grid coverage](op_specs/MASK-07D_offset_polygon_grid.md) | primitive / D2_draft | `mask.offset_polygon_grid_strict` | `offset_polygon_grid` |
| [MASK-08A — Finite normalized Gaussian mask feather](op_specs/MASK-08A_gaussian_feather.md) | primitive / D1_draft | `mask.gaussian_feather_strict` | `gaussian_feather` |
| [MASK-09A — Linear signed-distance feather](op_specs/MASK-09A_distance_feather_linear.md) | primitive / D1_draft | `mask.distance_feather_linear_strict` | `distance_feather_linear` |
| [MASK-09B — Smoothstep signed-distance feather](op_specs/MASK-09B_distance_feather_smoothstep.md) | primitive / D1_draft | `mask.distance_feather_smoothstep_strict` | `distance_feather_smoothstep` |
| [MASK-10A — Nearest discrete feature and distance](op_specs/MASK-10A_nearest_feature.md) | primitive / D1_draft | `mask.nearest_feature_strict` | `nearest_feature` |
| [MASK-10B — Signed center-to-opposite-class distance](op_specs/MASK-10B_signed_center_distance.md) | primitive / D1_draft | `mask.signed_center_distance_strict` | `signed_center_distance` |
| [MASK-10C — Local truncated nearest-feature distance](op_specs/MASK-10C_truncated_nearest_feature.md) | primitive / D1_draft | `mask.truncated_nearest_feature_strict` | `truncated_nearest_feature` |
| [MASK-10D — Local truncated signed center-distance](op_specs/MASK-10D_truncated_signed_distance.md) | primitive / D1_draft | `mask.truncated_signed_distance_strict` | `truncated_signed_distance` |
| [MASK-11A — Fixed-seed-range region growing](op_specs/MASK-11A_flood_fixed.md) | primitive / D1_draft | `mask.flood_fixed_strict` | `flood_fixed` |
| [MASK-11B — Neighbor-relative region growing](op_specs/MASK-11B_flood_neighbor.md) | primitive / D1_draft | `mask.flood_neighbor_strict` | `flood_neighbor` |
| [MASK-11C — Flood regions bounded by a binary barrier](op_specs/MASK-11C_flood_barrier.md) | primitive / D1_draft | `mask.flood_barrier_strict` | `flood_barrier` |
| [MASK-12A — Four/eight-connected compact component labels](op_specs/MASK-12A_label_compact.md) | primitive / D1_draft | `mask.label_compact_strict` | `label_compact` |
| [MASK-12B — Four/eight-connected MinPixel labels](op_specs/MASK-12B_label_min_pixel.md) | primitive / D1_draft | `mask.label_min_pixel_strict` | `label_min_pixel` |
| [MASK-12C — Count declared positive label IDs](op_specs/MASK-12C_component_count.md) | primitive / D1_draft | `mask.component_count_strict` | `component_count` |
| [MASK-12D — Associated component area table](op_specs/MASK-12D_component_areas.md) | primitive / D1_draft | `mask.component_areas_strict` | `component_areas` |
| [MASK-12E — Associated component bounding-box table](op_specs/MASK-12E_component_bboxes.md) | primitive / D1_draft | `mask.component_bboxes_strict` | `component_bboxes` |
| [MASK-12F — MinPixel labels with complete component attributes](op_specs/MASK-12F_component_bundle.md) | primitive / D1_draft | `mask.component_bundle_strict` | `component_bundle` |
| [MASK-12G — Filter labels through their associated area index](op_specs/MASK-12G_filter_area_index.md) | primitive / D1_draft | `mask.filter_area_index_strict` | `filter_area_index` |
| [MASK-13A — Fill every enclosed binary hole](op_specs/MASK-13A_fill_holes.md) | primitive / D1_draft | `mask.fill_holes_strict` | `fill_holes` |
| [MASK-13B — Remove small foreground components](op_specs/MASK-13B_remove_small.md) | primitive / D1_draft | `mask.remove_small_strict` | `remove_small` |
| [MASK-13C — Fill enclosed holes up to an inclusive size](op_specs/MASK-13C_fill_small_holes.md) | primitive / D1_draft | `mask.fill_small_holes_strict` | `fill_small_holes` |
| [MASK-14A — Morphological gradient](op_specs/MASK-14A_morph_gradient.md) | primitive / D1_draft | `mask.morph_gradient_strict` | `morph_gradient` |
| [MASK-14B — Inner morphological border](op_specs/MASK-14B_inner_border.md) | primitive / D1_draft | `mask.inner_border_strict` | `inner_border` |
| [MASK-14C — Outer morphological border](op_specs/MASK-14C_outer_border.md) | primitive / D1_draft | `mask.outer_border_strict` | `outer_border` |
| [MASK-14D — White top hat](op_specs/MASK-14D_white_top_hat.md) | primitive / D1_draft | `mask.white_top_hat_strict` | `white_top_hat` |
| [MASK-14E — Black top hat](op_specs/MASK-14E_black_top_hat.md) | primitive / D1_draft | `mask.black_top_hat_strict` | `black_top_hat` |
| [MASK-15A — Row-major topology-preserving binary thinning](op_specs/MASK-15A_thin_topological.md) | primitive / D1_draft | `mask.thin_topological_strict` | `thin_topological` |
| [MASK-15B — Distance-priority topology thinning with radius](op_specs/MASK-15B_thin_distance_ordered.md) | primitive / D1_draft | `mask.thin_distance_ordered_strict` | `thin_distance_ordered` |
| [MASK-15C — Grayscale reconstruction by dilation](op_specs/MASK-15C_reconstruct_dilate.md) | primitive / D1_draft | `mask.reconstruct_dilate_strict` | `reconstruct_dilate` |
| [MASK-15D — Grayscale reconstruction by erosion](op_specs/MASK-15D_reconstruct_erode.md) | primitive / D1_draft | `mask.reconstruct_erode_strict` | `reconstruct_erode` |
| [MASK-15E — Zhang-Suen binary thinning](op_specs/MASK-15E_thin_zhang_suen.md) | primitive / D1_draft | `mask.thin_zhang_suen_strict` | `thin_zhang_suen` |
| [MASK-15F — Guo-Hall binary thinning](op_specs/MASK-15F_thin_guo_hall.md) | primitive / D1_draft | `mask.thin_guo_hall_strict` | `thin_guo_hall` |
| [MASK-15G — Maximal digital-ball axis](op_specs/MASK-15G_medial_axis_maximal_balls.md) | primitive / D1_draft | `mask.medial_axis_maximal_balls_strict` | `medial_axis_maximal_balls` |
| [MASK-16A — Bridge bounded horizontal and vertical barrier gaps](op_specs/MASK-16A_bridge_axis_gaps.md) | primitive / D2_draft | `mask.bridge_axis_gaps_strict` | `bridge_axis_gaps` |
| [MASK-16B — Flood using a temporary axis-gap boundary](op_specs/MASK-16B_fill_axis_gaps.md) | composite_workflow / D2_draft | `mask.fill_axis_gaps` | `fill_axis_gaps` |
| [MASK-16C — Flood using a temporary morphology-closed boundary](op_specs/MASK-16C_fill_morphological_gaps.md) | composite_workflow / D2_draft | `mask.fill_morphological_gaps` | `fill_morphological_gaps` |
| [MASK-17A — Interpolate a processed result by a mask](op_specs/MASK-17A_affect_result.md) | primitive / D1_draft | `mask.affect_result_strict` | `affect_result` |
| [MASK-17B — Gate or scale selected numeric channels](op_specs/MASK-17B_multiply_mask.md) | primitive / D1_draft | `mask.multiply_mask_strict` | `multiply_mask` |
| [MASK-17C — Mask-restricted local mean with denominator validity](op_specs/MASK-17C_restricted_mean.md) | primitive / D1_draft | `mask.restricted_mean_strict` | `restricted_mean` |
| [MASK-17D — Apply coverage to internal straight-image alpha only](op_specs/MASK-17D_apply_alpha.md) | primitive / D1_draft | `mask.apply_alpha_strict` | `apply_alpha` |

## 相关入口

- [研究资料](research-sources.md)
- [数学 oracle](../../../examples/mask_morphology_oracle/README.md)
- [机器目录](op_specs/catalog.json)
