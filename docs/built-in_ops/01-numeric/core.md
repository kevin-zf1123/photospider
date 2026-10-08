# 数值与数组基础

2026-09-20：`ops-impl` 的 NUM-01～15 运行时实现已进入当前 public registry，
实现事实按功能簇记录于[实现进度](implementation.md)。本轮各簇的 workflow、独立
oracle、Clang 与 installed-consumer 证据按对应 README/实现记录区分，剩余验收边界
不由规格的 Proposed 状态推断。

2026-09-13：G4 已有 `numeric.radius_gather/radius_scatter` 与 ordered scan；Phase A 提供 `statistics.histogram/parameters/grade` 的受预算分页全局流程。见[采样](../../kernel-architecture/Dependency-Sampling.md)、[Region](../../kernel-architecture/Region-Semantics.md)、[整数统计](../../kernel-architecture/Integer-Statistics.md)。分页统计为显式注册 factory，不能替代任意 numeric reduction。

已实现的基础子集、精确参数和 Region 见[基础算子实现](../../kernel-architecture/Basic-Operations.md)。规格表中的 Proposed 表示契约状态，不表示当前 runtime 未注册；NUM-01～15 与 CRV-01～11 合计 330 个 primitive keys 已进入当前 public registry，详细实现事实和验证边界见[实现进度](implementation.md)及各 workflow README。分类表中的建议参数不覆盖现有接口。

已接受首版见 [ADR 0020](../../adr/0020-composable-operation-foundations.md)：四 dtype cast/range 分离，Float32/64 同 shape 基础算术和显式 clamp，全数组 Float64 mean/variance，有界单通道 expression（静态 start/step/count、动态 Float64 coefficients）及 linear 1D LUT。未带后缀的 `numeric.add`、`subtract`、`multiply`、`divide` 与 `clamp` 现使用 Whole Result：每个输入是无 fields、恰含一个 tensor 的 Result，按完整 `sample_shape()`（含 batch）匹配 Float32/64；输出 `value` 使用 `photospider.tensor` v1 / `samples`。非空请求声明完整输入的 role 13 needs，Empty 仅执行静态 preflight。Clamp 的有限 `min`、`max` 及顺序在 immutable preparation 中验证；算术拒绝非有限输入/结果，除法拒绝正负零。未带后缀的 `numeric.abs`、`numeric.minimum` 和 `numeric.maximum` 也使用 Result tensor，但通过 staged Dependency Needs 先验证输入再读取输出请求 Q，仅发布 Q；输入形状、类型和 typed closure 规则见[数值算子架构说明](../../kernel-architecture/Numeric-Operations.md)。这些有限 key 与独立注册的 suffixed Whole profiles 不同。`numeric.mean`、`numeric.variance` 和 `numeric.ordered_scan` 现以 Result tensor 为输入/输出，原有 named output port `value` 保留，schema 为 `photospider.tensor` v1 / `samples`。非空请求按分阶段 Needs 处理 row-major 区间或 prefix；Empty 执行静态 preflight 后返回没有 tensor coverage 的 sealed Result，且不发 input Need。Variance 保留两遍 population 语义，scan 保留正零起点和严格左折加法。Block state 通过相同 schema 的完整单 tensor Result 传递；仅 cacheable 且 selected-input producer closure 纯且确定、输入与状态没有附加 resources 时，成功 block 才可按当前状态和已供给输入内容复用；块计算不得依赖 ObjectId、semantic_key、association、原始 Q 或未供应的历史窗口。带 resources 的 state 仍可计算，但不进入 Value LRU。其他参数、数学与资源边界以各实现文档为准；规格接受状态继续为 Proposed。

当前已实现的数值算子与公开运行示例见[数值算子实现](../../kernel-architecture/zh/Numeric-Operations.zh.md)和[实现进度](implementation.md)。Expression/LUT 的实现与公开 workflow 见[实现文档](../../kernel-architecture/zh/Expression-and-LUT-Operations.zh.md)；下表 Proposed 只表示规格接受状态。

状态 Proposed。本篇为 D1 数学核心，受 G1/G3/G5 数据与组合前置条件约束。建议 CPU Float32/Float64 参考实现；整数支持逐项定义，不能默认为所有张量运算有 Metal 后端。符号 E/W/S 和默认约定见[公共契约](../00-foundation/contracts.md)。

## 基础目录

各独立规格显式继承 [NUM 共享契约](op_specs/NUM_common_contract.md)，统一记录
规格状态、注册与平台、参数格式、错误范围、资源和验收要求；具体算子的例外优先。

| ID / 提议操作 | 输入 → 输出 | 参数、语义与算法 | 依赖 / 验收 |
| --- | --- | --- | --- |
| NUM-01 expression generator family | Result 标量 inputs：start/end/具名系数 → `values` 与 `axis` Results | 输入 member key 任意，shape `[1]`，Float32/64 可混合；`values` dtype 静态选择，默认 Float64。输出为 `photospider.tensor` / `samples`，values shape `[N]`，axis 为 Float64 atomic trailing tuple `[3]`。Whole `values` 计算完整样本后投影；axis 只需有效端点。N=1 values 排除 end，axis 仅读 start；Empty 不读 payload。Strict 按每个 AST 步骤 RN64 舍入；accelerated 遵守最终 FP32-scaled ULP contract 并严格回退不确定样本。 | [具体规格](op_specs/NUM-01_sample_expression.md)，Proposed；当前 Result workflow 验证入口为 `test_numeric_expression_result` / `installed_numeric_expression_result`，见[执行页](expression-whole.md) |
| NUM-02 sequence generators | 单 tensor Result 标量输入 → `values` 与原子 `axis` Results | 每个输入 Result 恰有一个 `[1]` tensor member，key 任意。Linspace 接受 Float32/64；arange 接受同为 Int64 的整数对或 Float32/64 浮点对。静态 count 为 1..1048576，dtype 显式给出；输出分别为 `photospider.tensor` / `samples` 的 `[count]` 序列和 `[3]` 原子轴 tuple。Count=1 只运行时读取 start；其他非空输出 Whole 计算完整所选值或 tuple 后再投影，Empty 不读 payload、不做样本算术。 | [NUM-02A linspace](op_specs/NUM-02A_linspace.md)、[NUM-02B arange](op_specs/NUM-02B_arange.md)，Proposed；Result Whole 实现与当前验证边界见[实现页](sequences-whole.md)，行为入口为 `test_numeric_sequences_result` / `installed_numeric_sequences_result` |
| NUM-03 constant / broadcast families | scalar/array + shape → array | 已选定两个独立算子；constant 填充标量，broadcast 显式映射输入轴 | [NUM-03A constant](op_specs/NUM-03A_constant.md)、[NUM-03B broadcast](op_specs/NUM-03B_broadcast.md)，Proposed |
| NUM-04 unary families | array（rational 入口为 numerator/denominator）→同形 | abs/neg/sqrt/exp/ln/sin/cos/tan/sinpi/cospi/tanpi/sinc/sincpi 及四个精确 p/q·π rational 对应版本、floor/ceil/round/sign/reciprocal 独立命名；pow 归 NUM-05；允许 NaN/Inf 的 IEEE 风格行为，特殊值与精度逐项定义 | [共享契约](op_specs/NUM-04_unary_contract.md)，各函数分文件细化；Proposed |
| NUM-05 binary families | 两个单 tensor Result 输入 → `values` Result | 27 个 CPU Whole keys：add/subtract/multiply/divide/minimum/maximum/pow/atan2/atan2pi，各有 Strict/Apple/x86 命名 profile。每个输入 Result 含一个 slot 0 tensor member，完整 sample shape 与 dtype 必须匹配；输出保留完整 sample shape 和 dtype，按普通轴发布并丢弃 batch 拓扑及 facets。Broadcast/cast 显式处理。 | [共享契约](op_specs/NUM-05_binary_contract.md)，各函数独立规格；Proposed；当前 Result 实现与验证边界见[实现页](point-math-whole.md) |
| NUM-06 clamp / remap_range | Clamp 三个、remap_range 五个 `Result` tensor 输入 → `values` Result | 各输入 member 的完整 `sample_shape()`（含 batch 前缀）和 dtype 必须相同；输出使用完整 shape、丢弃 facets 与 batch 拓扑。Clamp 支持 UInt8/Int64/Float32/Float64，按比较选值并保留区间内位模式；remap_range 支持 Float32/64，以精确 rational 整式为数学定义，strict 精确舍入、accelerated 遵守 FP32 ULP contract，并允许外推。六个 formal keys 执行 Whole：非空 Need 覆盖所有输入的 Data/Validation/Descriptor，空请求不读 payload、不做样本运算。 | [NUM-06A clamp](op_specs/NUM-06A_clamp.md)、[NUM-06B remap_range](op_specs/NUM-06B_remap_range.md)，Proposed；Result Whole 实现与实测范围见[实现页](range-whole.md)，行为入口为 `test_numeric_ranges_result` / `installed_numeric_ranges_result` |
| NUM-07 comparisons / select | 单 tensor Result inputs → `photospider.tensor` / `samples` Result | 每个输入使用 slot 0，完整 `sample_shape()`（含 batch）必须匹配；非空 Whole 以 role 13 Need 读取授权窗口并发布完整普通轴 shape。六个谓词支持 UInt8/Int64/Float32/Float64 并输出 UInt8；`is_close` 支持 Float32/64，使用精确有理阈值。`select` 以 UInt8 condition 和同 dtype/shape 双分支为输入，先校验两分支并复制选中位模式。Empty seal 零 coverage 与 descriptor witness，不发 payload Need、不做样本计算。 | [精确比较](op_specs/NUM-07_comparison_contract.md)、[is_close](op_specs/NUM-07G_is_close.md)、[select](op_specs/NUM-07H_select.md)，Proposed；Result Whole 实现见[执行页](comparison-whole.md)，行为入口为 `test_numeric_comparisons_result` / `installed_numeric_comparisons_result` |
| NUM-08 mix / smoothstep | 三个动态同 shape/dtype 浮点 Result tensor → 一个 Result tensor | 每个输入 Result 含一个 tensor member，member key 任意；匹配完整 `sample_shape()`（含 batch）及 Float32/64。Whole Need 请求三个输入的 Data/Validation/Descriptor（role 13）；非空输出为 `photospider.tensor` / `samples`，完整 shape 为普通 axes，丢弃 facets 与 batch 拓扑。Mix 验证全域有限 `t∈[0,1]`，端点复制对应输入位模式但仍完整读取和验证两端输入；smoothstep 先验证有限递增边界，再执行固定三次曲线。数学契约与资源/错误语义见成员规格。 | [NUM-08A mix](op_specs/NUM-08A_mix.md)、[NUM-08B smoothstep](op_specs/NUM-08B_smoothstep.md)，Proposed；Result Whole 实现及验证边界见[实现页](interpolation-whole.md)，行为入口为 `test_numeric_interpolation_result` / `installed_numeric_interpolation_result` |
| NUM-09 reshape / transpose / slice | 单 tensor Result → `photospider.tensor` / `samples` Result | UInt8/Int8/UInt16/Int16/Int64/Float32/Float64 按位保留、元素数≤2^40；reshape 按逻辑行主序；transpose 静态轴排列；slice 动态 Int64 起点/步长、静态数量；Whole 发布完整全局坐标；auto/view/dense | [reshape](op_specs/NUM-09A_reshape.md)、[transpose](op_specs/NUM-09B_transpose.md)、[slice](op_specs/NUM-09C_slice.md)，Proposed；当前 Result 工作流与验证见[执行说明](layouts-whole.md)，行为入口 `test_numeric_layouts_result` / `installed_numeric_layouts_result` |
| NUM-10 concatenate / gather / scatter | Result tensors → `values` Result | 每个输入 Result 恰有一个 tensor member，key 任意；sample shape 含 batch 前缀，rank 1..8、元素数≤2^40。非空 Whole 对活动输入请求完整 Data/Validation/Descriptor（role 13）并计算完整输出；Empty 不读 payload、不做样本运算。Concatenate 支持 2..256 输入及 View/Dense；Gather 保留重复 Int64 indices 顺序；Scatter 全局校验索引并按稳定顺序选择贡献者。输出为 `photospider.tensor` / `samples`，普通完整 shape，丢弃 facets 和 batch 拓扑。 | [NUM-10A concatenate](op_specs/NUM-10A_concatenate.md)、[NUM-10B gather](op_specs/NUM-10B_gather.md)、[scatter 共用契约](op_specs/NUM-10_scatter_contract.md)，Proposed；Result Whole 实现与验证见[执行页](indexing-whole.md)，行为入口 `test_numeric_indexing_result` / `installed_numeric_indexing_result` |
| NUM-11 reduction family | 单 tensor Result → `values` Result | 每个 input Result 恰有一个 tensor member，key 任意；完整 sample shape 含 batch 前缀，rank 1..8、元素数≤2^40。输出保留 rank、reduced axes 置 1，使用 `photospider.tensor` / `samples`，丢弃 facets 和 batch 拓扑。六个数值归约非空时 Whole 请求完整 Data/Validation/Descriptor 并计算全部 groups；count 只依赖静态 schema/axes，不发 runtime input Need，输出 8-byte Int64 零步长 backing。Sum/min/max/mean/variance/std 的 NaN 顺序、精确和、ddof 与最终舍入见共享契约。 | [共享及独立规格](op_specs/NUM-11_reduction_contract.md)，Proposed；Result Whole 实现见[实现页](reductions-whole.md)，行为入口 `test_numeric_reductions_result` / `installed_numeric_reductions_result` |
| NUM-12 sort / quantile | `Result` tensor → sort 的 `values` / `indices` Results；source 与 `q` Results → quantile `values` Result | 每个输入 Result 恰有一个 tensor member，key 可变；`sample_shape()` 含 batch 前缀，rank 1..8、元素数≤2^40；输出将完整 shape 作为普通 axes 并丢弃 facets/topology。Sort 支持四种 dtype，静态 `axis`，稳定按数值和原索引排序；values 与 Int64 indices 独立选择、各自 Whole 计算。Quantile 静态 `axis`/`dtype`，q 为 Float32/64 `[1]` Result tensor；按精确二进制有理 rank 和最终舍入计算。六个 key 对活动输入执行 Whole Need（Data/Validation/Descriptor）；q 在 axis 长度 1 时静态排除；Empty 不读 payload、不做样本计算。 | [NUM-12A sort](op_specs/NUM-12A_sort.md)、[NUM-12B quantile](op_specs/NUM-12B_quantile.md)，Proposed；Result Whole 实现见[实现页](ordering-whole.md)，行为入口 `test_numeric_ordering_result` / `installed_numeric_ordering_result` |
| NUM-13 prefix_sum / integral_image | 单 tensor Result → `values` Result | 输入 member key 任意，完整 `sample_shape()` 含 batch；输出 `photospider.tensor` / `samples`，完整 shape 为普通 axes，丢弃 facets 和 batch 拓扑。单轴 N+1 或双轴各加 1；Whole 非空 demand 请求完整 Data/Validation/Descriptor（role 13），通过授权 windows 读取并发布完整 global coverage；请求只限定 observed dependency roots，消费者可读取已发布 coverage 的任意坐标。Empty 不读 payload。精确 carry 与最终舍入；任一完整输出整数溢出均失败，包括请求投影外坐标。 | [prefix_sum](op_specs/NUM-13A_prefix_sum.md)、[integral_image](op_specs/NUM-13B_integral_image.md)，Proposed；Result Whole 实现和验证入口 `test_numeric_scans_result` / `installed_numeric_scans_result` 见[执行说明](scans-whole.md) |
| NUM-14 matrix_transform | 三个 Result tensor：vectors `[...,Cin]`、matrix `[Cout,Cin]`、bias `[Cout]` → `values` Result | 输入 member key 任意，Float32/64 同 dtype，Cin/Cout∈{2,3,4}。输出 schema 为 `photospider.tensor` / `samples`，shape `[...,Cout]`，普通 axes，无 facets/batch topology。Whole 非空请求三个输入的 Data/Validation/Descriptor（role 13），批量精确计算并最终舍入；Empty 不读 payload。允许奇异矩阵。 | [具体规格](op_specs/NUM-14_matrix_transform.md)，Proposed；Result math 覆盖及当前测试入口见[workflow说明](../../examples/numeric_workflow/README.md) |
| NUM-15 derivative_1d / integrate_1d | Result tensor `[N]` 加一个或两个 `[1]` 控制张量 → `values` Result | 输入 member key 任意；Float32/64 同 dtype。输出为 `photospider.tensor` / `samples`、`[N]` shape，丢弃 facets。Derivative 对非空 Whole demand 请求 samples 与 step；integrate 的 N=1 静态只选择 initial，N>1 请求 samples/step/initial 全部输入。ExactCalculus 按离散公式计算并最终舍入。 | [derivative_1d](op_specs/NUM-15A_derivative_1d.md)、[integrate_1d](op_specs/NUM-15B_integrate_1d.md)，Proposed；Result Whole 实现及验证范围见[执行说明](calculus-whole.md) |

以上是基础语义设计，不声称有单个商业软件与每行完全对应。数据配线、数学和统计通常用于构造上层流程，不应全部暴露为面向绘画使用者的主菜单。

## 后续 NUM 规格的版本规则

本轮已确认 NUM-02 起后续规格默认采用 strict、Apple Silicon CPU accelerated、
x86-64 CPU accelerated 三个独立操作名。序列生成、复制和基本算术的 accelerated
保持与 strict 位一致；涉及近似算法时，在对应规格中另行确认误差要求。
NUM-01～NUM-15 已完成本轮逐项澄清，具体目标契约见各行链接；
这些 Proposed 规格仍表示待维护者接受的契约，不反映 public registry 的实现可用性；运行时状态见[实现进度](implementation.md)。

## 表达式生成器

NUM-01 的逐项澄清与已确认范围见[单算子规格](op_specs/NUM-01_sample_expression.md)。
本轮限定单函数、一维主输出。采样由动态标量 start/end 与静态 count 确定；
count≥2 包含两端，支持递增或递减区间；count=1 只在 start 求值，不读取 end。
输出为通用数值 `values[N]` 和 Float64 `axis=[start,end,step]`，单样本 axis 为
`[start,start,0]`。动态轴使用独立数值输出，消费方显式连接，不能假设当前 typed LUT 接口已支持。

表达式、系数名、count 和输出 dtype 静态；具名系数分别连接动态 Float32/64 `[1]`。
输出默认 Float64，另可选择 Float32；无系数表达式无需占位输入。
values 按请求索引求值，严格检查实际求值的定义域和有限性；axis 独立请求。
语言、坐标舍入、预算、取消、缓存及错误范围以单算子规格为准。

strict 对每步 Float64 运算及数学函数正确舍入；Apple Silicon CPU 与 x86-64 CPU
各有独立 accelerated 名称。加速数学函数每次调用最多 4 ULP，超出已验证能力时
允许数学步骤回退 strict 并报告。数值敏感边界允许两种版本的成功/失败差异。
原始需求中的 “spine” 已澄清为一维 `y=f(x)` 调节曲线，不涉及几何路径。

## 实现和使用面

逐元素族可共享循环、dtype dispatch、边界检查和向量化设施，同时保留各操作独立语义与参数校验。矩阵、scan、reduce 可复用经验证的数值库，但库的默认 NaN、rounding、归一化与线程策略必须适配。要求精确总和的归约和 scan 使用精确状态及最终舍入；固定遍历或固定浮点树本身不能满足该契约。

已运行的可组合工作流包括 `sample_expression → bake_lut1d → apply_lut1d`、显式 broadcast 后的 smoothstep/mix，以及 `lowpass → resample`。公开入口、运行命令和可检查结果见[numeric workflow](../../../examples/numeric_workflow/README.md)。Noise/displacement、auto exposure 和 path width 属于后续组合方向，本轮未验证其完整节点链路。
