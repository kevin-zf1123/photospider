# 图形、坐标、渐变与噪声生成

本页按功能整理当前已接受的目标规格。Perlin2002 strict CPU Whole、CPU tiled 和
原生 GPU 形式已注册。三者使用 Result 输入与输出，GPU 按内核构建使用 Metal
或 Vulkan，且不提供 CPU fallback。当前 Result GPU 路径已在 Metal 上通过独立
oracle 与安装消费检查；Vulkan 的旧 Value 路径验证不代表当前 Result 实现。
Gaussian 与 PixelOE Vulkan 尚未交付。具体英文规格记录端口、
参数、数学定义与依赖规则，继承 01-numeric 与 02-format-color。
[共享契约](op_specs/GEN_common_contract.md)、[oracle 覆盖说明](oracle-coverage.md)、
[oracle 使用说明](../../../oracle/ops/generation/README.md)与
[研究来源及采用边界](research-sources.md)分别说明公共语义、可运行参考及证据范围。
具体 schema、随机映射或后端细节中仍标明未完成的部分，需要在注册成员前明确并验证。

当前 Perlin 注册、参数与验证见 [Perlin 实现说明](perlin-implementation.md)。

## 图形与坐标

| ID / 功能 | 输入 → 输出 | 参数与方法 | 验收 |
| --- | --- | --- | --- |
| [GEN-01 constant](op_specs/GEN-01_constant_contract.md) | shape+value→image/field | 显式 dtype、shape 与值；图像描述完整 | 任意ROI值一致，alpha不猜；成员：[GEN-01A](op_specs/GEN-01A_constant_tensor.md)、[GEN-01B](op_specs/GEN-01B_constant_image.md) |
| [GEN-02 coordinate grid](op_specs/GEN-02_coordinate_grid_contract.md) | shape→[H,W,2] | pixel/normalized edge坐标；identity source map用像素中心 | 相邻x差1px，宽W的归一坐标为(x+.5)/W；成员：[GEN-02A](op_specs/GEN-02A_coordinates_xy.md)、[GEN-02B](op_specs/GEN-02B_coordinate_axis.md) |
| [GEN-03 basic shape](op_specs/GEN-03_basic_shapes_contract.md) | rectangle/ellipse/polygon→coverage/SDF；star→path | position/size、fill、AA误差；先mask再着色 | mask外0；SDF内负外正（或正截断值），面积/亚像素移动另测；成员：[GEN-03A](op_specs/GEN-03A_rectangle_coverage.md)、[GEN-03B](op_specs/GEN-03B_ellipse_coverage.md)、[GEN-03C](op_specs/GEN-03C_polygon_coverage.md)、[GEN-03D](op_specs/GEN-03D_star_path.md)、[GEN-03E](op_specs/GEN-03E_rectangle_sdf.md)、[GEN-03F](op_specs/GEN-03F_ellipse_sdf.md) |
| [GEN-04 test patterns](op_specs/GEN-04_test_patterns_contract.md) | shape+pattern→field/image | checker/grid/ramp/impulse/zone plate/Siemens star/color bars | 频率、中心、强度明示；采样测试可解析；成员：[GEN-04A](op_specs/GEN-04A_checker_pattern.md)、[GEN-04B](op_specs/GEN-04B_grid_pattern.md)、[GEN-04C](op_specs/GEN-04C_ramp_pattern.md)、[GEN-04D](op_specs/GEN-04D_impulse_pattern.md)、[GEN-04E](op_specs/GEN-04E_zone_plate.md)、[GEN-04F](op_specs/GEN-04F_siemens_star.md)、[GEN-04G](op_specs/GEN-04G_linear_rgb_bars.md) |
| [GEN-05 gradient coordinate](op_specs/GEN-05_gradient_coordinates_contract.md) | geometry→scalar t[H,W] | linear/radial/angular/diamond/box/path-distance | 几何与颜色表分离；退化参数显式报错；成员：[GEN-05A](op_specs/GEN-05A_linear_gradient_coordinate.md)、[GEN-05B](op_specs/GEN-05B_radial_gradient_coordinate.md)、[GEN-05C](op_specs/GEN-05C_angular_gradient_coordinate.md)、[GEN-05D](op_specs/GEN-05D_diamond_gradient_coordinate.md)、[GEN-05E](op_specs/GEN-05E_box_gradient_coordinate.md)、[GEN-05F](op_specs/GEN-05F_path_distance_coordinate.md)、[GEN-05G](op_specs/GEN-05G_two_circle_radial_coordinate.md) |
| [GEN-06 gradient lookup](op_specs/GEN-06_gradient_lookup_contract.md) | t+数值表／颜色表→field/image | spread、数值 lookup 与颜色插值分离；颜色描述和 alpha 解释显式 | 端点/接缝/透明彩色中点；成员：[GEN-06A](op_specs/GEN-06A_spread_coordinate.md)、[GEN-06B](op_specs/GEN-06B_lookup_numeric_gradient.md)、[GEN-06C](op_specs/GEN-06C_gradient_color.md) |
| [GEN-07 mesh/bilinear numeric field](op_specs/GEN-07_mesh_gradients_contract.md) | 网格/四角数值→通用数值张量 | 显式拓扑与插值基函数；不隐式解释颜色、alpha 或 gamut | 控制点值、边界连续、非方形画布；成员：[GEN-07A](op_specs/GEN-07A_bilinear_rectangle_gradient.md)、[GEN-07B](op_specs/GEN-07B_triangle_mesh_gradient.md)、[GEN-07C](op_specs/GEN-07C_bicubic_parameter_patch.md) |
| [GEN-08 point distributions](op_specs/GEN-08_point_distributions_contract.md) | bounds+seed+density→points[M,2] | grid/jitter/Poisson disk；min_distance>0 | 最近距离、seed、动态M上界；成员：[GEN-08A](op_specs/GEN-08A_grid_points.md)、[GEN-08B](op_specs/GEN-08B_jittered_grid_points.md)、[GEN-08C](op_specs/GEN-08C_poisson_rejection_points.md)、[GEN-08D](op_specs/GEN-08D_bridson_points.md) |

Linear用`dot(p-a,b-a)/|b-a|²`；radial用`|p-c|/r`，椭圆先变换局部坐标；angular用`atan2`并映射到周期，中心与接缝显式；diamond/box采用L1/L∞等值线。two-circle radial选择最大有效根；无可返回坐标默认 valid=0，可显式 reject；相同圆非法。退化为全参数解时仍须检查是否存在有限最大有效参数。SVG明确包含坐标系、变换和spread method，只有颜色数组与尺寸不足以决定二维渐变。[^svg]

颜色数组使用完整 FMT 描述，完整图像采用 planar straight 表示，alpha 为独立平面。
颜色插值按成员规定选择空间及 alpha 处理；Lab 使用 `l=L*/100`，极坐标保留原始
hue 与圈数，无彩色端点按颜色规格处理。CSS Color 4 提供命名语义来源。[^css]
repeat 与 angular 的数学周期归一后正常舍入；舍入到上端点不再次 wrap 或 nextDown。
此规则独立于 rank/cell 成员明确规定的端点修正；pad 复制区间内输入，保留负零。
GEN-07 三个成员统一为通用数值插值，颜色与 alpha 处理通过显式 workflow 组合。

## 噪声

Gaussian表示分布，white/blue表示频谱，Perlin/Voronoi表示构造。将这些维度分开定义，既能产生Gaussian白噪声，也能产生具有指定相关长度的Gaussian场。

| ID / 功能 | 输入 → 输出 | 参数与算法 | 支持 / 验收 |
| --- | --- | --- | --- |
| [NOI-01 white uniform](op_specs/NOI-01_uniform_contract.md) | shape/seed→field | seed、stream/channel 显式；浮点映射依最终成员定义 | Philox4x64-10；核心已知答案、均值.5、方差1/12和相关性；成员：[NOI-01A](op_specs/NOI-01A_uniform_philox.md) |
| [NOI-02 white Gaussian](op_specs/NOI-02_gaussian_contract.md) | shape/seed→signed field | μ、σ 显式；具名 Box–Muller；不clip | log(0)保护，signed field；均值/方差、分布尾部；成员：[NOI-02A](op_specs/NOI-02A_gaussian_box_muller.md) |
| [NOI-03 correlated noise](op_specs/NOI-03_correlated_contract.md) | white field+filter→field | correlation length px、边界与方差归一 | H/W；与white频谱不同；成员：[NOI-03A](op_specs/NOI-03A_correlate_kernel.md)、[NOI-03B](op_specs/NOI-03B_correlated_gaussian.md) |
| [NOI-04 Perlin](op_specs/NOI-04_gradient_noise_contract.md) | coordinates→field | NOI-04A 固定 permutation、无 seed；坐标缩放由上游完成；NOI-04B 独立使用 Philox | O(P)，E；格点连续性和周期，输出范围按版本；成员：[NOI-04A](op_specs/NOI-04A_perlin2002_3d.md)、[NOI-04B](op_specs/NOI-04B_gradient2d_philox.md) |
| [NOI-05 cellular/Voronoi](op_specs/NOI-05_cellular_contract.md) | coords+feature process→F1/F2/ID | L2 距离；每 cell 点数与随机寻址显式 | 最近点搜索；F2-F1不是精确边界距离；成员：[NOI-05A](op_specs/NOI-05A_cellular2d_l2.md)、[NOI-05B](op_specs/NOI-05B_nearest_point_distances.md) |
| [NOI-06 fractal](op_specs/NOI-06_fractal_contract.md) | base noise→field | octaves、lacunarity、gain 显式；fBm/turbulence/ridged分别具名 | O(P·octaves)；按采样足迹截断 octave；不声称严格带限；成员：[NOI-06A](op_specs/NOI-06A_fbm_sum.md)、[NOI-06B](op_specs/NOI-06B_turbulence_sum.md)、[NOI-06C](op_specs/NOI-06C_ridged_sum.md)、[NOI-06D](op_specs/NOI-06D_footprint_fbm.md) |
| [NOI-07 blue noise](op_specs/NOI-07_blue_noise_contract.md) | rank tile/volume+coords→field/points | 通用 rank lookup 与具名 blue/STBN 资源分离 | PSD低频抑制、周期重复；生成点集不等于像素mask；成员：[NOI-07A](op_specs/NOI-07A_blue_rank_tile.md)、[NOI-07B](op_specs/NOI-07B_blue_threshold_points.md) |
| [NOI-08 Poisson/shot](op_specs/NOI-08_poisson_contract.md) | calibrated signal+gain→noisy field | electron/count单位、exposure、read noise分开 | 方差随均值；不能仅对RGB加独立同方差噪声；成员：[NOI-08A](op_specs/NOI-08A_poisson_icdf.md)、[NOI-08B](op_specs/NOI-08B_shot_electrons.md) |
| [NOI-09 speckle/grain](op_specs/NOI-09_grain_contract.md) | signal+model→field/image | multiplicative speckle与艺术film grain独立 | grain 显式 none/sum/l2；strength 仅作最终乘数；隐藏 RGB 同样处理、alpha 按位复制；成员：[NOI-09A](op_specs/NOI-09A_speckle_integer_looks.md)、[NOI-09B](op_specs/NOI-09B_artistic_linear_grain.md) |
| [NOI-10 continuous temporal noise](op_specs/NOI-10_temporal_contract.md) | x,y,t→field sequence | 连续维noise或时空blue，time单位显式 | 逐帧独立随机图不表示连续演化；成员：[NOI-10A](op_specs/NOI-10A_advected_gradient2d.md)、[NOI-10B](op_specs/NOI-10B_temporal_perlin3d.md)、[NOI-10C](op_specs/NOI-10C_stbn_rank_volume.md) |

Perlin2002的固定gradient/permutation与五次fade为明确可实现的版本，换hash或seed展开就改变图样，应纳入算法身份。[^perlin] Random123的counter-based方法适合按全局坐标取样；整数序列和正态浮点变换的跨后端一致性分别定义。[^random] 时空blue noise具有独立时间频谱设计，不能以每帧新2D蓝噪声替代。[^blue]

随机寻址由 seed、stream、绝对坐标、channel 及 frame/draw 等字段构成，不依赖tile顺序或局部ROI原点。cache依赖seed和全部生成参数。signed 噪声按数值 field 或显式颜色描述输出，不从通道数猜测颜色模型。
Philox4x64-10 的全局 x/y 为 signed32，frame/draw/stream 取 uint32 范围；生产 packing
与浮点位提取尚待明确，oracle 中的候选映射不能作为生产 golden。
有限候选拒绝与 FIFO Bridson 分别定义；max_count 是正常停止目标，候选／active list
耗尽可成功返回较少点，独立资源预算耗尽按错误处理。具名 blue/STBN 需要生产资源
与独立质量验收；普通 rank 结构合法不代表已经满足频谱质量。

## 使用与验收

`noise→range→mask/color ramp/displacement`可复用到纹理、转场、选择与液化；AE Cell Pattern官方列出matte和displacement用途，可作为使用面锚点。[^ae] `shape→coverage→colorize→over`同时服务选区与绘制。普通图形首版无需额外实现颜色合成。

解析测试覆盖坐标identity、线性渐变0/1端点、非法半径、repeat/reflect、全局seed一致；随机统计使用固定样本数与置信阈值，不能要求每次精确达到理论均值。shape/path coverage的误差用面积/轮廓检查，不仅看图。


执行依真实依赖选择 Regional、Halo 或 Whole，完整控制数据校验与区域计算分开。
CPU 是必需后端，GPU 按成员提供实际负载并使用独立 profile；同版本同 profile 的
数值、数量、顺序与 ID 必须确定。旧 `field.constant`、`field.coordinate` 不再提供实现。


## 来源

[^svg]: W3C，[*SVG2 Paint Servers*](https://www.w3.org/TR/SVG2/pservers.html)，2018-10-04 CR；渐变几何与spread。
[^css]: W3C，[*CSS Color4 Interpolation*](https://www.w3.org/TR/css-color-4/#interpolation)，颜色/极坐标/alpha插值。
[^perlin]: Ken Perlin，[*Improved Noise reference implementation*](https://mrl.cs.nyu.edu/~perlin/noise/)，2002。
[^random]: D. E. Shaw Research，[*Random123*](https://github.com/DEShawResearch/random123)，SC11论文2011及官方实现。
[^blue]: Wolfe等，[*Spatiotemporal Blue Noise Masks*](https://research.nvidia.com/publication/2022-07_spatiotemporal-blue-noise-masks)，NVIDIA Research，2022-07-06。
[^ae]: Adobe，[*Generate effects: Cell Pattern*](https://helpx.adobe.com/after-effects/desktop/apply-effects-and-animation-presets/list-of-effects/generate-effects.html)，滚动英文指南。
