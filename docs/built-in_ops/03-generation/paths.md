# 路径、变化线宽与光栅化

本页按功能整理已接受的路径规格。PTH-01 构造、拼接、稳定分组与反向，PTH-02 Bézier、椭圆弧、Hermite、B-spline 和 PathSet 求值，以及 PTH-09A 精确多边形填充已实现。其余路径成员保留各自的未实现状态。

Core 与 Primitive 使用独立的 CompleteBundle schema，partition 显式保存本次输入的 ObjectId。公开操作消费冻结的 fixed invocation ABI 8，不改变内核契约。
[共享契约](op_specs/GEN_common_contract.md)、[oracle 覆盖说明](oracle-coverage.md)、[oracle 使用说明](../../../oracle/ops/generation/README.md)与[研究来源及采用边界](research-sources.md)分别提供公共规则和独立数学参考。后续求解器与后端的实现状态见对应成员。

schema 与关联均使用 unchecked 契约。只检查静态结构；运行时 counts、offsets、引用、padding、源对象身份和几何数值合法性都由调用方保证。发布端和消费端不扫描或比较 payload，生产端按规定构造结果。长度计算、舍入细化、逼近算法及显式请求的质量测量保留。Float64 转换在构造坐标时累计位移界，舍入后的可表示性、位移预算与连接/拓扑条件由调用方保证，不再完成后扫描并决定是否发布；普通转换报告 topology_status=0。

## 表示

固定次数求值接口使用 quadratic controls `[S,3,2]`、cubic controls `[S,4,2]`，查询为显式 `segment_indices[N]` 与 `t[N]`，输出 position/derivative/tangent `[N,2]` 和 tangent_valid `[N]`。PathSet 是逻辑类型族，CoreVerbs 与 Primitives 使用独立 schema，转换必须显式。CoreVerbs 使用 `photospider.ops.path_core` v1，六个成员为 UInt8 `verbs[V]`、Int64 `control_offsets[V+1]`、Float64 `controls[C,2]`、Int64 `subpath_offsets[S+1]`、UInt8 `closed[S]` 和 Int64 `counts[3]`；三个计数依次表示有效 verb/control/subpath 数，空路径全部为 0，V/C/S 为正静态容量。Primitives 使用 `photospider.ops.path_primitives` v1 的 13 个固定成员，分别存引用、子路径、孤立点及 Bézier、ArcSweep/FullTurn、Hermite、非周期 B-spline 的数据池。Close 引用表达数学端点之间的闭合边，不要求把非有理端点塞进 Float64 控制点。属性和来源映射使用独立关联 Result。M/L/Q/C/Z 编码为 1..5，closed 为 0/1；未使用 verbs/closed 为 0、controls 为 +0，offsets 尾部重复各自的有效终值。构造器显式接收 counts，容量从输入 shape 推导；concat 按输入容量相加，Core reverse 使用 V+S、C+S、S 容量，为固定 M 起点后的显式闭合边预留空间；Primitive reverse、仿射变换及 split 复用输入容量，其余需要容量上界的生成算子使用显式 max 参数。controls 的 x/y 和 counts 的三个计数分别作为原子观察，六成员由 lowering 完整写入并以 CompleteBundle 完整交付；不增加验证扫描。公开算子 SDK 已提供 Core、Primitive 与 partition schema factory；其余派生 schema 仍按各自成员的实现状态区分。Core 反向两次恢复几何与方向，表示中可能保留冗余的零长度直线。SVG2 提供路径与子路径的公开语义词汇。[^svg]

Primitive 的 terminal Close 可以表达非 Float64 端点间的闭合边，但 v1 reverse 不承诺保留起点并反向这类闭合子路径；调用方须满足该成员的可表示输入域。该限制不增加运行时准入校验或隐式转换。

Primitive 反向保留静态池容量，可重排有效记录并更新引用、起始索引和 counts。控制点与权重按有效池长度 N 一起反转，各 spline 的新起点为 `N-control_begin-control_count`，因此共享、嵌套或交叉切片不会互相覆盖。knot 反射从不可变输入计算；原位置冲突时重新放置，容量紧张时通过去包含和最大权重叠匹配压缩序列。ArcSweep 与 FullTurn 即使引用同一个输入 arc 行，也分别使用各自的反向角度规则；按变换后的完整记录去重。压缩后的尾区按规范归零，工作量、匹配搜索和 scratch 均计入预算。

独立 Bézier 的 Int64 `degree` 决定静态控制点 shape，必须先检查原值属于 1..3，再转换内部整数类型。独立 B-spline 记录的第五列是 `weighted`：0 表示数学单位权重，显式 weights 输入仍保留；Primitive 对应列是 `knot_count`，始终使用自身的权重池。

坐标采用[公共约定](../00-foundation/contracts.md)，width 单位 px，表示完整直径。
线宽可使用正式附着字段或独立输入，每次显式选择唯一来源；支持整 PathSet 归一弧长、
每子路径归一弧长和每子路径实际弧长三种定义域。trim/dash 保留原始来源映射，不能
将截断后的片段重新归一后静默改变宽度。关联字段默认 reject_unmappable；显式 drop
必须报告丢弃内容。具体绑定与零长规则见[几何契约](op_specs/PTH_geometry_contract.md)。

## 派生数据的最小 schema

具体字段、shape 与编码以[英文几何共享契约](op_specs/PTH_geometry_contract.md)为准。所有 schema 都使用 version 1、正静态容量和有效 count；不用运行时 shape、通用属性字典或哈希校验。普通未使用行填零，offsets 尾部重复有效终值。来源 ObjectId 仅作为输入身份字节记录，不在发布或消费时比较；需要来源几何时由 workflow 显式传入其 ResultRef。

| schema 后缀（统一前缀 `photospider.ops.`） | 最小内容与边界 |
| --- | --- |
| `path_arclength_table` | segment IDs、knot offsets、`(t,s_lower,s_upper)`、子路径 offsets、总长及上下界、counts、source ID；表误差不能替代真实长度求值。 |
| `path_samples_param` / `path_samples_arc` | positions、source_segment、local_t、子路径 offsets、counts、source ID；arc 版本增加子路径内的 arc_s。local_t 独立舍入，不能用于重建原 position。 |
| `path_partition` | group_offsets、subpath_count、source ID；几何和顺序由配对的扁平 PathSet 提供。 |
| `path_width` | 固定标准键 width 的 Linear profiles、bindings、stops、widths、source IDs 和 counts；PCHIP 保持独立求值，不新增附着编码。 |
| `path_source_map` | 目标段到单一来源段的精确有理仿射参数映射；固定 limb 容量，不使用表达式图。无法精确表示时沿用 reject_unmappable 或显式 drop。 |
| `exact_polygon` / `grid_polygon` | 前者使用约分有理坐标及轮廓 offsets，后者使用 Int64 坐标；均有有效 counts。网格参数显式提供，backend 的交点处理规则仍需落实。 |
| `path_fit_report` / `geometry_report` | 仅保存现有算法要求的误差上界、拓扑状态、来源身份，以及显式丢弃 width 的标志；不记录迭代轨迹或新增校验数据。 |

有理数使用符号、有效 limb 数和 little-endian base-2^32 limbs，每个 limb 由四个 UInt8 字节表示。有效数约分且分母为正，规范零为 0/1；这些均是 unchecked 前提和生产端构造规则。真弧长切点不可表示为有理 affine 映射时，不用 Float64 近似伪装精确对应。

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
空 PathSet 使用 payload count=0，offsets 有效前缀为 [0]；固定容量和 padding 按共享 schema 构造；普通 Tensor 轴约束独立适用。B-spline
须带 degree/knots，并显式定义归一 t 到有效 knot 区间的映射。
精确几何对象与近似发布结果分离；拟合按后端求解器区分算子，连续 Hausdorff 误差
验证独立于求解器的离散残差。拓扑策略默认 allow_change，可选 reject_unproven；
无法证明时拒绝，有限采样或必要检查不能充当完整证明。

## 光栅化方案

CPU参考可采用自适应细分+扫描线/边表+coverage累计；曲线几何误差默认 .05px，预览可显式使用 .25px，coverage积分精度另定。逐像素遍历全部段成本O(P·S)，需要空间桶/BVH优化。GPU可研究细分三角形或Loop–Blinn解析曲线判定，它并不自动解决任意拓扑、变化线宽和准确面积积分。[^loop]

coverage与SDF独立：前者为像素面积比例[0,1]，后者为px距离。中心距离smoothstep只是coverage近似；细线、尖角和高曲率需面积基准。`min/max`组合SDF通常只保留部分符号/边界性质，不保证仍是精确距离；继续offset前可能需要重新距离化。

同一 stroke 自重叠采用几何并集 coverage 再着色，避免每个细分片重复source-over导致深色接缝；若工具目标是按笔迹累积flow，则命名为有序paint过程。CSP线宽的加减与比例缩放对渐尖端点不同，应提供独立控制。[^csp]

## Region、使用和验收

输出tile需读取所有可能覆盖其footprint的段，stroke扩张包含半宽、join/miter与AA。PathSet schema 不提供 stroke 专用空间索引；目标实现把完整路径或经证明的数学段支持转换为 fixed invocation ABI 8 的成员 tile reads，并通过静态 schema 读取和普通 task 依赖表达拓扑需求。路径中一次编辑的dirty不能只用控制点bbox，要覆盖曲线与描边的真实/保守范围。

概念流程：`controls→arc-length→width curve→stroke coverage→color ramp/constant→over`；另可`path→SDF→mask offset→feather`。解析验收包括100px直线、圆/矩形面积、孔洞、自交、闭合接缝、宽度0/.25/1/10px、非均匀变换和不同tile一致。边界与迭代使用Float64参考，GPU 按独立后端 profile 声明数值契约，同版本同 profile 保持确定性；几何误差与数值误差分别验收。


Regional、Halo 与 complete-input 按实际数学依赖选择。全局拓扑构造需要 complete-input 时显式声明；
局部 raster 输出与完整几何数学依赖分开。精确 Boolean、量化网格 Boolean、精确 Result
发布和 Float64 网格输出分别由 PTH-12 成员定义，Result schema 已在共享规格定义，运行时 codec/factory、数值算法及生产后端仍未实现。


## 来源

[^svg]: W3C，[*SVG2 Paths*](https://www.w3.org/TR/SVG2/paths.html)，2018-10-04 CR；路径动作/子路径。
[^loop]: Loop、Blinn，[*Resolution Independent Curve Rendering using Programmable Graphics Hardware*](https://www.microsoft.com/en-us/research/publication/resolution-independent-curve-rendering-using-programmable-graphics-hardware/)，SIGGRAPH2005。
[^csp]: CELSYS，[*Vector layers*](https://help.clip-studio.com/en-us/manual_en/180_layers/Vector_layers.htm)，滚动手册；线宽编辑方式。
