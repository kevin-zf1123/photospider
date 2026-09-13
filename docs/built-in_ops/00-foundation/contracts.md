# 公共数据与执行约定

2026-09-13：本页对齐 package 0.10.0 / C operation ABI 9，合并依据见
[当前实现](current-state.md)。已实现契约以英文 ADR、公开头文件和
`docs/kernel-architecture/` 为准；下文明确标注的建议及未实现功能保持 Proposed。
分类页中的参数是研究建议，调用已实现节点时须遵循对应实现契约。

## 数据对象与输出

| 对象 | 当前承载 / 逻辑形状 | 必须附带的解释 |
| --- | --- | --- |
| scalar / sampled signal | Value `[1]` / `[N,C]`，单通道可约定 `[N]` | 自变量、间距/positions、端点、单位；已有 computed scalar 可连接 bounded 输入 |
| image / image plane | typed Float32 HWC / HW ImagePlane | 颜色模型、通道角色、transfer、primaries/white、reference、alpha；plane origin/step 与实际 source support 分开 |
| mask / scalar / vector field | HW coverage Float32 [0,1]；signed field 与 vector 按各端口契约 | coverage/probability/fuzzy 区别、单位、分量坐标；不将 depth/导数冒充 mask |
| ComplexField | typed Float32/Float64 Value `[H,W,2]`，Real/Imag 角色 | 已有通道提取/合并；这类 dense field 不自动具有 FFT 的原尺寸、packing 和变换 basis |
| Spectrum | Result 的 Float64 complex-pair records | 原 shape、axes/order、packing、sign/norm、shift、sampling、real policy；没有新增 complex dtype |
| Bands / PathSet / DynamicPoints / Components | 有版本 schema 的多字段 Result | 成员身份、拓扑、有效 count、ID basis、关联与 descriptor；空字段允许零 rows |
| Layer / Response / WeightedSum | 独立的结构化 Result family | CoverageRGBA 与 emission、Q/T response、mass 与 weight 分开；不能只凭 shape 互换 |
| independent 1D LUT / coupled RGB 3D LUT | `[N,C]` / `[Nr,Ng,Nb,3]` 的逻辑设计 | 同轴逐通道与三轴耦合不同；已有 1D 子集，通用 3D LUT 仍 Proposed |
| sequence / deep samples | 帧索引集合 / offsets+samples 的后续设计 | timestamp/timebase、缺帧、sample depth/opacity；专用 schema/算子仍 Proposed |

静态命名多输出使用 `WorkflowNodeOutput{node, port}` 和
`WorkflowOutput{name, node, port}`，各输出可有不同 shape、Region 和依赖。
pure 算子的未请求端口不执行，`enable_joint` 仅选择可选物理优化。
动态 count 和多字段全局对象使用编译器可见 `SchemaTemplate`、`OperationPortKind::Result`
和不可变 `ResultRef`。Value 的各轴仍为正，不能用零长度 Value 模拟空集合。
终端 RequestRecord 不能替代可组合 Result。

Result 的 `CompleteBundle` 在 seal 前不可见；`StablePrefix` 和 `IndependentChunks`
当前发布有序字段前缀，每次发布均须完成数据、控制、校验和 descriptor 义务。
旧 descriptor 快照不随之后发布增长。关联使用准确 ObjectId；相同 shape 或数值
不能代替对象关联。源字段和 descriptor 支持分别保留，空集合仍有 count/basis 校验依赖。
见[多输出](../../kernel-architecture/Multi-Output-Operations.md)、
[全局结果](../../kernel-architecture/Global-Results.md)、
[结构化表示](../../kernel-architecture/Structured-Representations.md)。

## 颜色、alpha 与 emission

颜色模型、通道顺序、primaries/white、transfer、scene/display reference、亮度单位、
alpha association、dtype 与量化范围分别定义。YCbCr 还需 matrix/range/subsampling/siting。
已有 `color.assign`、RGB/XYZ/Lab 和 alpha 转换仅覆盖其明确输入域；ICC/OCIO、
任意 transfer、tone/gamut mapping 和外部 profile 转换仍需专用实现。

CoverageRGBA 保存 premultiplied P 和 A，finite signed/HDR P 合法，A∈[0,1]，A=0⇒P=0。
Layer 另外保存 finite signed emission E，允许 A=0、E≠0。当前 Layer space 1 固定为
linear-sRGB、D65、scene-relative；不会隐式转换颜色空间。
颜色非线性处理一般使用 unassociate→transform→associate；emission 有独立含义，
不能除以 coverage alpha。任意旧 image 算子不自动成为 Layer 算子。

已实现 Layer over 分别计算 `(Ps+Ts*Pb, As+Ts*Ab, Es+Ts*Eb)`，Ts=1−As，
各指定 primitive 按 binary32 nearest ties-to-even、gradual underflow 和无 FMA contraction 舍入。
Coverage opacity 缩放 P/A 并保留 E；front/behind emission 使用各自明确遮挡公式。
Flatten 在显式 opaque 背景 B 上计算 `(P+E)+(1-A)*B`，输出 A=1 的 Whole RGBA。
Response 只保存 Q=P+E、T=1−A，不能恢复原 P/A/E；两种代数的数值成功域不同，
编译器不隐式互换。RawSum 的 alpha mass 与加权平均的 W 分开；weighted reduce
使用固定 ordinal midpoint tree，W=0 finalization 返回 OptionalLayer 的 valid=false。
关联下溢不通过清零 P、改写 emission 或 epsilon 修补。
见[Layer runtime](../../kernel-architecture/Layer-Runtime.md)。

## 坐标与依赖映射

数组按 y,x,c，几何向量按 x,y。默认设计采用像素中心 `(x+.5,y+.5)`、左上原点、y 向下；
已实现 STMap/coordinate 使用该约定。ImagePlane 的 nominal origin 是源像素索引坐标，
必须按该 plane 契约解释。角度单位、方向、pixel aspect 和 window 逐算子定义。
边界扩展作用于完整逻辑图像；constant/clamp/reflect/mirror/wrap 的支持以各节点为准。

| 简记 | 数学依赖 | 当前落地约束 |
| --- | --- | --- |
| E | 同像素/样本 | Elementwise 仍服从 shape/facet 规则；多输出按各端口定义 observation |
| H(r) | 有限邻域 | 静态 Halo 或 staged 精确邻域；radius 不自动等于全算法 halo |
| W | 全局计算 | 旧 Whole Value 必须容纳完整物化；structured recipe 可借 mandatory disk backing 分页，所有预算仍生效 |
| R | 任意源采样 | 已有 STMap、radius gather/scatter 和按端口 staged reads；其他 sampler/warp 需自己的 data/control/validation 关系 |
| S | scan/reduce/递推 | 已有 ordered scan 和具名全局 recipe；跨 tile 状态、顺序和发布 finality 逐算法证明 |
| T | 多时间点 | 仍需专用序列、时间窗与边界帧契约 |

protocol 1 的 `DependencyCertificate` 为 Exact-only。protocol 2 的 `ResultRelation`
可表示 Exact、Conservative、Unknown；Conservative 经组合保持 Conservative，Unknown
在 dirty 查询中为 Unresolved。它们不可伪装成旧 Exact 证书。统计、FFT、分页连通域
和 Layer 当前各有明确 Conservative support，不能据“已分页”宣称精确局部重算。

每个输出分别声明每个输入的 data/control/validation/descriptor demand、坐标映射、
返回 Region/布局/owner 和 dirty 正向传播。LUT/kernel 可全表读取、图像可局部读取；
已有 G4 接口支持这种拆分，但旧 `field.apply_lut_1d` 等 Whole 节点并未自动升级。
见[依赖采样](../../kernel-architecture/Dependency-Sampling.md)和
[Region](../../kernel-architecture/Region-Semantics.md)。

## 资源、缓存与生命周期

structured CPU 执行要求 `ExecutionContextConfig::managed_resources`。
共享 root 统一准入 Host/Device/Shared/Metadata/Referenced/Payload、临时 Disk、entries/files、
I/O slots/queue，以及累计 work/bytes/requests/stages；`maximum_live_bytes` 仍是 Payload 子限额。
重叠维度不能相加解释为物理内存。先预留再分配，增长计入新旧同时存活容量。
已提交 work、stage 和 I/O 在失败/取消后不退还；不足时返回 ResourceExhausted。

mandatory backing 独立于可选 completed cache；关闭 cache 仍需保留活动消费者的结果。
派生 Result 保守保留直接输入 ObjectIds/ResultRefs，可能保留整个祖先链。
结果和 owning read window 可超过 ExecutionContext 生命周期，最后 owner 释放 backing。
I/O 由 continuation yield 显式计划后执行，不能在 structured callback 中隐式阻塞读写。

每份规格给输出、scratch、阶段 state、页窗口、临时文件、校验成本和峰值同时存活对象，
同时给算法 work/stage 上界或显式拒绝条件。例如整数直方图有扫描 stage 下界准入，
FFT 有两代完整临时 complex backing，连通域有独立于最终 K 的 union backing。
`WithinBudgetOrFail` 覆盖受管理容量；allocator 内部、legacy 未埋点 metadata、线程栈、
驱动及 OS cache 等属于排除范围。实测 managed peak 不代表 RSS 上界或完成保证。
详见[Managed resources](../../kernel-architecture/Managed-Resources.md)。

执行身份包含 operation contract、参数、ordered inputs、冻结输入身份和 canonical schema；
Result 的页大小、文件偏移、descriptor revision 和资源限额不进入语义身份。物理计划选择
保留在 physical plan identity。外部 LUT/profile/model 由宿主载入不可变快照，路径字符串
本身不能代表内容身份；后端/质量选项按对应节点契约参与身份。

## 错误与数值质量

规格分别记录 `Status.code` 分类、`reason` 原因和 `detail` 来源/范围；诊断字符串不作分支条件。
Atom、ValidationDomain、Association 为语义失败范围；Group、Run、Waiter 记录运行影响范围。
失败保留实际 upstream node/input 身份。host read/work/allocation 失败是 sticky，callback
不能返回成功掩盖它。发布前检查整个 envelope 与关联字段；失败不得附带成功 Value/证书。
普通 `execute` 保持 fail-fast；`execute_atoms` 对满足 Atomic/PerAtomOutcome 的 managed CPU
Value plans 收集独立 observation；structured Result 按自身 finality 策略发布。
已认证前缀可在之后运行失败时保留，不能称作完整成功结果。

`QualityReport` 区分 Measured 与 CertifiedBound。有限 residual 包括零 residual 都不自动
成为误差证明；当前 CertifiedBound factory 仅覆盖受限整数对角系统及匹配的 Float64 estimate。
依赖 Exact、数值误差界、受管理容量是不同保证。见
[Atom errors and quality](../../kernel-architecture/Atom-Errors-and-Quality.md)。

未实现随机算子的建议仍为 seed/global coordinate/channel/frame/stream 的 counter-based 序列，
并单列浮点变换的后端误差；没有新增随机 registry key。[^random]

## 公共验收

每项节点规格提供实际公开入口 workflow、独立 oracle、输入/参数、可检查预期结果和运行方法。
有限浮点的 `atol=1e-6, rtol=1e-5` 仅可作新研究候选；已有算子的精确舍入、bitwise 或
特定容差契约优先。局部算法按需要检查 whole/ROI/tile；全局 Result 检查页大小、动态/空 count、
关联、cache-off、取消、低预算、上下文销毁后寿命。只报告本轮实际运行的验证。

[^random]: John K. Salmon 等，*Parallel Random Numbers: As Easy as 1, 2, 3*，SC11，2011；[作者与项目页](https://random123.com/)。原研究参考；Photospider 随机键布局仍为建议。
