# 数值与数组基础

2026-09-14：`ops-impl` 正在实现本目录完整契约，按功能簇记录于
[实现进度](implementation.md)。NUM-02 三种 CPU profile 与手动公开 workflow
已运行，本簇独立审核及必要修复已完成；下述 Proposed 规格状态保持独立。

2026-09-13：G4 已有 `numeric.radius_gather/radius_scatter` 与 ordered scan；Phase A 提供 `statistics.histogram/parameters/grade` 的受预算分页全局流程。见[采样](../../kernel-architecture/Dependency-Sampling.md)、[Region](../../kernel-architecture/Region-Semantics.md)、[整数统计](../../kernel-architecture/Integer-Statistics.md)。分页统计为显式注册 factory，不能替代任意 numeric reduction。

已实现的基础子集、精确参数和 Region 见[基础算子实现](../../kernel-architecture/Basic-Operations.md)；未标注实现的扩展条目保持 Proposed。分类表中的建议参数不覆盖现有接口。

已接受首版见 [ADR 0020](../../adr/0020-composable-operation-foundations.md)：四 dtype cast/range 分离，Float32/64 同 shape 基础算术和显式 clamp，全数组 Float64 mean/variance，有界单通道 expression（静态 start/step/count、动态 Float64 coefficients）及 linear 1D LUT。必填参数由 workflow 显式提交；首版 Whole 是历史范围，当前 Region 以各实现文档为准。min/max/abs、smoothstep 和 `numeric.ordered_scan` 已交付，其他 unary/broadcast/通用 scan 与数组扩展继续 Proposed。

当前已实现的 cast/encode_range、四种二元算术、clamp、mean/variance 及公开运行示例见[数值算子实现](../../kernel-architecture/zh/Numeric-Operations.zh.md)。Expression/LUT 的首版实现与公开 workflow 见[实现文档](../../kernel-architecture/zh/Expression-and-LUT-Operations.zh.md)；下表扩展目录仍为 Proposed。

状态 Proposed。本篇为 D1 数学核心，受 G1/G3/G5 数据与组合前置条件约束。建议 CPU Float32/Float64 参考实现；整数支持逐项定义，不能默认为所有张量运算有 Metal 后端。符号 E/W/S 和默认约定见[公共契约](../00-foundation/contracts.md)。

## 基础目录

各独立规格显式继承 [NUM 共享契约](op_specs/NUM_common_contract.md)，统一记录
规格状态、注册与平台、参数格式、错误范围、资源和验收要求；具体算子的例外优先。

| ID / 提议操作 | 输入 → 输出 | 参数、语义与算法 | 依赖 / 验收 |
| --- | --- | --- | --- |
| NUM-01 expression generator family | 单函数、动态 start/end/具名系数 → `values[N]`、`axis[3]` | `start,end,count` 含端点，支持反向；N=1 忽略 end；Float32/64 输出默认 Float64；strict 与两个 CPU 平台 accelerated 独立命名 | [具体规格](op_specs/NUM-01_sample_expression.md)，Proposed；values 按索引请求，axis 独立；新接口尚未实现 |
| NUM-02 sequence generators | linspace: start/end/count；arange: start/step/count | 已选择两个独立算子，分别细化；显式数量决定有限序列长度 | [NUM-02A linspace](op_specs/NUM-02A_linspace.md)、[NUM-02B arange](op_specs/NUM-02B_arange.md)，Proposed |
| NUM-03 constant / broadcast families | scalar/array + shape → array | 已选定两个独立算子；constant 填充标量，broadcast 显式映射输入轴 | [NUM-03A constant](op_specs/NUM-03A_constant.md)、[NUM-03B broadcast](op_specs/NUM-03B_broadcast.md)，Proposed |
| NUM-04 unary families | array（rational 入口为 numerator/denominator）→同形 | abs/neg/sqrt/exp/ln/sin/cos/tan/sinpi/cospi/tanpi/sinc/sincpi 及四个精确 p/q·π rational 对应版本、floor/ceil/round/sign/reciprocal 独立命名；pow 归 NUM-05；允许 NaN/Inf 的 IEEE 风格行为，特殊值与精度逐项定义 | [共享契约](op_specs/NUM-04_unary_contract.md)，各函数分文件细化；Proposed |
| NUM-05 binary families | A,B → 同 shape | add/subtract/multiply/divide/minimum/maximum/pow/atan2/atan2pi 独立命名；输入 shape/dtype 严格相同，使用显式 broadcast/cast | [共享契约](op_specs/NUM-05_binary_contract.md)，各函数独立规格；Proposed |
| NUM-06 clamp / remap_range | 动态输入与边界 → 同形 | clamp 三个同 shape/dtype 输入，四种 dtype；按比较选择并保留区间内输入位模式；remap_range 五个浮点输入，整式正确舍入、默认外推 | [NUM-06A clamp](op_specs/NUM-06A_clamp.md)、[NUM-06B remap_range](op_specs/NUM-06B_remap_range.md)，Proposed |
| NUM-07 comparisons / select | A,B 或 condition,A,B → UInt8 0/1 或 array | 六种精确比较独立命名；is_close 精确对称容差；select 先读条件，再读取选中分支位置 | [精确比较](op_specs/NUM-07_comparison_contract.md)、[is_close](op_specs/NUM-07G_is_close.md)、[select](op_specs/NUM-07H_select.md)，Proposed |
| NUM-08 mix / smoothstep | 三个动态同 shape/dtype 浮点输入 → 同形 | mix 的 t∈[0,1]，端点只读对应分支；smoothstep 有限递增边界、固定三次；两者整式正确舍入 | [NUM-08A mix](op_specs/NUM-08A_mix.md)、[NUM-08B smoothstep](op_specs/NUM-08B_smoothstep.md)，Proposed |
| NUM-09 reshape / transpose / slice | array → 通用数组 | 四种 dtype 按位保留、元素数≤2^40；reshape 按逻辑行主序；transpose 静态轴排列；slice 动态起点/步长、静态数量；auto/view/dense | [reshape](op_specs/NUM-09A_reshape.md)、[transpose](op_specs/NUM-09B_transpose.md)、[slice](op_specs/NUM-09C_slice.md)，Proposed |
| NUM-10 concatenate / gather / scatter_reduce | arrays/indices → array | concatenate 默认多片段 view；gather 单轴按需索引；四种 scatter 全局索引校验、按需贡献者 | [concatenate](op_specs/NUM-10A_concatenate.md)、[gather](op_specs/NUM-10B_gather.md)、[scatter](op_specs/NUM-10_scatter_contract.md)，Proposed |
| NUM-11 reduction family | input + 静态 axes → 保留维度的归约数组 | 七个独立算子；count 只读元数据；sum/mean/variance/std 整式正确舍入；ddof 静态默认 0 | [共享及独立规格](op_specs/NUM-11_reduction_contract.md)，Proposed |
| NUM-12 sort / quantile | array → values/indices 或单分位数 | sort 稳定升序、NaN 末尾；quantile 动态 q、精确线性插值、传播 NaN；按命中切片读取 | [sort](op_specs/NUM-12A_sort.md)、[quantile](op_specs/NUM-12B_quantile.md)，Proposed |
| NUM-13 prefix_sum / integral_image | array → 零边界前缀和 | 单轴 N+1 或双轴各加 1；精确源和、最终舍入；仅按请求前缀检查溢出 | [prefix_sum](op_specs/NUM-13A_prefix_sum.md)、[integral_image](op_specs/NUM-13B_integral_image.md)，Proposed |
| NUM-14 matrix_transform | vectors + 共享 matrix/bias → vectors | 2/3/4 维 Mx+b，完整点积最终舍入；允许奇异矩阵；按输出分量读取对应行 | [具体规格](op_specs/NUM-14_matrix_transform.md)，Proposed |
| NUM-15 derivative_1d / integrate_1d | samples + 动态 step / initial → values | 中心差分＋一阶端点；累积梯形积分；整式正确舍入与精确依赖 | [derivative_1d](op_specs/NUM-15A_derivative_1d.md)、[integrate_1d](op_specs/NUM-15B_integrate_1d.md)，Proposed |

以上是基础语义设计，不声称有单个商业软件与每行完全对应。数据配线、数学和统计通常用于构造上层流程，不应全部暴露为面向绘画使用者的主菜单。

## 后续 NUM 规格的版本规则

本轮已确认 NUM-02 起后续规格默认采用 strict、Apple Silicon CPU accelerated、
x86-64 CPU accelerated 三个独立操作名。序列生成、复制和基本算术的 accelerated
保持与 strict 位一致；涉及近似算法时，在对应规格中另行确认误差要求。
NUM-01～NUM-15 已完成本轮逐项澄清，具体目标契约见各行链接；
这些 Proposed 规格与当前运行时实现状态分别记录，不因澄清完成而视为已实现。

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

概念工作流：`sample_expression → LUT apply`、`linspace → periodic noise → displacement field`、`channel extract → mean/variance → auto exposure`、`path arc length → width curve`。现有公开入口样例使用 legacy expression/LUT 接口；本轮新增的 versioned sample_expression 与动态 axis 配线仍须实现和运行验收。其余概念流程的完整节点也有待实现。
