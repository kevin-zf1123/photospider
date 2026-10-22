# 图形、坐标、渐变与噪声生成

本页按功能整理已接受的规格。GEN-01～04、GEN-05A～E、GEN-06、NOI-01～04、NOI-06、PTH-01～02 已实现，GEN-03C 所需的 PTH-09A 精确多边形面积填充也已实现；其余功能按各成员的实现状态区分。生成类别共有 110 个注册 key，其中本次梯度／分形实现提供 33 个 CPU key。Core、Primitive、partition 的公开 schema factory 及四项 workflow helper 位于 `photospider/ops/generation.hpp`。GEN-05F/G 保持独立的未实现状态。

[共享契约](op_specs/GEN_common_contract.md)、[oracle 覆盖说明](oracle-coverage.md)、[oracle 使用说明](../../../oracle/ops/generation/README.md)与[研究来源及采用边界](research-sources.md)分别定义公共语义和独立参考的证据范围。英文成员规格继续作为数学行为的权威定义。

schema 与随机协议均为 unchecked 契约：只检查静态成员、dtype、shape、轴和参数类型；count、offset、ID、关联、padding、随机地址及数值/几何域都由调用方保证。上游按规格构造输出，下游直接消费，均不增加 payload 扫描或关联比较。算法本身的候选选择、收敛、质量计算、容量预算和取消规则保留。

## 图形与坐标

GEN-03B 椭圆覆盖率与 GEN-03F 椭圆距离的 `max_refinements` 是 Int64 参数，默认 4096。实现完整保留正 Int64 上限，不按 32 位截断；工作量预算与可用区间精度仍独立限制计算。只要在默认次数内得到同一舍入证书，`2^32`、`2^32+1` 和 `INT64_MAX` 等较大上限应给出相同结果。

| ID / 功能 | 输入 → 输出 | 参数与方法 | 验收 |
| --- | --- | --- | --- |
| [GEN-01 constant](op_specs/GEN-01_constant_contract.md) | shape+value→image/field | 显式 dtype、shape 与值；图像描述完整 | 任意ROI值一致，alpha不猜；成员：[GEN-01A](op_specs/GEN-01A_constant_tensor.md)、[GEN-01B](op_specs/GEN-01B_constant_image.md) |
| [GEN-02 coordinate grid](op_specs/GEN-02_coordinate_grid_contract.md) | shape→[H,W,2] | pixel/normalized edge坐标；identity source map用像素中心 | 相邻x差1px，宽W的归一坐标为(x+.5)/W；成员：[GEN-02A](op_specs/GEN-02A_coordinates_xy.md)、[GEN-02B](op_specs/GEN-02B_coordinate_axis.md) |
| [GEN-03 basic shape](op_specs/GEN-03_basic_shapes_contract.md) | rectangle/ellipse/polygon→coverage/SDF；star→path | position/size、fill、AA误差；先mask再着色 | mask外0；SDF内负外正（或正截断值），面积/亚像素移动另测；成员：[GEN-03A](op_specs/GEN-03A_rectangle_coverage.md)、[GEN-03B](op_specs/GEN-03B_ellipse_coverage.md)、[GEN-03C](op_specs/GEN-03C_polygon_coverage.md)、[GEN-03D](op_specs/GEN-03D_star_path.md)、[GEN-03E](op_specs/GEN-03E_rectangle_sdf.md)、[GEN-03F](op_specs/GEN-03F_ellipse_sdf.md) |
| [GEN-04 test patterns](op_specs/GEN-04_test_patterns_contract.md) | shape+pattern→field/image | checker/grid/ramp/impulse/zone plate/Siemens star/color bars | 频率、中心、强度明示；采样测试可解析；成员：[GEN-04A](op_specs/GEN-04A_checker_pattern.md)、[GEN-04B](op_specs/GEN-04B_grid_pattern.md)、[GEN-04C](op_specs/GEN-04C_ramp_pattern.md)、[GEN-04D](op_specs/GEN-04D_impulse_pattern.md)、[GEN-04E](op_specs/GEN-04E_zone_plate.md)、[GEN-04F](op_specs/GEN-04F_siemens_star.md)、[GEN-04G](op_specs/GEN-04G_linear_rgb_bars.md) |
| [GEN-05 gradient coordinate](op_specs/GEN-05_gradient_coordinates_contract.md) | geometry→scalar t[H,W] | linear/radial/angular/diamond/box/path-distance | 几何与颜色表分离；几何参数合法性由调用方保证；成员：[GEN-05A](op_specs/GEN-05A_linear_gradient_coordinate.md)、[GEN-05B](op_specs/GEN-05B_radial_gradient_coordinate.md)、[GEN-05C](op_specs/GEN-05C_angular_gradient_coordinate.md)、[GEN-05D](op_specs/GEN-05D_diamond_gradient_coordinate.md)、[GEN-05E](op_specs/GEN-05E_box_gradient_coordinate.md)、[GEN-05F](op_specs/GEN-05F_path_distance_coordinate.md)、[GEN-05G](op_specs/GEN-05G_two_circle_radial_coordinate.md) |
| [GEN-06 gradient lookup](op_specs/GEN-06_gradient_lookup_contract.md) | t+数值表／颜色表→field/image | spread、数值 lookup 与颜色插值分离；颜色描述和 alpha 解释显式 | 端点/接缝/透明彩色中点；成员：[GEN-06A](op_specs/GEN-06A_spread_coordinate.md)、[GEN-06B](op_specs/GEN-06B_lookup_numeric_gradient.md)、[GEN-06C](op_specs/GEN-06C_gradient_color.md) |
| [GEN-07 mesh/bilinear numeric field](op_specs/GEN-07_mesh_gradients_contract.md) | 网格/四角数值→通用数值张量 | 显式拓扑与插值基函数；不隐式解释颜色、alpha 或 gamut | 控制点值、边界连续、非方形画布；成员：[GEN-07A](op_specs/GEN-07A_bilinear_rectangle_gradient.md)、[GEN-07B](op_specs/GEN-07B_triangle_mesh_gradient.md)、[GEN-07C](op_specs/GEN-07C_bicubic_parameter_patch.md) |
| [GEN-08 point distributions](op_specs/GEN-08_point_distributions_contract.md) | bounds+seed+参数→DynamicPoints（静态 positions[P,2]、ids[P]、count[1]） | grid/jitter/Poisson disk；min_distance>0 | 最近距离、seed、有效前缀与静态容量上界；成员：[GEN-08A](op_specs/GEN-08A_grid_points.md)、[GEN-08B](op_specs/GEN-08B_jittered_grid_points.md)、[GEN-08C](op_specs/GEN-08C_poisson_rejection_points.md)、[GEN-08D](op_specs/GEN-08D_bridson_points.md) |

Linear用`dot(p-a,b-a)/|b-a|²`；radial用`|p-c|/r`，椭圆先变换局部坐标；angular用`atan2`并映射到周期，中心与接缝显式；diamond/box采用L1/L∞等值线。two-circle radial选择最大有效根；无可返回坐标默认 valid=0，可显式 reject；相同圆非法。退化为全参数解时仍须检查是否存在有限最大有效参数。SVG明确包含坐标系、变换和spread method，只有颜色数组与尺寸不足以决定二维渐变。[^svg]

颜色解释使用算子 SDK 的 `TensorDescription`，由每个消费算子的静态参数显式提供。Result 仅携带结构、样本和存储 owner。完整图像采用 planar straight 表示，alpha 为同一 tensor 内的独立平面；通道数量不决定颜色模型或 alpha 角色。

公开 `generation::gradient_color(document, color_ramp, method, spread_profile)` helper 先追加独立的 spread 节点，再追加传入的 CRV-06 节点；支持 RGB、XYZ、CMYK、Lab、LCh、OKLab、OKLCh、HSL、YCbCr 及 pi／rational-pi 共 15 个模型／单位变体。GEN-06C 组合现有 CRV-06，继承各模型的数值前置条件、静态检查、公共节点舍入及读取规则。RGB/RGBA 使用 straight 输入输出；正 exact alpha 舍入为零时，各 RGB 分量也必须舍入为零，这是调用方前置条件。满足条件的透明输出统一为 +0，数值前置条件违例不构成运行时错误验收。Lab 使用 `l=L*/100`，极坐标保留原始 hue 与圈数。CRV 的逻辑 tuple 描述使用 `channel_axis=0`；下游 FMT 消费 `[H,W,C]` 时显式提供 `channel_axis=2` 的描述。CSS Color 4 提供命名语义来源。[^css]
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
| [NOI-06 fractal](op_specs/NOI-06_fractal_contract.md) | coordinates+[footprint]→field | basis、octaves、frequency、lacunarity、gain 显式；fBm/turbulence/ridged分别具名 | O(P·octaves)；按采样足迹截断 octave；不声称严格带限；成员：[NOI-06A](op_specs/NOI-06A_fbm_sum.md)、[NOI-06B](op_specs/NOI-06B_turbulence_sum.md)、[NOI-06C](op_specs/NOI-06C_ridged_sum.md)、[NOI-06D](op_specs/NOI-06D_footprint_fbm.md) |
| [NOI-07 blue noise](op_specs/NOI-07_blue_noise_contract.md) | rank tile/volume+coords→field/points | 通用 rank lookup 与具名 blue/STBN 资源分离 | PSD低频抑制、周期重复；生成点集不等于像素mask；成员：[NOI-07A](op_specs/NOI-07A_blue_rank_tile.md)、[NOI-07B](op_specs/NOI-07B_blue_threshold_points.md) |
| [NOI-08 Poisson/shot](op_specs/NOI-08_poisson_contract.md) | 非负期望计数或期望电子数→同形状 Int64 计数 | 上游显式完成电子单位标定；read noise、gain、clipping 和 ADC 各自显式处理 | 有限随机网格分位数近似；与理想 Poisson 均值/方差分别说明；成员：[NOI-08A](op_specs/NOI-08A_poisson_icdf.md)、[NOI-08B](op_specs/NOI-08B_shot_electrons.md) |
| [NOI-09 speckle/grain](op_specs/NOI-09_grain_contract.md) | signal+model→field/image | multiplicative speckle与艺术film grain独立 | grain 显式 none/sum/l2；strength 仅作最终乘数；隐藏 RGB 同样处理、alpha 按位复制；成员：[NOI-09A](op_specs/NOI-09A_speckle_integer_looks.md)、[NOI-09B](op_specs/NOI-09B_artistic_linear_grain.md) |
| [NOI-10 continuous temporal noise](op_specs/NOI-10_temporal_contract.md) | x,y,t→field sequence | 连续维noise或时空blue，time单位显式 | 逐帧独立随机图不表示连续演化；成员：[NOI-10A](op_specs/NOI-10A_advected_gradient2d.md)、[NOI-10B](op_specs/NOI-10B_temporal_perlin3d.md)、[NOI-10C](op_specs/NOI-10C_stbn_rank_volume.md) |

Perlin2002的固定gradient/permutation与五次fade为明确可实现的版本，换hash或seed展开就改变图样，应纳入算法身份。[^perlin] Random123的counter-based方法适合按全局坐标取样；整数序列和正态浮点变换的跨后端一致性分别定义。[^random] 时空blue noise具有独立时间频谱设计，不能以每帧新2D蓝噪声替代。[^blue]

随机寻址由 seed、stream、绝对坐标、channel 及 frame/draw 等字段构成，不依赖tile顺序或局部ROI原点。cache依赖seed和全部生成参数。signed 噪声输出数值 field；颜色消费者通过显式静态描述解释样本，输出 Result 不附带颜色描述。
Philox4x64-10 的全局 x/y 为 signed32，frame/draw/stream 取 uint32 范围。生产 v1 counter 按低位到高位依次打包为 c0=(x,y)、c1=(frame,draw)、c2=(stream,channel,domain)、c3=0；channel 占 16 位，domain 占 8 位，其余 72 位固定为 0；地址范围由调用方保证，运行时不检查。key=(seed_bits,0)，完整保留 seed 的 64 位比特。

Float32 均匀数取输出 word 0 的高 24 位 j，返回精确的 j/2^24。Float64 均匀数取 word 0 的高 53 位 j，返回精确的 j/2^53。open52 取所选输出字的高 52 位 j，返回 (2j+1)/2^53。

Box–Muller 每通道使用同一 block 的 word 0 生成 open52、word 1 生成 halfopen53，word 2/3 不使用；domain 2 与艺术颗粒的 domain 10 共用此规则。jitter、有限候选拒绝、Bridson、cellular 的两个随机量也使用 word 0/1。单次采样 draw=0，speckle 的第 j 个 look 用 draw=j；Poisson 与 speckle 均取 word 0 的 open52。

有限候选拒绝把候选序号 i 编码到 x，y/channel/frame/draw 为 0。Bridson 初始点用 x=y=0，第 i 轮第 a 次尝试用 x=i+1、y=a，channel/frame/draw 为 0；max_iterations 范围为 0..2^31-1。gradient 用 word 0 低 3 位选择既定梯度。

正 Int64 上界 n 的整数采样令 t=2^64 mod n，只取 word 0，拒绝 w<t，否则返回 w mod n；重试逐次增加 draw，禁止回绕。整数采样消费者仍须定义地址分配和尝试预算。oracle 的精确映射参考与 Bridson 的 Measured 三角函数结果分别标注。位分配以[随机契约](op_specs/NOI_random_contract.md)为准。
DynamicPoints v1 使用 `photospider.ops.dynamic_points`：Float64 `positions[P,2]`、Int64 `ids[P]` 和 Int64 `count[1]`。只有前 count 行有效，ID 非负且唯一；生成器按输出顺序编号，后续筛选可保留原 ID。未使用行的 positions 为 +0、ids 为 0。拒绝采样和 Bridson 的容量为 P=max(1,max_count)，零目标以 count=0 成功返回，不取随机样本；positions 的末尾 x/y 轴为原子轴，三个非图像成员组成一个 CompleteBundle，完整覆盖后共同交付；ABI 8 中需要 lowering 显式写满所有成员，再按成功 call 退休规则发布。物理存储遵循现有 stride/offset 接口。独立 report 使用 `photospider.ops.point_generation_report` v1，成员为 Int64 `stop_reason[1]` 和 UInt8 `points_object_id[8]`：0=count_reached、1=candidates_exhausted、2=active_exhausted；错误仍以失败返回。points_object_id 以小端字节保存完整 points 对象身份。GEN-08C/D 用两阶段 workflow 先生成最终 points，再由下游节点绑定 report，最后完整返回同一个 points 值；绑定后复制或部分投影会改变身份，不能继续使用原关联。报告绑定节点声明 deterministic=false，仅对象 ID 字节允许跨 Run 不同；点值、顺序、数量和停止原因仍确定。该关联仅适用于对应进程内对象。

有限候选拒绝与 FIFO Bridson 分别定义；max_count 是正常停止目标，候选／active list
耗尽可成功返回较少点，独立资源预算耗尽按错误处理。具名 blue/STBN 需要生产资源
与独立质量验收；普通 rank 结构合法不代表已经满足频谱质量。

## 梯度与分形的实现和优化

GEN-05A～E、GEN-06A/B 和 NOI-06A～D 均提供 strict、Apple Silicon、x86-64 三个 CPU profile；加速 profile 按目标架构准入。Float32 快路径通过完整结果区间两端舍入一致证明正确舍入；accelerated Float64 使用 NUM 的最终 FP32 四 ULP 上限，不能把误差额度分配给各 octave。无法证明时使用精确算术，工作量耗尽和取消直接保留失败状态。

NOI-06 的 octave 坐标、权重与 basis 都在最终舍入前保持精确。`gain=0` 不构造后续坐标；footprint 按显式输入衰减，并使用未衰减振幅和作归一化分母。算法不会从 ROI 大小推断 footprint。默认 basis 为 `gradient2d_philox`，需显式 seed/stream；三维 Perlin 必须显式选择 `perlin2002_3d`，使用固定 permutation，不接收 gradient 专属随机参数。

## 使用与验收

`noise→range→mask/color ramp/displacement`可复用到纹理、转场、选择与液化；AE Cell Pattern官方列出matte和displacement用途，可作为使用面锚点。[^ae] `shape→coverage→colorize→over`同时服务选区与绘制。普通图形首版无需额外实现颜色合成。

解析测试覆盖坐标identity、线性渐变0/1端点、合法半径边界、repeat/reflect、全局seed一致；随机统计使用固定样本数与置信阈值，不能要求每次精确达到理论均值。shape/path coverage的误差用面积/轮廓检查，不仅看图。

Regional、Halo 和 complete-input 描述数学依赖范围；实现使用 fixed invocation ABI 8 的静态 task、完整成员 tile reads/writes 和显式 COPY。算子按输入各自的 tile grid 声明读取闭包，仅检查静态结构，运行时 payload 合法性由调用方保证。成功 call 的输出在 retirement 时发布，所需 task 失败时不发布该 call 的部分输出。

CPU 是必需后端，GPU 按成员与 backend 完成独立实现和验证；同版本同 profile 的数值、数量、顺序与 ID 必须确定。保留 oracle 的通过结果仅说明所测数学参考行为。

## 来源

[^svg]: W3C，[*SVG2 Paint Servers*](https://www.w3.org/TR/SVG2/pservers.html)，2018-10-04 CR；渐变几何与spread。
[^css]: W3C，[*CSS Color4 Interpolation*](https://www.w3.org/TR/css-color-4/#interpolation)，颜色/极坐标/alpha插值。
[^perlin]: Ken Perlin，[*Improved Noise reference implementation*](https://mrl.cs.nyu.edu/~perlin/noise/)，2002。
[^random]: D. E. Shaw Research，[*Random123*](https://github.com/DEShawResearch/random123)，SC11论文2011及官方实现。
[^blue]: Wolfe等，[*Spatiotemporal Blue Noise Masks*](https://research.nvidia.com/publication/2022-07_spatiotemporal-blue-noise-masks)，NVIDIA Research，2022-07-06。
[^ae]: Adobe，[*Generate effects: Cell Pattern*](https://helpx.adobe.com/after-effects/desktop/apply-effects-and-animation-presets/list-of-effects/generate-effects.html)，滚动英文指南。
