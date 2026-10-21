# 公共数据与执行约定

本页保留研究期数据、空间、颜色与数值设计约定。涉及当前运行行为时，以 0.39.0 package 的公开头文件和[kernel 架构指南](../../kernel-architecture/README.md)为准，尤其是[数据模型](../../kernel-architecture/Data-Model.md)、[依赖采样](../../kernel-architecture/Dependency-Sampling.md)、[资源](../../kernel-architecture/Managed-Resources.md)和[全局结果](../../kernel-architecture/Global-Results.md)。本文中的 Proposed 设计不是当前实现承诺；分类页参数仍须与已实现节点的具体规格区分。

## 数据对象与输出

| 概念对象 | 研究期形状示意 | 规格需要说明的语义 |
| --- | --- | --- |
| scalar / sampled signal | Tensor `[1]` / `[N,C]`，单通道可约定 `[N]` | 自变量、间距/positions、端点、单位；已有 computed scalar 可连接 bounded 输入 |
| image / image plane | typed Float32 HWC / HW ImagePlane | 颜色模型、通道角色、transfer、primaries/white、reference、alpha；plane origin/step 与实际 source support 分开 |
| mask / scalar / vector field | HW coverage Float32 [0,1]；signed field 与 vector 按各端口契约 | coverage/probability/fuzzy 区别、单位、分量坐标；不将 depth/导数冒充 mask |
| ComplexField | typed Float32/Float64 Tensor `[H,W,2]`，Real/Imag 角色 | 已有通道提取/合并；这类 dense field 不自动具有 FFT 的原尺寸、packing 和变换 basis |
| Spectrum | Result 的 Float64 complex-pair records | 原 shape、axes/order、packing、sign/norm、shift、sampling、real policy；没有新增 complex dtype |
| Bands / PathSet / DynamicPoints / Components | 有版本 schema 的静态多 Tensor Result | 成员身份、拓扑、有效 count、ID basis、关联与 descriptor；静态容量与有效 count，空集合用 count=0 表达 |
| Layer / Response / WeightedSum | composite SDK 的值类型 | CoverageRGBA 与 emission、Q/T response、mass 与 weight 分开；不能只凭 shape 互换 |
| independent 1D LUT / coupled RGB 3D LUT | `[N,C]` / `[Nr,Ng,Nb,3]` 的逻辑设计 | 同轴逐通道与三轴耦合不同；已有 1D 子集，通用 3D LUT 仍 Proposed |
| sequence / deep samples | 帧索引集合 / offsets+samples 的后续设计 | timestamp/timebase、缺帧、sample depth/opacity；专用 schema/算子仍 Proposed |

静态命名多输出使用 `WorkflowNodeOutput{node, port}` 和
`WorkflowOutput{name, node, port}`，各输出可有不同 shape、Region 和依赖。
pure 算子的未请求端口不执行。
所有端口都传递 Result，每个 Result 的 Tensor 成员名称、数量、dtype、rank 和正 extent 在 preparation 后固定。数据依赖数量使用静态容量与普通 payload，例如 `records[N,R]` 和 `count:Int64[1]`，或同形状数据加 mask。Count、mask、offsets 没有内核特殊含义，读取它们需要编译后的 payload grant；descriptor-only 不提供这些值。有效数量可以为零，shape 仍保持正 extent。每个公开 tile 完整初始化，未使用位置按算子规则填充。未生产 tile 不缩短数组，要求完整结果的算子在容量不足时失败。

Planner 根据成员 publication policy 编译固定 selection 闭包；每个 task 完整初始化其声明的输出 tile。Task 完成后可向内部后继转发；CallGroup 在全部成员 task settle 后按 IR 顺序退休，成功时才向 observer 交付声明的 Result exports。共享计算使用显式 task 和 call 私有 value。外部 ResultBuilder 的 coverage/capture 规则独立于 call 退休，旧 descriptor 不随之后生产增长。见[全局结果](../../kernel-architecture/Global-Results.md)。

## 颜色、alpha 与 emission

普通图像算子默认采用 straight alpha，宿主 backing 默认 tiled planar。调用方不需要声明已遵循这些默认约定，内核不扫描认证，也不自动 associate/unassociate。以下 CoverageRGBA、Layer 等显式领域类型仍遵循自身 premultiplied 数学。

颜色模型、通道顺序、primaries/white、transfer、scene/display reference、亮度单位、
alpha association、dtype 与量化范围分别定义。YCbCr 还需 matrix/range/subsampling/siting。
颜色描述作为算子的显式静态参数传入；Result/Tensor 不附着或传播描述。
描述编辑由 SDK 纯函数完成。RGB/XYZ/Lab、transfer 与 alpha 转换各自限定输入域；
ICC/OCIO 资源封装不代表已实现完整 CMM、tone/gamut mapping 或任意外部 profile 转换。

CoverageRGBA 保存 premultiplied P 和 A，finite signed/HDR P 合法，A∈[0,1]，A=0⇒P=0。
Layer 另外保存 finite signed emission E，允许 A=0、E≠0。这些类型和纯函数由 `Photospider::ops_sdk` 提供；调用者选择一致的线性颜色空间，
SDK 不定义固定 space id，也不隐式转换颜色空间。
颜色非线性处理一般使用 unassociate→transform→associate；emission 有独立含义，
不能除以 coverage alpha。任意旧 image 算子不自动成为 Layer 算子。

已实现 Layer over 分别计算 `(Ps+Ts*Pb, As+Ts*Ab, Es+Ts*Eb)`，Ts=1−As，
各指定 primitive 按 binary32 nearest ties-to-even、gradual underflow 和无 FMA contraction 舍入。
Coverage opacity 缩放 P/A 并保留 E；front/behind emission 使用各自明确遮挡公式。
Flatten 在显式 opaque 背景 B 上计算 `(P+E)+(1-A)*B`，返回 A=1 的 `CoveragePixel`。
Response 只保存 Q=P+E、T=1−A，不能恢复原 P/A/E；两种代数的数值成功域不同，
编译器不隐式互换。RawSum 的 alpha mass 与加权平均的 W 分开；weighted leaf/add 不选择归约树，调用者决定合并顺序；W=0 finalization
返回 `OptionalLayerPixel` 的 valid=false。
关联下溢不通过清零 P、改写 emission 或 epsilon 修补。
见[Layer runtime](../../kernel-architecture/Layer-Runtime.md)。

## 坐标与依赖映射

数组按 y,x,c，几何向量按 x,y。默认设计采用像素中心 `(x+.5,y+.5)`、左上原点、y 向下；
已实现 STMap/coordinate 使用该约定。ImagePlane 的 nominal origin 是源像素索引坐标，
必须按该 plane 契约解释。角度单位、方向、pixel aspect 和 window 逐算子定义。
边界扩展作用于完整逻辑图像；constant/clamp/reflect/mirror/wrap 的支持以各节点为准。

| 简记 | 数学依赖 | 当前落地约束 |
| --- | --- | --- |
| E | 同像素/样本 | Elementwise 仍服从 shape 规则；多输出按各端口定义 observation |
| H(r) | 有限邻域 | 编译期固定 Halo 或有限邻域并集；radius 不自动等于全算法 halo |
| W | 全局计算 | 完整输入由 lowering 声明；共享计算使用固定 task 和私有 value，输出与 scratch 均需预算 |
| R | 任意源采样 | STMap 使用完整 source 静态授权；其他 sampler/warp 需证明合法地址的有限静态范围 |
| S | scan/reduce/递推 | 已有 ordered scan 和具名全局 recipe；跨 tile 状态、顺序和发布 finality 逐算法证明 |
| T | 多时间点 | 仍需专用序列、时间窗与边界帧契约 |

数学支持集用于在编译期推导固定成员 tile reads。Lowering 明确声明 task 的 payload reads、schema reads、writes 和额外 ordering dependencies；input projection 不能替代读取边。运行时 invocation 只接收固定 views，不增加 producer 或扩张依赖。数据相关地址必须由有限静态 superset 覆盖；schema-only read 不提供 count、mask 等 payload。每个输出说明其闭包、view 映射及共享中间 task；数值上只使用局部 stencil 不代表已编译读取范围同样局部。

## 资源、缓存与生命周期

`ExecutionContext` 持有共享 ResourceBudget Root；`managed_resources` 可指定限额，省略时使用 `ResourceLimits{}`。当前维度为 Host、Device、Shared、Metadata、Referenced、Entries、Queue 和 Payload；`maximum_live_bytes` 是 Payload 子限额。Host 包含 Metadata 和 Shared，Device 包含 Shared，重叠维度不能相加为物理内存。每个 block 在发射前原子预留全部输出及各 backend lane 的最大 scratch；容量不足返回 `ResourceExhausted`。累计 work 和 stage 准入后不退还。Metadata 分别报告 reserved capacity 与已成功分配的受控字节，Run attribution 保留到对应 owner 释放。

执行器没有跨 Run 的 Result、checkpoint 或 block cache。PreparedPlan 共享不可变准备数据，各 Run 持有独立的 bindings、activation 和工作状态。Result association 记录有序源 ObjectId；需要源 backing 的 view 保留对应 owner。已接受 invocation 的 inputs、outputs、scratch 和 completion storage 保持到真实完成，取消不能提前释放。跨 domain 使用显式 COPY。Result 与 owning read window 可超过 `ExecutionContext` 生命周期，最后一个实际 owner 释放 backing。见[Global Results](../../kernel-architecture/Global-Results.md)。

每份规格说明输出、scratch、准备数据、校验成本、最大在途 task 和峰值同时存活对象，并给出固定工作上界或拒绝条件。全局算法需要的临时值须进入物理 Plan；最终输出较小不代表临时 backing 同样小。`WithinBudgetOrFail` 覆盖受管理容量，排除 allocator 私有开销、普通 shared control block、线程栈、provider opaque context、驱动和 OS cache；受控计费不等同于 RSS。见[Managed resources](../../kernel-architecture/Managed-Resources.md)。

IR identity 包含静态 operation contract、参数、schema、输入关系和控制流；Plan identity 还包含固定 reads/writes、placement、layout、依赖、selection 和资源界。运行期 completion 使用 Run、activation、submission 键。外部 LUT/profile/model 由宿主载入不可变快照，路径字符串不能代表内容身份；payload owner 和地址不写入 artifact。

## 错误与数值质量

规格分别记录 `Status.code`、`reason` 和 `detail`；诊断字符串不作为分支条件。Task 操作失败形成 call failure cut：更早 call 继续完成并有序退休，cut 及之后未提交的工作被跳过，已接受工作排空。同一 call 从实际失败 task 中选择最小 TaskId。用户取消、Stale、device/context/observer 失败按 Run abort 契约处理。`join()` 等待 task quiescence 与 observer settlement；失败不返回最终成功结果。此前已退休的 call Result 保持有效。数值输入前提、质量检查与 typed failure 由各算子固定 kernel 契约定义。

ops SDK 的 `QualityReport` 区分 Measured 与 CertifiedBound。有限 residual 包括零 residual 都不自动
成为误差证明；当前 CertifiedBound factory 仅覆盖受限整数对角系统及匹配的 Float64 estimate。
依赖 Exact、数值误差界、受管理容量是不同保证。见
[Atom errors and quality](../../kernel-architecture/Atom-Errors-and-Quality.md)。

未实现随机算子的建议仍为 seed/global coordinate/channel/frame/stream 的 counter-based 序列，
并单列浮点变换的后端误差；没有新增随机 registry key。[^random]

## 公共验收

每项节点规格提供实际公开入口 workflow、独立 oracle、输入/参数、可检查预期结果和运行方法。
有限浮点的 `atol=1e-6, rtol=1e-5` 仅可作新研究候选；已有算子的精确舍入、bitwise 或
特定容差契约优先。局部算法按需要检查 whole/ROI/tile；全局 Result 检查页大小、静态容量下的零/非零 count、
关联、独立 Run、取消排空、低预算准入、上下文销毁后寿命。

[^random]: John K. Salmon 等，*Parallel Random Numbers: As Easy as 1, 2, 3*，SC11，2011；[作者与项目页](https://random123.com/)。原研究参考；Photospider 随机键布局仍为建议。
