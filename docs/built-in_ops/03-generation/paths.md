# 路径、变化线宽与光栅化

本页按功能整理当前已接受的目标规格；各新成员尚未实现。具体英文规格记录端口、
参数、数学定义与依赖规则，继承 01-numeric 与 02-format-color。
[共享契约](op_specs/GEN_common_contract.md)、[oracle 覆盖说明](oracle-coverage.md)、
[oracle 使用说明](../../../examples/generation_workflow/README.md)与
[研究来源及采用边界](research-sources.md)分别说明公共语义、可运行参考及证据范围。
具体 schema、随机映射或后端细节中仍标明未完成的部分，需要在注册成员前明确并验证。

## 表示

固定次数首版：quadratic controls `[S,3,2]`，cubic controls `[S,4,2]`，每段固定样本 `[S,N,2]`。现有 CoreVerbs 保存 verbs/control_offsets/controls/subpath_offsets/closed，支持 M/L/Q/C/Z；primitive authority 已表示 Bézier、ArcSweep/FullTurn、Hermite 和非周期 B-spline，并验证连接/属性。布尔和字体轮廓节点仍待实现。公共端点是否重复、闭合是否隐含末段、每段属性归属必须固定。SVG2提供路径与子路径的公开语义词汇。[^svg]

坐标采用[公共约定](../00-foundation/contracts.md)，width 单位 px，表示完整直径。
线宽可使用正式附着字段或独立输入，每次显式选择唯一来源；支持整 PathSet 归一弧长、
每子路径归一弧长和每子路径实际弧长三种定义域。trim/dash 保留原始来源映射，不能
将截断后的片段重新归一后静默改变宽度。关联字段默认 reject_unmappable；显式 drop
必须报告丢弃内容。具体绑定与零长规则见[几何契约](op_specs/PTH_geometry_contract.md)。

## 算子目录

| ID / 功能 | 输入 → 输出 | 参数与方法 | 验收 |
| --- | --- | --- | --- |
| [PTH-01 make/concat/split/reverse](op_specs/PTH-01_construction_contract.md) | controls+topology→PathSet | 闭合/接点显式；不自动吸附邻近端点 | 段方向、索引、属性一一对应；成员：[PTH-01A](op_specs/PTH-01A_make_core_path.md)、[PTH-01B](op_specs/PTH-01B_concat_paths.md)、[PTH-01C](op_specs/PTH-01C_split_subpaths.md)、[PTH-01D](op_specs/PTH-01D_reverse_paths.md)、[PTH-01E](op_specs/PTH-01E_make_primitive_path.md) |
| [PTH-02 evaluate](op_specs/PTH-02_evaluation_contract.md) | path+t→position/tangent | 首版Bézier使用de Casteljau，t∈[0,1]；B-spline须另带degree/knots | 端点与解析直线；零导数不归一成NaN；成员：[PTH-02A](op_specs/PTH-02A_evaluate_bezier_path.md)、[PTH-02B](op_specs/PTH-02B_evaluate_elliptic_arc.md)、[PTH-02C](op_specs/PTH-02C_evaluate_hermite_path.md)、[PTH-02D](op_specs/PTH-02D_evaluate_bspline_path.md)、[PTH-02E](op_specs/PTH-02E_evaluate_pathset.md) |
| [PTH-03 arc length](op_specs/PTH-03_arc_length_contract.md) | path→length及(t,s)表 | 自适应积分/细分；误差px | 直线长度、可复核曲线数值积分；成员：[PTH-03A](op_specs/PTH-03A_bezier_arc_length.md)、[PTH-03B](op_specs/PTH-03B_primitive_arc_length.md) |
| [PTH-04 resample](op_specs/PTH-04_resampling_contract.md) | path+spacing/count→points | uniform_t/uniform_arc/adaptive；数量上界 | 相同弧长位置与控制点密度无关；成员：[PTH-04A](op_specs/PTH-04A_resample_uniform_t.md)、[PTH-04B](op_specs/PTH-04B_resample_uniform_arc.md)、[PTH-04C](op_specs/PTH-04C_resample_arc_spacing.md)、[PTH-04D](op_specs/PTH-04D_flatten_bezier_path.md) |
| [PTH-05 width profile](op_specs/PTH-05_width_profiles_contract.md) | path+scalar curve→widths/path | linear/PCHIP 分别具名；w≥0；显式定义域与唯一来源 | 0、亚像素、非单调宽度、端点；成员：[PTH-05A](op_specs/PTH-05A_width_profile_linear.md)、[PTH-05B](op_specs/PTH-05B_width_profile_pchip.md)、[PTH-05C](op_specs/PTH-05C_attach_linear_width.md) |
| [PTH-06 transform](op_specs/PTH-06_transforms_contract.md) | path+matrix→path | centerline变换后描边或原stroke轮廓整体变换分开 | 非均匀缩放两者应有不同明确结果；成员：[PTH-06A](op_specs/PTH-06A_transform_centerline.md)、[PTH-06B](op_specs/PTH-06B_transform_stroke_outline.md) |
| [PTH-07 trim/dash](op_specs/PTH-07_trim_dash_contract.md) | path+interval/pattern→path | 弧长px/归一长度、offset、closed seam | 跨段/接缝节奏连续；成员：[PTH-07A](op_specs/PTH-07A_trim_path.md)、[PTH-07B](op_specs/PTH-07B_dash_path.md) |
| [PTH-08 fit/simplify/smooth](op_specs/PTH-08_editing_contract.md) | samples/path→path | error tolerance、保尖角/拓扑、最大段数 | 最大几何误差而非只看点数下降；成员：[PTH-08A](op_specs/PTH-08A_simplify_polyline.md)、[PTH-08B](op_specs/PTH-08B_fit_cubic_segments.md)、[PTH-08C](op_specs/PTH-08C_smooth_chaikin.md) |
| [PTH-09 fill](op_specs/PTH-09_fill_contract.md) | path+canvas→coverage | nonzero默认，evenodd可选，AA模式 | 自交/孔洞/反向子路径/边缘面积；成员：[PTH-09A](op_specs/PTH-09A_fill_polygon_area.md)、[PTH-09B](op_specs/PTH-09B_fill_bezier_flatten.md) |
| [PTH-10 stroke](op_specs/PTH-10_stroke_contract.md) | path+width+canvas→coverage | round cap/join默认；butt/square/miter/bevel，miter limit必填 | 自交、尖角、重叠、极端miter、0width；成员：[PTH-10A](op_specs/PTH-10A_stroke_constant_area.md)、[PTH-10B](op_specs/PTH-10B_stroke_variable_round_area.md)、[PTH-10C](op_specs/PTH-10C_stroke_outline_flatten.md) |
| [PTH-11 distance](op_specs/PTH-11_distance_contract.md) | path→unsigned field或SDF | 开放中心线仅unsigned；signed必须来自按fill rule定义的区域或width/cap/join构成的stroke | inside<0、px单位；与解析/穷举比较，closest多解规则；成员：[PTH-11A](op_specs/PTH-11A_centerline_distance.md)、[PTH-11B](op_specs/PTH-11B_fill_region_sdf.md)、[PTH-11C](op_specs/PTH-11C_stroke_region_sdf.md) |
| [PTH-12 boolean/offset](op_specs/PTH-12_boolean_offset_contract.md) | 区域→精确 Result／网格 Result／显式发布几何 | union/intersect/difference/xor；精确与量化网格分离，Float64 发布单列 | 拓扑、几何退化、极小缝；后端尚未选定；成员：[PTH-12A](op_specs/PTH-12A_boolean_polygon_regions.md)、[PTH-12B](op_specs/PTH-12B_offset_euclidean_region.md)、[PTH-12C](op_specs/PTH-12C_boolean_exact_result.md)、[PTH-12D](op_specs/PTH-12D_boolean_grid_result.md)、[PTH-12E](op_specs/PTH-12E_publish_exact_geometry.md)、[PTH-12F](op_specs/PTH-12F_boolean_grid_float64.md) |

Bézier二次 `B=(1-t)²P0+2(1-t)tP1+t²P2`；三次用对应Bernstein权重。弧长 `s(t)=∫₀ᵗ|B′(u)|du`，具体成员规定单调(t,s)表与反求t策略。均匀t通常不是均匀几何距离；Fourier只适合显式周期profile，普通线宽优先linear/PCHIP避免振铃负值。

零长路径不计算未定义的 s/L；宽度定义域、重采样数量及零长 cap 分别按成员规定处理。
空 PathSet 使用零字段 rows 与 offsets=[0]；普通 Value 轴约束独立适用。B-spline
须带 degree/knots，并显式定义归一 t 到有效 knot 区间的映射。
精确几何对象与近似发布结果分离；拟合按后端求解器区分算子，连续 Hausdorff 误差
验证独立于求解器的离散残差。拓扑策略默认 allow_change，可选 reject_unproven；
无法证明时拒绝，有限采样或必要检查不能充当完整证明。

## 光栅化方案

CPU参考可采用自适应细分+扫描线/边表+coverage累计；曲线几何误差默认 .05px，预览可显式使用 .25px，coverage积分精度另定。逐像素遍历全部段成本O(P·S)，需要空间桶/BVH优化。GPU可研究细分三角形或Loop–Blinn解析曲线判定，它并不自动解决任意拓扑、变化线宽和准确面积积分。[^loop]

coverage与SDF独立：前者为像素面积比例[0,1]，后者为px距离。中心距离smoothstep只是coverage近似；细线、尖角和高曲率需面积基准。`min/max`组合SDF通常只保留部分符号/边界性质，不保证仍是精确距离；继续offset前可能需要重新距离化。

同一 stroke 自重叠采用几何并集 coverage 再着色，避免每个细分片重复source-over导致深色接缝；若工具目标是按笔迹累积flow，则命名为有序paint过程。CSP线宽的加减与比例缩放对渐尖端点不同，应提供独立控制。[^csp]

## Region、使用和验收

输出tile需读取所有可能覆盖其footprint的段，stroke扩张包含半宽、join/miter与AA。PathSet schema 不提供 stroke 专用空间索引；节点可用现有 ResultRelation 表达 Conservative 全路径支持，或提供经过证明的精确段映射，并声明 descriptor/拓扑依赖。路径中一次编辑的dirty不能只用控制点bbox，要覆盖曲线与描边的真实/保守范围。

概念流程：`controls→arc-length→width curve→stroke coverage→color ramp/constant→over`；另可`path→SDF→mask offset→feather`。解析验收包括100px直线、圆/矩形面积、孔洞、自交、闭合接缝、宽度0/.25/1/10px、非均匀变换和不同tile一致。边界与迭代使用Float64参考，GPU 按独立后端 profile 声明数值契约，同版本同 profile 保持确定性；几何误差与数值误差分别验收。


Regional、Halo 与 Whole 按实际数学依赖选择。全局拓扑构造需要 Whole 时显式声明；
局部 raster 输出与完整控制验证分开。精确 Boolean、量化网格 Boolean、精确 Result
发布和 Float64 网格输出分别由 PTH-12 成员定义，Result schema 与生产后端仍须落实。


## 来源

[^svg]: W3C，[*SVG2 Paths*](https://www.w3.org/TR/SVG2/paths.html)，2018-10-04 CR；路径动作/子路径。
[^loop]: Loop、Blinn，[*Resolution Independent Curve Rendering using Programmable Graphics Hardware*](https://www.microsoft.com/en-us/research/publication/resolution-independent-curve-rendering-using-programmable-graphics-hardware/)，SIGGRAPH2005。
[^csp]: CELSYS，[*Vector layers*](https://help.clip-studio.com/en-us/manual_en/180_layers/Vector_layers.htm)，滚动手册；线宽编辑方式。
