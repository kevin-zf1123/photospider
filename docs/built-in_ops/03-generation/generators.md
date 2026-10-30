# 图形、坐标、渐变与噪声生成

本页是 GEN-01～08 与 NOI-01～10 的入口目录。端口、参数、公式、数值类别与随机寻址以链接的英文族契约和成员规格为准；路径族见[路径、变化线宽与光栅化](paths.md)。

## 基本术语

| 术语 | 含义 |
| --- | --- |
| 像素中心坐标 | 默认左上原点、y 向下，像素 (x,y) 的中心为 `(x+0.5, y+0.5)`；宽 W 的归一坐标为 `(x+0.5)/W`。 |
| 颜色解释 | 使用算子 SDK 的 `TensorDescription`，由消费算子的静态参数显式提供；Result 只携带结构与样本。完整图像为 planar straight，alpha 是同一 tensor 内的独立平面。 |
| 随机寻址 | 由 seed、stream、绝对坐标、channel 与 frame/draw 组成的 counter-based 地址，不依赖 tile 顺序或 ROI 原点；位分配见 [NOI 随机契约](op_specs/NOI_random_contract.md)。 |
| 分布 / 频谱 / 构造 | Gaussian 描述分布，white/blue 描述频谱，Perlin/Voronoi 描述构造方法；三者分别定义，可组合出例如具有指定相关长度的 Gaussian 场。 |
| DynamicPoints v1 | `photospider.ops.dynamic_points`：Float64 `positions[P,2]`、Int64 `ids[P]`、Int64 `count[1]`；只有前 count 行有效，未使用行 positions 为 +0、ids 为 0。点生成报告为 `photospider.ops.point_generation_report` v1。 |

## 基础数值规定

GEN 与 NOI 继承[生成共享契约](op_specs/GEN_common_contract.md)以及 [NUM 共同契约](../01-numeric/op_specs/NUM_common_contract.md)的舍入规定。GEN-05A～E、GEN-06A/B 与 NOI-06A～D 提供 strict、Apple Silicon、x86-64 三个 profile，accelerated Float64 遵守 [NUM accelerated 契约](../01-numeric/op_specs/NUM_accelerated_contract.md)的最终误差界。schema 与随机协议均为 unchecked：静态成员、dtype、shape、轴与参数类型在准备阶段检查，count、offset、ID、关联、padding、随机地址及数值与几何域由调用方保证。同版本同 profile 的数值、数量、顺序与 ID 确定。

## 图形与坐标

| 族 | 输入 → 输出 | 参数与方法 | 成员 |
| --- | --- | --- | --- |
| [GEN-01 constant](op_specs/GEN-01_constant_contract.md) | shape+value→image/field | 显式 dtype、shape 与值；图像描述完整 | [GEN-01A](op_specs/GEN-01A_constant_tensor.md)、[GEN-01B](op_specs/GEN-01B_constant_image.md) |
| [GEN-02 coordinate grid](op_specs/GEN-02_coordinate_grid_contract.md) | shape→[H,W,2] | pixel/normalized edge坐标；identity source map用像素中心 | [GEN-02A](op_specs/GEN-02A_coordinates_xy.md)、[GEN-02B](op_specs/GEN-02B_coordinate_axis.md) |
| [GEN-03 basic shape](op_specs/GEN-03_basic_shapes_contract.md) | rectangle/ellipse/polygon→coverage/SDF；star→path | position/size、fill、AA误差；先mask再着色 | [GEN-03A](op_specs/GEN-03A_rectangle_coverage.md)、[GEN-03B](op_specs/GEN-03B_ellipse_coverage.md)、[GEN-03C](op_specs/GEN-03C_polygon_coverage.md)、[GEN-03D](op_specs/GEN-03D_star_path.md)、[GEN-03E](op_specs/GEN-03E_rectangle_sdf.md)、[GEN-03F](op_specs/GEN-03F_ellipse_sdf.md) |
| [GEN-04 test patterns](op_specs/GEN-04_test_patterns_contract.md) | shape+pattern→field/image | checker/grid/ramp/impulse/zone plate/Siemens star/color bars | [GEN-04A](op_specs/GEN-04A_checker_pattern.md)、[GEN-04B](op_specs/GEN-04B_grid_pattern.md)、[GEN-04C](op_specs/GEN-04C_ramp_pattern.md)、[GEN-04D](op_specs/GEN-04D_impulse_pattern.md)、[GEN-04E](op_specs/GEN-04E_zone_plate.md)、[GEN-04F](op_specs/GEN-04F_siemens_star.md)、[GEN-04G](op_specs/GEN-04G_linear_rgb_bars.md) |
| [GEN-05 gradient coordinate](op_specs/GEN-05_gradient_coordinates_contract.md) | geometry→scalar t[H,W] | linear/radial/angular/diamond/box/path-distance | [GEN-05A](op_specs/GEN-05A_linear_gradient_coordinate.md)、[GEN-05B](op_specs/GEN-05B_radial_gradient_coordinate.md)、[GEN-05C](op_specs/GEN-05C_angular_gradient_coordinate.md)、[GEN-05D](op_specs/GEN-05D_diamond_gradient_coordinate.md)、[GEN-05E](op_specs/GEN-05E_box_gradient_coordinate.md)、[GEN-05F](op_specs/GEN-05F_path_distance_coordinate.md)、[GEN-05G](op_specs/GEN-05G_two_circle_radial_coordinate.md) |
| [GEN-06 gradient lookup](op_specs/GEN-06_gradient_lookup_contract.md) | t+数值表／颜色表→field/image | spread、数值 lookup 与颜色插值分离；颜色描述和 alpha 解释显式 | [GEN-06A](op_specs/GEN-06A_spread_coordinate.md)、[GEN-06B](op_specs/GEN-06B_lookup_numeric_gradient.md)、[GEN-06C](op_specs/GEN-06C_gradient_color.md) |
| [GEN-07 mesh/bilinear numeric field](op_specs/GEN-07_mesh_gradients_contract.md) | 网格/四角数值→通用数值张量 | 显式拓扑与插值基函数；不隐式解释颜色、alpha 或 gamut | [GEN-07A](op_specs/GEN-07A_bilinear_rectangle_gradient.md)、[GEN-07B](op_specs/GEN-07B_triangle_mesh_gradient.md)、[GEN-07C](op_specs/GEN-07C_bicubic_parameter_patch.md) |
| [GEN-08 point distributions](op_specs/GEN-08_point_distributions_contract.md) | bounds+seed+参数→DynamicPoints（静态 positions[P,2]、ids[P]、count[1]） | grid/jitter/Poisson disk；min_distance>0 | [GEN-08A](op_specs/GEN-08A_grid_points.md)、[GEN-08B](op_specs/GEN-08B_jittered_grid_points.md)、[GEN-08C](op_specs/GEN-08C_poisson_rejection_points.md)、[GEN-08D](op_specs/GEN-08D_bridson_points.md) |

Linear用`dot(p-a,b-a)/|b-a|²`；radial用`|p-c|/r`，椭圆先变换局部坐标；angular用`atan2`并映射到周期，中心与接缝显式；diamond/box采用L1/L∞等值线。two-circle radial选择最大有效根；二次根式在区间舍入前检查有理平方根，精确相消返回 `+0, valid=1`，并处理 Float32/64 中点的 ties-even 舍入，负非零值下溢仍保留 `-0`；无可返回坐标默认 valid=0，可显式 reject；相同圆非法。退化为全参数解时仍须检查是否存在有限最大有效参数。SVG明确包含坐标系、变换和spread method，只有颜色数组与尺寸不足以决定二维渐变。[^svg]

## 噪声

| 族 | 输入 → 输出 | 参数与方法 | 成员 |
| --- | --- | --- | --- |
| [NOI-01 white uniform](op_specs/NOI-01_uniform_contract.md) | shape/seed→field | seed、stream/channel 显式；浮点映射依最终成员定义 | [NOI-01A](op_specs/NOI-01A_uniform_philox.md) |
| [NOI-02 white Gaussian](op_specs/NOI-02_gaussian_contract.md) | shape/seed→signed field | μ、σ 显式；具名 Box–Muller；不clip | [NOI-02A](op_specs/NOI-02A_gaussian_box_muller.md) |
| [NOI-03 correlated noise](op_specs/NOI-03_correlated_contract.md) | white field+filter→field | correlation length px、边界与方差归一 | [NOI-03A](op_specs/NOI-03A_correlate_kernel.md)、[NOI-03B](op_specs/NOI-03B_correlated_gaussian.md) |
| [NOI-04 Perlin](op_specs/NOI-04_gradient_noise_contract.md) | coordinates→field | NOI-04A 固定 permutation、无 seed；坐标缩放由上游完成；NOI-04B 独立使用 Philox | [NOI-04A](op_specs/NOI-04A_perlin2002_3d.md)、[NOI-04B](op_specs/NOI-04B_gradient2d_philox.md) |
| [NOI-05 cellular/Voronoi](op_specs/NOI-05_cellular_contract.md) | coords+feature process→F1/F2/ID | L2 距离；每 cell 点数与随机寻址显式 | [NOI-05A](op_specs/NOI-05A_cellular2d_l2.md)、[NOI-05B](op_specs/NOI-05B_nearest_point_distances.md) |
| [NOI-06 fractal](op_specs/NOI-06_fractal_contract.md) | coordinates+[footprint]→field | basis、octaves、frequency、lacunarity、gain 显式；fBm/turbulence/ridged分别具名 | [NOI-06A](op_specs/NOI-06A_fbm_sum.md)、[NOI-06B](op_specs/NOI-06B_turbulence_sum.md)、[NOI-06C](op_specs/NOI-06C_ridged_sum.md)、[NOI-06D](op_specs/NOI-06D_footprint_fbm.md) |
| [NOI-07 blue noise](op_specs/NOI-07_blue_noise_contract.md) | rank tile/volume+coords→field/points | 通用 rank lookup 与具名 blue/STBN 资源分离 | [NOI-07A](op_specs/NOI-07A_blue_rank_tile.md)、[NOI-07B](op_specs/NOI-07B_blue_threshold_points.md) |
| [NOI-08 Poisson/shot](op_specs/NOI-08_poisson_contract.md) | 非负期望计数或期望电子数→同形状 Int64 计数 | 上游显式完成电子单位标定；read noise、gain、clipping 和 ADC 各自显式处理 | [NOI-08A](op_specs/NOI-08A_poisson_icdf.md)、[NOI-08B](op_specs/NOI-08B_shot_electrons.md) |
| [NOI-09 speckle/grain](op_specs/NOI-09_grain_contract.md) | signal+model→field/image | multiplicative speckle与艺术film grain独立 | [NOI-09A](op_specs/NOI-09A_speckle_integer_looks.md)、[NOI-09B](op_specs/NOI-09B_artistic_linear_grain.md) |
| [NOI-10 continuous temporal noise](op_specs/NOI-10_temporal_contract.md) | x,y,t→field sequence | 连续维noise或时空blue，time单位显式 | [NOI-10A](op_specs/NOI-10A_advected_gradient2d.md)、[NOI-10B](op_specs/NOI-10B_temporal_perlin3d.md)、[NOI-10C](op_specs/NOI-10C_stbn_rank_volume.md) |

Perlin2002的固定gradient/permutation与五次fade为明确可实现的版本，换hash或seed展开就改变图样，应纳入算法身份。[^perlin] Random123的counter-based方法适合按全局坐标取样；整数序列和正态浮点变换的跨后端一致性分别定义。[^random] 时空blue noise具有独立时间频谱设计，不能以每帧新2D蓝噪声替代。[^blue]

## 逐成员入口

### 图形与坐标

| ID / spec | 类别 | 注册 key | 实现 |
| --- | --- | --- | --- |
| [GEN-01A — constant tensor](op_specs/GEN-01A_constant_tensor.md) | primitive | `generation.constant_tensor_v1_strict`<br>`generation.constant_tensor_v1_accelerated_apple_silicon`<br>`generation.constant_tensor_v1_accelerated_x86_64` | 已实现 |
| [GEN-01B — constant image](op_specs/GEN-01B_constant_image.md) | primitive | `generation.constant_image_v1_strict`<br>`generation.constant_image_v1_accelerated_apple_silicon`<br>`generation.constant_image_v1_accelerated_x86_64` | 已实现 |
| [GEN-02A — coordinates xy](op_specs/GEN-02A_coordinates_xy.md) | primitive | `generation.coordinates_xy_v1_strict`<br>`generation.coordinates_xy_v1_accelerated_apple_silicon`<br>`generation.coordinates_xy_v1_accelerated_x86_64` | 已实现 |
| [GEN-02B — coordinate axis](op_specs/GEN-02B_coordinate_axis.md) | primitive | `generation.coordinate_axis_v1_strict`<br>`generation.coordinate_axis_v1_accelerated_apple_silicon`<br>`generation.coordinate_axis_v1_accelerated_x86_64` | 已实现 |
| [GEN-03A — rectangle coverage](op_specs/GEN-03A_rectangle_coverage.md) | primitive | `generation.rectangle_coverage_v1_strict`<br>`generation.rectangle_coverage_v1_accelerated_apple_silicon`<br>`generation.rectangle_coverage_v1_accelerated_x86_64` | 已实现 |
| [GEN-03B — ellipse coverage](op_specs/GEN-03B_ellipse_coverage.md) | primitive | `generation.ellipse_coverage_v1_strict`<br>`generation.ellipse_coverage_v1_accelerated_apple_silicon`<br>`generation.ellipse_coverage_v1_accelerated_x86_64` | 已实现 |
| [GEN-03C — polygon coverage](op_specs/GEN-03C_polygon_coverage.md) | composite_workflow | — | 已实现 |
| [GEN-03D — star path](op_specs/GEN-03D_star_path.md) | primitive | `generation.star_path_v1_strict` | 已实现 |
| [GEN-03E — rectangle sdf](op_specs/GEN-03E_rectangle_sdf.md) | primitive | `generation.rectangle_sdf_v1_strict`<br>`generation.rectangle_sdf_v1_accelerated_apple_silicon`<br>`generation.rectangle_sdf_v1_accelerated_x86_64` | 已实现 |
| [GEN-03F — ellipse sdf](op_specs/GEN-03F_ellipse_sdf.md) | primitive | `generation.ellipse_sdf_v1_strict`<br>`generation.ellipse_sdf_v1_accelerated_apple_silicon`<br>`generation.ellipse_sdf_v1_accelerated_x86_64` | 已实现 |
| [GEN-04A — checker pattern](op_specs/GEN-04A_checker_pattern.md) | primitive | `generation.checker_pattern_v1_strict` | 已实现 |
| [GEN-04B — grid pattern](op_specs/GEN-04B_grid_pattern.md) | primitive | `generation.grid_pattern_v1_strict` | 已实现 |
| [GEN-04C — ramp pattern](op_specs/GEN-04C_ramp_pattern.md) | primitive | `generation.ramp_pattern_v1_strict`<br>`generation.ramp_pattern_v1_accelerated_apple_silicon`<br>`generation.ramp_pattern_v1_accelerated_x86_64` | 已实现 |
| [GEN-04D — impulse pattern](op_specs/GEN-04D_impulse_pattern.md) | primitive | `generation.impulse_pattern_v1_strict` | 已实现 |
| [GEN-04E — zone plate](op_specs/GEN-04E_zone_plate.md) | primitive | `generation.zone_plate_v1_strict`<br>`generation.zone_plate_v1_accelerated_apple_silicon`<br>`generation.zone_plate_v1_accelerated_x86_64` | 已实现 |
| [GEN-04F — siemens star](op_specs/GEN-04F_siemens_star.md) | primitive | `generation.siemens_star_v1_strict`<br>`generation.siemens_star_v1_accelerated_apple_silicon`<br>`generation.siemens_star_v1_accelerated_x86_64` | 已实现 |
| [GEN-04G — linear rgb bars](op_specs/GEN-04G_linear_rgb_bars.md) | primitive | `generation.linear_rgb_bars_v1_strict` | 已实现 |
| [GEN-05A — linear gradient coordinate](op_specs/GEN-05A_linear_gradient_coordinate.md) | primitive | `generation.linear_gradient_coordinate_v1_strict`<br>`generation.linear_gradient_coordinate_v1_accelerated_apple_silicon`<br>`generation.linear_gradient_coordinate_v1_accelerated_x86_64` | 已实现 |
| [GEN-05B — radial gradient coordinate](op_specs/GEN-05B_radial_gradient_coordinate.md) | primitive | `generation.radial_gradient_coordinate_v1_strict`<br>`generation.radial_gradient_coordinate_v1_accelerated_apple_silicon`<br>`generation.radial_gradient_coordinate_v1_accelerated_x86_64` | 已实现 |
| [GEN-05C — angular gradient coordinate](op_specs/GEN-05C_angular_gradient_coordinate.md) | primitive | `generation.angular_gradient_coordinate_v1_strict`<br>`generation.angular_gradient_coordinate_v1_accelerated_apple_silicon`<br>`generation.angular_gradient_coordinate_v1_accelerated_x86_64` | 已实现 |
| [GEN-05D — diamond gradient coordinate](op_specs/GEN-05D_diamond_gradient_coordinate.md) | primitive | `generation.diamond_gradient_coordinate_v1_strict`<br>`generation.diamond_gradient_coordinate_v1_accelerated_apple_silicon`<br>`generation.diamond_gradient_coordinate_v1_accelerated_x86_64` | 已实现 |
| [GEN-05E — box gradient coordinate](op_specs/GEN-05E_box_gradient_coordinate.md) | primitive | `generation.box_gradient_coordinate_v1_strict`<br>`generation.box_gradient_coordinate_v1_accelerated_apple_silicon`<br>`generation.box_gradient_coordinate_v1_accelerated_x86_64` | 已实现 |
| [GEN-05F — path distance coordinate](op_specs/GEN-05F_path_distance_coordinate.md) | composite_workflow | — | 已实现 |
| [GEN-05G — two circle radial coordinate](op_specs/GEN-05G_two_circle_radial_coordinate.md) | primitive | `generation.two_circle_radial_coordinate_v1_strict`<br>`generation.two_circle_radial_coordinate_v1_accelerated_apple_silicon`<br>`generation.two_circle_radial_coordinate_v1_accelerated_x86_64` | 已实现 |
| [GEN-06A — spread coordinate](op_specs/GEN-06A_spread_coordinate.md) | primitive | `generation.spread_coordinate_v1_strict`<br>`generation.spread_coordinate_v1_accelerated_apple_silicon`<br>`generation.spread_coordinate_v1_accelerated_x86_64` | 已实现 |
| [GEN-06B — lookup numeric gradient](op_specs/GEN-06B_lookup_numeric_gradient.md) | primitive | `generation.lookup_numeric_gradient_v1_strict`<br>`generation.lookup_numeric_gradient_v1_accelerated_apple_silicon`<br>`generation.lookup_numeric_gradient_v1_accelerated_x86_64` | 已实现 |
| [GEN-06C — gradient color](op_specs/GEN-06C_gradient_color.md) | composite_workflow | — | 已实现 |
| [GEN-07A — bilinear rectangle gradient](op_specs/GEN-07A_bilinear_rectangle_gradient.md) | primitive | `generation.bilinear_rectangle_gradient_v1_strict` | 已实现 |
| [GEN-07B — triangle mesh gradient](op_specs/GEN-07B_triangle_mesh_gradient.md) | primitive | `generation.triangle_mesh_gradient_v1_strict` | 已实现 |
| [GEN-07C — bicubic parameter patch](op_specs/GEN-07C_bicubic_parameter_patch.md) | primitive | `generation.bicubic_parameter_patch_v1_strict` | 已实现 |
| [GEN-08A — grid points](op_specs/GEN-08A_grid_points.md) | primitive | `generation.grid_points_v1_strict` | 已实现 |
| [GEN-08B — jittered grid points](op_specs/GEN-08B_jittered_grid_points.md) | primitive | `generation.jittered_grid_points_v1_strict` | 已实现 |
| [GEN-08C — poisson rejection points](op_specs/GEN-08C_poisson_rejection_points.md) | composite_workflow | `generation.poisson_rejection_candidates_v1_strict`<br>`generation.bind_point_generation_report_v1` | 已实现 |
| [GEN-08D — bridson points](op_specs/GEN-08D_bridson_points.md) | composite_workflow | `generation.bridson_candidates_v1_strict`<br>`generation.bind_point_generation_report_v1` | 已实现 |

### 噪声

| ID / spec | 类别 | 注册 key | 实现 |
| --- | --- | --- | --- |
| [NOI-01A — uniform philox](op_specs/NOI-01A_uniform_philox.md) | primitive | `noise.uniform_philox_v1_strict` | 已实现 |
| [NOI-02A — gaussian box muller](op_specs/NOI-02A_gaussian_box_muller.md) | primitive | `noise.gaussian_box_muller_v1_strict`<br>`noise.gaussian_box_muller_v1_accelerated_apple_silicon`<br>`noise.gaussian_box_muller_v1_accelerated_x86_64` | 已实现 |
| [NOI-03A — correlate kernel](op_specs/NOI-03A_correlate_kernel.md) | primitive | `noise.correlate_kernel_v1_strict`<br>`noise.correlate_kernel_v1_accelerated_apple_silicon`<br>`noise.correlate_kernel_v1_accelerated_x86_64` | 已实现 |
| [NOI-03B — correlated gaussian](op_specs/NOI-03B_correlated_gaussian.md) | composite_workflow | — | 已实现 |
| [NOI-04A — perlin2002 3d](op_specs/NOI-04A_perlin2002_3d.md) | primitive | `noise.perlin2002_3d_v1_strict_cpu_whole`<br>`noise.perlin2002_3d_v1_strict_cpu_tiled`<br>`noise.perlin2002_3d_v1_strict_gpu` | 已实现 |
| [NOI-04B — gradient2d philox](op_specs/NOI-04B_gradient2d_philox.md) | primitive | `noise.gradient2d_philox_v1_strict`<br>`noise.gradient2d_philox_v1_accelerated_apple_silicon`<br>`noise.gradient2d_philox_v1_accelerated_x86_64` | 已实现 |
| [NOI-05A — cellular2d l2](op_specs/NOI-05A_cellular2d_l2.md) | primitive | `noise.cellular2d_l2_v1_strict`<br>`noise.cellular2d_l2_v1_accelerated_apple_silicon`<br>`noise.cellular2d_l2_v1_accelerated_x86_64` | 已实现 |
| [NOI-05B — nearest point distances](op_specs/NOI-05B_nearest_point_distances.md) | primitive | `noise.nearest_point_distances_v1_strict`<br>`noise.nearest_point_distances_v1_accelerated_apple_silicon`<br>`noise.nearest_point_distances_v1_accelerated_x86_64` | 已实现 |
| [NOI-06A — fbm sum](op_specs/NOI-06A_fbm_sum.md) | primitive | `noise.fbm_sum_v1_strict`<br>`noise.fbm_sum_v1_accelerated_apple_silicon`<br>`noise.fbm_sum_v1_accelerated_x86_64` | 已实现 |
| [NOI-06B — turbulence sum](op_specs/NOI-06B_turbulence_sum.md) | primitive | `noise.turbulence_sum_v1_strict`<br>`noise.turbulence_sum_v1_accelerated_apple_silicon`<br>`noise.turbulence_sum_v1_accelerated_x86_64` | 已实现 |
| [NOI-06C — ridged sum](op_specs/NOI-06C_ridged_sum.md) | primitive | `noise.ridged_sum_v1_strict`<br>`noise.ridged_sum_v1_accelerated_apple_silicon`<br>`noise.ridged_sum_v1_accelerated_x86_64` | 已实现 |
| [NOI-06D — footprint fbm](op_specs/NOI-06D_footprint_fbm.md) | primitive | `noise.footprint_fbm_v1_strict`<br>`noise.footprint_fbm_v1_accelerated_apple_silicon`<br>`noise.footprint_fbm_v1_accelerated_x86_64` | 已实现 |
| [NOI-07A — blue rank tile](op_specs/NOI-07A_blue_rank_tile.md) | primitive | `noise.rank_tile_v1_strict` | 已实现 |
| [NOI-07B — blue threshold points](op_specs/NOI-07B_blue_threshold_points.md) | primitive | `noise.rank_threshold_points_v1_strict` | 已实现 |
| [NOI-08A — poisson icdf](op_specs/NOI-08A_poisson_icdf.md) | primitive | `noise.poisson_icdf_v1_strict` | 已实现 |
| [NOI-08B — shot electrons](op_specs/NOI-08B_shot_electrons.md) | composite_workflow | — | 已实现 |
| [NOI-09A — speckle integer looks](op_specs/NOI-09A_speckle_integer_looks.md) | primitive | `noise.speckle_integer_looks_v1_strict`<br>`noise.speckle_integer_looks_v1_accelerated_apple_silicon`<br>`noise.speckle_integer_looks_v1_accelerated_x86_64` | 已实现 |
| [NOI-09B — artistic linear grain](op_specs/NOI-09B_artistic_linear_grain.md) | composite_workflow | — | 已实现 |
| [NOI-10A — advected gradient2d](op_specs/NOI-10A_advected_gradient2d.md) | primitive | `noise.advected_gradient2d_v1_strict`<br>`noise.advected_gradient2d_v1_accelerated_apple_silicon`<br>`noise.advected_gradient2d_v1_accelerated_x86_64` | 已实现 |
| [NOI-10B — temporal perlin3d](op_specs/NOI-10B_temporal_perlin3d.md) | primitive | `noise.temporal_perlin3d_v1_strict`<br>`noise.temporal_perlin3d_v1_accelerated_apple_silicon`<br>`noise.temporal_perlin3d_v1_accelerated_x86_64` | 已实现 |
| [NOI-10C — stbn rank volume](op_specs/NOI-10C_stbn_rank_volume.md) | primitive | `noise.rank_volume_v1_strict` | 已实现 |

## 使用与验收

`noise→range→mask/color ramp/displacement`可复用到纹理、转场、选择与液化；AE Cell Pattern官方列出matte和displacement用途，可作为使用面锚点。[^ae] `shape→coverage→colorize→over`同时服务选区与绘制。

解析测试覆盖坐标identity、线性渐变0/1端点、合法半径边界、repeat/reflect、全局seed一致；随机统计使用固定样本数与置信阈值，不能要求每次精确达到理论均值。shape/path coverage的误差用面积/轮廓检查，不仅看图。

## 相关入口

- [路径、变化线宽与光栅化](paths.md)
- [研究来源及采用边界](research-sources.md)
- [oracle 覆盖](oracle-coverage.md)
- [oracle 使用说明](../../../oracle/ops/generation/README.md)

## 来源

[^svg]: W3C，[*SVG2 Paint Servers*](https://www.w3.org/TR/SVG2/pservers.html)，2018-10-04 CR；渐变几何与spread。
[^css]: W3C，[*CSS Color4 Interpolation*](https://www.w3.org/TR/css-color-4/#interpolation)，颜色/极坐标/alpha插值。
[^perlin]: Ken Perlin，[*Improved Noise reference implementation*](https://mrl.cs.nyu.edu/~perlin/noise/)，2002。
[^random]: D. E. Shaw Research，[*Random123*](https://github.com/DEShawResearch/random123)，SC11论文2011及官方实现。
[^blue]: Wolfe等，[*Spatiotemporal Blue Noise Masks*](https://research.nvidia.com/publication/2022-07_spatiotemporal-blue-noise-masks)，NVIDIA Research，2022-07-06。
[^ae]: Adobe，[*Generate effects: Cell Pattern*](https://helpx.adobe.com/after-effects/desktop/apply-effects-and-animation-presets/list-of-effects/generate-effects.html)，滚动英文指南。
