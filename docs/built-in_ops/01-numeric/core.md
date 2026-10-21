# 数值与数组基础

2026-09-20：`ops-impl` 的 NUM-01～15 运行时实现已进入当前 public registry，
实现事实按功能簇记录于[实现进度](implementation.md)。

当前 dependency-driven numeric 行为由 Result 需求与 dependency records 表达。算子修改时，
按其数学依赖验证 radius/gather/scatter、scan、reduction，以及需求传播和取消行为；
检查程序现场编写，使用后删除。内核并发与需求测试见[测试与验证](../../development/Testing-and-Validation.md)。
受预算的分页统计是显式注册 factory，不能替代任意 numeric reduction。

已实现的基础子集、精确参数和 Region 见[基础算子实现](../../kernel-architecture/Basic-Operations.md)。规格表中的 Proposed 表示契约状态，不表示当前 runtime 未注册；NUM-01～15 与 CRV-01～11 合计 330 个 primitive keys 已进入当前 public registry，详细实现事实和验证边界见[实现进度](implementation.md)。分类表中的建议参数不覆盖现有接口。

已接受首版见 [ADR 0020](../../adr/0020-composable-operation-foundations.md)：四 dtype cast/range 分离，Float32/64 同 shape 基础算术和显式 clamp，全数组 Float64 mean/variance，有界单通道 expression（静态 start/step/count、动态 Float64 coefficients）及 linear 1D LUT。未带后缀的 `numeric.add`、`subtract`、`multiply`、`divide`、`clamp`、`abs`、`minimum` 与 `maximum` 使用固定 CPU task。每个输入恰含一个 tensor，按完整 `sample_shape()`（含 batch）匹配 Float32/64；输出 `value` 为 `photospider.tensor` v1/member `samples`。每个选中输出 tile 声明与其相交的输入 tile，直接写入独立的预分配输出；空选择不生成 task。Clamp 参数的存在性和类型在 preparation 前检查，有限性及边界顺序是 unchecked 数值前提。运行时采用 unchecked 有限域契约，由调用方保证数值前提。精确行为和绑定限制见[数值算子架构说明](../../kernel-architecture/Numeric-Operations.md)。`numeric.mean`、`numeric.variance` 和 `numeric.ordered_scan` 使用固定的完整输入 CPU task，分别保持 Float64 row-major mean、两遍 population variance，以及从正零开始的 inclusive scan 顺序。输出预分配；空选择不生成 task。可选 Int64 `block_size` 范围为 1..65536，默认 64，只控制取消查询间隔。数值前提遵循 unchecked 有限域契约，参数与执行细节见数值算子架构页。

当前已实现的数值算子与公开运行示例见[数值算子实现](../../kernel-architecture/zh/Numeric-Operations.zh.md)和[实现进度](implementation.md)。Expression/LUT 的实现与公开 workflow 见[实现文档](../../kernel-architecture/zh/Expression-and-LUT-Operations.zh.md)；下表 Proposed 只表示规格接受状态。

状态 Proposed。本篇为 D1 数学核心，受[公共契约](../00-foundation/contracts.md)和已注册 Result 数据模型约束。建议 CPU Float32/Float64 参考实现；整数支持逐项定义，不能默认为所有张量运算有 Metal 后端。符号 E/W/S 和默认约定见[公共契约](../00-foundation/contracts.md)。

未带后缀的 `numeric.sample_expression` 和 `lut.apply_1d` 使用固定 CPU task。前者按所选输出 tile 读取完整小型系数表，使用 4096 字节 scratch；后者读取所选 query 区域及作为数据相关地址上界的完整 table，scratch 为零。数值定义域由调用方保证，静态 schema、参数类型与语法仍检查；LUT 地址数值域由调用方保证，保留计算所需的有界 cell correction 和精度判定。见[表达式与 LUT 契约](../../kernel-architecture/Expression-and-LUT-Operations.md)。

## 基础目录

各独立规格显式继承 [NUM 共享契约](op_specs/NUM_common_contract.md)，统一记录
规格状态、注册与平台、参数格式、错误范围、资源和验收要求；具体算子的例外优先。

| ID / 提议操作 | 输入 → 输出 | 参数、语义与算法 | 依赖 / 验收 |
| --- | --- | --- | --- |
| NUM-01 expression generator family | Float32/64 `[1]` start/end/具名系数 → `values` 与 `axis` | values 为普通 `[N]`、指定 Float32/64，axis 为原子 Float64 `[3]`。每所选 values tile 一个固定 task，按全局 index 计算；axis 仅读有效端点。N=1 排除 end payload，所有 schema 仍校验。输入／中间结果有限性、函数域和坐标精度由调用方保证。Strict 按 AST 每步 RN64，accelerated 对最终误差验证并严格重算不确定样本。各 task 使用预分配 scratch，选中输出共享 call 成败。 | [规格](op_specs/NUM-01_sample_expression.md)、[固定执行](expression-execution.md)，Proposed |
| NUM-02 sequence generators | 单 tensor Result 标量输入 → `values` 与原子 `axis` Results | 六个固定 CPU key，count 1..1048576。每选中 values tile 一个任务，axis 一个完整三元组任务；精确全局索引算术和直接最终舍入。Count=1 只读 start，第二 schema 仍校验；其他非空任务读两标量。浮点有限性及所选输出可表示性为 unchecked 前提。两输出独立选择但共享 call 成败；Empty 无任务。 | [linspace](op_specs/NUM-02A_linspace.md)、[arange](op_specs/NUM-02B_arange.md)、[执行](sequences-execution.md)，Proposed |
| NUM-03 constant / broadcast families | scalar/array + shape → `values` Result | 六个固定 CPU profile key，支持七种数值 dtype。每个选中输出 tile 独立 dense 分配；constant 每 task 读一次标量，broadcast 按显式 injective axis_map 声明映射源 tile。移除 layout/ArrayLayout；Empty 无 task，运行无 payload scratch，保留全部原始位。 | [constant](op_specs/NUM-03A_constant.md)、[broadcast](op_specs/NUM-03B_broadcast.md)、[执行](arrays-execution.md)，Proposed |
| NUM-04 unary families | array（rational 入口为 numerator/denominator）→同形 | 66 个 profile 定义使用固定 tile task。abs/neg/sqrt/exp/ln/sin/cos/tan/sinpi/cospi/tanpi/sinc/sincpi 及四个精确 p/q·π rational 对应版本、floor/ceil/round/sign/reciprocal 独立命名；pow 归 NUM-05；允许 NaN/Inf 的 IEEE 风格行为，特殊值与精度逐项定义 | [共享契约](op_specs/NUM-04_unary_contract.md)，各函数分文件细化；Proposed |
| NUM-05 binary families | 两个单 tensor Result 输入 → `values` Result | 27 个固定 CPU task profile keys：add/subtract/multiply/divide/minimum/maximum/pow/atan2/atan2pi，各有 Strict/Apple/x86 命名 profile。每个输入 Result 含一个 slot 0 tensor member，完整 sample shape 与 dtype 必须匹配；输出保留完整 sample shape 和 dtype，按普通轴发布并丢弃 batch 拓扑。Broadcast/cast 显式处理。 | [共享契约](op_specs/NUM-05_binary_contract.md)，各函数独立规格；Proposed；当前 Result 实现与验证边界见[实现页](point-math-execution.md) |
| NUM-06 clamp / remap_range | Clamp 三个、remap_range 五个 `Result` tensor 输入 → `values` Result | 各输入 member 的完整 `sample_shape()`（含 batch 前缀）和 dtype 必须相同；输出使用完整 shape、丢弃 batch 拓扑。Clamp 支持 UInt8/Int64/Float32/Float64，按比较选值并保留区间内位模式；remap_range 支持 Float32/64，以精确 rational 整式为数学定义，strict 精确舍入、accelerated 遵守 FP32 ULP contract，并允许外推。六个 profile 定义使用固定 CPU tile task，声明各输入的相交 tile，输出和 scratch 在发射前分配。有效 bounds 为 unchecked 调用者前提；Empty 不发任务。 | [NUM-06A clamp](op_specs/NUM-06A_clamp.md)、[NUM-06B remap_range](op_specs/NUM-06B_remap_range.md)、[执行说明](range-execution.md)，Proposed |
| NUM-07 comparisons / select | 单 tensor Result inputs → `photospider.tensor` / `samples` Result | 24 个 profile 定义使用固定 CPU tile task。完整 sample shape 含 batch，各输入可用不同网格；六个比较接受 UInt8/Int64/Float32/Float64，输出 UInt8；`is_close` 接受 Float32/64，容差类型在 prepare 前校验，有限非负数值域为 unchecked 前提，使用精确有理阈值。`select` 使用 UInt8 条件和同 dtype 双分支，0/1 条件为调用者前提，按位复制选中值。每个请求 tile 声明相交输入，Empty 不发任务。 | [比较契约](op_specs/NUM-07_comparison_contract.md)、[is_close](op_specs/NUM-07G_is_close.md)、[select](op_specs/NUM-07H_select.md)、[执行说明](comparison-execution.md)，Proposed |
| NUM-08 mix / smoothstep | 三个动态同 shape/dtype 浮点 Result tensor → 一个 Result tensor | 每个输入含一个 member，key 任意；Float32/64，完整 sample shape 含 batch。每个请求输出 tile 生成一个固定 CPU task，按各输入网格声明相交 tile，写入独立的预分配输出。Mix 的有限 factor `[0,1]` 和 smoothstep 的有限递增 edges 为调用者前提；端点位复制、NaN/Inf 和精确舍入按成员规格执行。Empty 不生成任务。 | [NUM-08A mix](op_specs/NUM-08A_mix.md)、[NUM-08B smoothstep](op_specs/NUM-08B_smoothstep.md)，Proposed；[执行说明](interpolation-execution.md) |
| NUM-09 reshape / transpose / slice | 单 tensor Result → `values` Result；slice 另有动态 Int64 starts/steps | 固定 CPU gather task，每个选中输出 tile 独立 dense 分配，移除 layout 策略。Reshape 按全局 row-major ordinal 映射，transpose 按静态 permutation 映射；slice 声明所有合法控制可能访问的源 tile，完整端点合法性为 unchecked 前提，运行时直接计算所选地址。全 singleton 排除 steps payload；Empty 无任务。保留全部原始位，支持负/零 stride 和输入 batch/atomic 拓扑。 | [reshape](op_specs/NUM-09A_reshape.md)、[transpose](op_specs/NUM-09B_transpose.md)、[slice](op_specs/NUM-09C_slice.md)、[执行](layouts-execution.md)，Proposed |
| NUM-10 concatenate / gather / scatter | Result tensors → `values` Result | 固定 CPU task，输出 tile 独立 dense 分配。Concatenate 按所选输出与各输入 slab 的交集声明 reads，移除 layout 参数。Gather/scatter 共享一次 indices table 构造与私有 Int64[M,2] table；scatter 稳定排序保持原 position 顺序。每个输出 tile 消费 table 及对应 base/source/updates 区域，直接信任 normalized position 和源关联，不检查边界。只计算选中输出，整数 sum 可表示为 unchecked 前提；Empty 无 task。 | [concatenate](op_specs/NUM-10A_concatenate.md)、[gather](op_specs/NUM-10B_gather.md)、[scatter](op_specs/NUM-10_scatter_contract.md)、[执行](indexing-execution.md)，Proposed |
| NUM-11 reduction family | 单 tensor Result → `values` Result | 每个 input Result 恰有一个 tensor member，key 任意；完整 sample shape 含 batch 前缀，rank 1..8、元素数≤2^40。输出保留 rank、reduced axes 置 1，使用 `photospider.tensor` / `samples`，丢弃 batch 拓扑。21 个 profile key 使用固定 CPU tile task；六个数值归约声明所选输出组对应的完整归约轴来源 tile，并使用预分配精确算术 scratch。Count 仅依赖静态 schema/axes，输出独立 dense Int64 tile，无 payload 读取；Empty 不发任务。Sum/min/max/mean/variance/std 的 NaN 顺序、精确和、ddof 与最终舍入见共享契约。 | [共享及独立规格](op_specs/NUM-11_reduction_contract.md)、[执行说明](reductions-execution.md)，Proposed |
| NUM-12 sort / quantile | `Result` tensor → sort 的 `values` / `indices`；source 与 `q` Results → quantile `values` | source 为 UInt8/Int64/Float32/64，完整 sample rank 1..8、元素数≤2^40，静态 `axis`。输出使用普通 axes。Sort 将非 axis ROI 相同的所选 tiles 合为一个固定 task，每条完整轴线排序一次，values 保留位模式，indices 为原轴坐标，分别分配所选 tiles。Quantile 输出保留 rank 且 axis 长度 1，q 为 Float32/64 `[1]`，精确 rank 与插值后最终 RN；活动 q 的有限 `[0,1]` 域由调用方保证。轴长 1 时仅检查 q schema，省去 payload 和 producer。scratch 为每 invocation 的 `16*L` 与固定状态，Empty 无任务。 | [NUM-12A](op_specs/NUM-12A_sort.md)、[NUM-12B](op_specs/NUM-12B_quantile.md)、[固定执行](ordering-execution.md)，Proposed |
| NUM-13 prefix_sum / integral_image | 单 tensor Result → `values` Result | 输入 member key 任意，完整 `sample_shape()` 含 batch；输出 `photospider.tensor` / `samples`，全部为普通 axes。单轴 N+1 或双轴各加 1。按非扫描区域分组为固定 CPU task，读取正边界所需前缀矩形相交的输入 tile，写入独立 dense 输出 tile。仅零边界的组无 payload 依赖和 scratch，Empty 无 task。精确 carry 与最终舍入；整数可表示是全部 task 写入的 unchecked 前提。`select_outputs` 保留完整 task；重新规划可缩小前缀。 | [prefix_sum](op_specs/NUM-13A_prefix_sum.md)、[integral_image](op_specs/NUM-13B_integral_image.md)、[执行](scans-execution.md)，Proposed |
| NUM-14 matrix_transform | 三个 Result tensor：vectors `[...,Cin]`、matrix `[Cout,Cin]`、bias `[Cout]` → `values` Result | 输入 member key 任意，Float32/64 同 dtype，Cin/Cout∈{2,3,4}。输出 schema 为 `photospider.tensor` / `samples`，shape `[...,Cout]`，普通 axes，无 batch topology。每个请求输出 tile 生成固定 CPU task：读取对应 vector prefix 的全部 Cin、所选 matrix 行及 bias 通道，预分配输出和 scratch。批量精确点积后最终舍入，Empty 不发任务。允许奇异矩阵。 | [具体规格](op_specs/NUM-14_matrix_transform.md)，Proposed |
| NUM-15 derivative_1d / integrate_1d | Result tensor `[N]` 加一个或两个 `[1]` 控制张量 → `values` Result | Float32/64 同 dtype，完整 shape `[N]`，输出为普通 axes。Derivative 每所选 tile 声明两点 stencil 的精确来源 tile 并集和 step。Integrate 将正索引输出 tiles 合为一个精确前缀 task，只读到最大选中索引；独占 `[0,1)` 的 tile 单独只读 initial、零 scratch，samples／step 仅 schema。活动 step 的有限非零域由调用方保证。Empty 无任务。 | [NUM-15A](op_specs/NUM-15A_derivative_1d.md)、[NUM-15B](op_specs/NUM-15B_integrate_1d.md)、[固定执行](calculus-execution.md)，Proposed |

以上是基础语义设计，不声称有单个商业软件与每行完全对应。数据配线、数学和统计通常用于构造上层流程，不应全部暴露为面向绘画使用者的主菜单。

## 后续 NUM 规格的版本规则

序列生成、复制和基本算术的 accelerated
保持与 strict 位一致；涉及近似算法时，在对应规格中另行确认误差要求。
NUM-01～NUM-15 的目标契约见各行链接；
这些 Proposed 规格仍表示待维护者接受的契约，不反映 public registry 的实现可用性；运行时状态见[实现进度](implementation.md)。

## 表达式生成器

NUM-01 的逐项澄清与已确认范围见[单算子规格](op_specs/NUM-01_sample_expression.md)。采样由动态标量 start/end 与静态 count 确定；
count≥2 包含两端，支持递增或递减区间；count=1 只在 start 求值，不读取 end。
输出为通用数值 `values[N]` 和 Float64 `axis=[start,end,step]`，单样本 axis 为
`[start,start,0]`。动态轴使用独立数值输出，消费方显式连接，不能假设当前 typed LUT 接口已支持。

表达式、系数名、count 和输出 dtype 静态；具名系数分别连接动态 Float32/64 `[1]`。
输出默认 Float64，另可选择 Float32；无系数表达式无需占位输入。
values 按所选 tile 的全局索引求值，实际求值的定义域和有限性由调用方保证；axis 独立选择。
语言、坐标舍入、预分配资源、取消及错误范围以单算子规格为准。

strict 对每步 Float64 运算及数学函数正确舍入；Apple Silicon CPU 与 x86-64 CPU
各有独立 accelerated 名称。加速结果相对于完整 strict AST 参考值满足最终 FP32-scaled 四 ULP 界；无法证明最终误差时严格重算该样本。数值域由调用方保证，固定调用不提供逐值 fallback 计数。
原始需求中的 “spine” 已澄清为一维 `y=f(x)` 调节曲线，不涉及几何路径。

## 实现和使用面

逐元素族可共享循环、dtype dispatch、静态结构检查和向量化设施，同时保留各操作独立语义与参数类型、语法校验。矩阵、scan、reduce 可复用经验证的数值库，但库的默认 NaN、rounding、归一化与线程策略必须适配。要求精确总和的归约和 scan 使用精确状态及最终舍入；固定遍历或固定浮点树本身不能满足该契约。

已运行的可组合工作流包括 `sample_expression → bake_lut1d → apply_lut1d`、显式 broadcast 后的 smoothstep/mix，以及 `lowpass → resample`。
