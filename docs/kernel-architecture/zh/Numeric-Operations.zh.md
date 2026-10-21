# 数值算子

所有已注册的 Core 标量／COPY 算子、未带后缀的算术／归约／scan／expression／LUT key，以及下文描述的数值 profile 族均使用物理 task。参见[执行契约](Compiler-and-Execution.zh.md)。

Bundled 源码定义了未带后缀的有限域算术与 clamp、固定标量归约、有序扫描，以及带 profile 的数值算子族。各 key 的参数、执行和输出契约不同，调用方应按完整 key 选择。这些是 `plugins/ops/01-numeric/` 中的 bundled operations，不是内核代码；内核只提供它们所用的通用 Result 执行、资源、取消和失败契约。这些算子使用 module ABI 8 和 fixed invocation ABI 8。数值工作报告、LUT bake 报告和质量报告是算子 SDK 类型（`photospider/ops/numeric/`）；见 [Atom 错误与质量](Atom-Errors-and-Quality.zh.md)。

## 未带后缀的 Result 数值算子

| Key | 输入与输出 | 参数和区域 |
| --- | --- | --- |
| `math.add` | 两个 Float64 tensor Results，各含一个 sample（`sample_shape() == [1]`）；输出 port `value` 为 `photospider.tensor` v1 Result/member `samples`，shape `[1]` | 无参数；固定标量 task；IEEE addition |
| `numeric.add`、`numeric.subtract`、`numeric.multiply`、`numeric.divide` | 两个 Float32/Float64 Result 输入，完整 `sample_shape()` 和 dtype 相同；输出 port `value` 为 `photospider.tensor` v1 Result，tensor member `samples` 保留 shape 和 dtype | 无参数；固定输出 tile task |
| `numeric.clamp` | 一个 Float32/Float64 Result 输入；输出 port `value` 使用相同 Result schema 并保留 shape 和 dtype | 必填有限 Float64 `min`、`max`，满足 `min <= max`；固定输出 tile task |
| `numeric.abs` | 一个 Float32/Float64 tensor Result；输出 port `value` 为 `photospider.tensor` v1/member `samples`，sample shape 与 dtype 相同 | 无参数；固定输出 tile task |
| `numeric.minimum`、`numeric.maximum` | 两个完整 sample shape、dtype 相同的 Float32/Float64 tensor Results；输出 port `value` 为 `photospider.tensor` v1/member `samples` | 无参数；固定输出 tile task |

每个逐元素输入恰含一个 tensor，schema 和 member key 必须结构有效。各输入的 Float32/Float64 dtype 和完整 `sample_shape()` 相同，rank 为 1..8，包含 batch axes。输出是 `photospider.tensor` v1 Result/member `samples`，保留 dtype，并将完整 shape 作为普通 axes。

八个未带后缀的 numeric key 使用 unchecked 有限域语义。调用方保证操作数有限、结果可表示；除法要求除数非零。Clamp 的静态 Float64 `min`、`max` 必须有限且满足 `min <= max`，这些数值条件均为 unchecked 前提，metadata specialization 只检查参数存在性和 Float64 类型。所选结果必须能用目标 dtype 表示；未选端点可以超出 Float32 范围。运行时 kernel 不扫描样本，也不承诺对违反数值前提的输入返回错误。规划阶段检查每个所选 tile 的字节容量。大逻辑域可进行稀疏选择，无需物化或计数完整 payload。

`math.add` 要求两个完整 `sample_shape()` 为 `[1]` 的 Float64 tensor 输入，产生 `photospider.tensor` v1 Result，其中 `samples` 成员的 shape 为 `[1]`。非空选择降低为一个 ABI 8 CPU task，具有两个固定 read 和一个预分配 write。Kernel 执行 IEEE Float64 addition，包括 NaN 和 infinity 传播。Preparation 检查完整的标量 invocation 签名。空选择不生成 task。

`core.constant` 没有输入，要求必填 Float64 参数 `value`，无默认值或有限域限制。它通过一个固定 CPU task 产生相同的标量 schema。Preparation 持有不可变参数值，直至所有已接受调用结束。

`core.identity` 接受一个恰含一个 tensor 的 Result，保持 schema、dtype、shape 和所选 coverage。每个请求 tile 降低为显式 Host COPY，输出分别分配。复制保持 sample 字节，包括 signed zero 和 NaN payload。输入遵循 Plan 的固定 affine 绑定契约。零 tile 选择不生成 COPY task。这三个算子均无需运行时 scratch；输出存储和 metadata 纳入 block admission。

每个选中的输出 tile 生成一个 CPU task。Lowering 分别按各输入网格声明与输出区域相交的全部输入 tile。因此输入的 batch、spatial 和 atomic axes 可以与输出网格不同，逻辑坐标保持一致。Preparation 检查完整读取覆盖并绑定固定 invocation slot。执行直接向独立分配的输出 tile 写入计算值，无 scratch payload。空选择不生成 task。

`abs(-0)` 产生 `+0`；minimum 对任意一对零操作数返回 `-0`，maximum 返回 `+0`。算术使用 nearest rounding 与 gradual underflow，并恢复 worker 的浮点环境。每至多 1024 个输出 sample 检查取消。未带后缀的有限域 key 使用上述独立语义；带后缀 profile 保持各自规定的 dtype 和 IEEE 规则。

## 未带后缀的表达式与 LUT

`numeric.sample_expression` 接受无 batch 的 Float64 系数向量，以及静态 start／正 step／count／expression。每个选中 Float32 输出 tile 读取完整系数向量，使用 4096 字节固定 scratch，并按全局 index 计算 FMA 坐标。有界 AST 在 preparation 解析一次；数值域及中间量／结果有限性由调用方保证。

`lut.apply_1d` 按静态 origin／正 step 与 reject／clip 策略将 Float32 query 映射到无 batch 的 Float32 table。每个 task 读取所选 query 区域，并将完整 table 声明为数据相关地址的保守上界。输出保留 query cell shape 和 batch axes，scratch 为零。Index 转换的数值域为 unchecked 前提；保留有界 cell correction 和补偿插值所需的精度判定。参见[表达式与 LUT 算子](Expression-and-LUT-Operations.zh.md)。

## 固定有序归约与 scan

| Key | 输入与输出 | 参数与执行 |
| --- | --- | --- |
| `numeric.mean`、`numeric.variance` | 一个 Float32/Float64 tensor Result，完整 sample rank 为 1..8；输出 `value` 为 Float64 `photospider.tensor` v1 Result/member `samples`，shape `[1]` | 可选 Int64 `block_size`，范围 `[1,65536]`，默认 64；一个完整输入 CPU task |
| `numeric.ordered_scan` | 一个完整 sample rank 为 1 的 Float64 tensor Result；输出 `value` 使用上述 tensor schema，shape 与输入相同 | 相同的 `block_size`；一个完整输入 CPU task，写入所选输出 tile |

Mean 将 Float32 sample 精确转换为 Float64，从正零开始按逻辑 row-major 顺序累加，再除以转换为 Float64 的 sample 数量。Variance 在第一遍计算该均值，第二遍按相同输入顺序用 Float64 累加 `(sample - mean) * (sample - mean)`，再除以数量（`ddof=0`）。Scan 从正零开始执行严格从左到右的 Float64 inclusive addition。输入 tile 边界与 `block_size` 均不改变算术分组。

非空请求降低为一个读取全部输入 tile 的固定 task。Mean 和 variance 写入一个预分配标量；scan 遍历完整输入，只写入所选的预分配输出 tile。Preparation 检查完整且有序的读取集合及每个输出 view。Kernel 将坐标、sum 和 mean 放在栈上，不分配 payload 或中间 Result。空选择不生成 task，返回带空 coverage 的声明 schema。

这些 kernel 使用 unchecked 有限域契约：sample 和所有中间算术必须有限且可表示。静态校验检查 dtype、shape、可表示的 sample count 和可选 block size。`block_size` 指定取消查询间隔，最多经过该数量的 sample；variance 第二遍重新开始计数。CPU invocation 使用 nearest rounding 与 gradual underflow，并恢复 worker 的浮点环境。取消会丢弃未发布输出，已接受的输入和输出 owner 保留到 kernel 返回。

## 固定比较与选择 profile

六个比较谓词、`is_close` 和 `select` 各有 strict、Apple Silicon 与 x86-64 profile key。每个请求输出 tile 生成一个 CPU task。Prepare 根据各端口自己的 dtype 和网格校验固定 reads。比较输出 UInt8；select 保留分支 dtype 和原始位模式。Empty 不发任务。

普通比较支持 UInt8、Int64、Float32 和 Float64，不将整数转为浮点。IEEE 位分类直接处理 signed zero、Inf 和 NaN。`is_close` 接受 Float32/Float64，检查必需静态参数 `atol`/`rtol` 的 Float64 类型，其有限非负数值域为 unchecked 前提，在预分配 scratch 内进行精确整数阈值比较。Select 使用 UInt8 条件，0/1 为 unchecked 调用者前提；两个分支均为声明依赖。其循环使用有界栈状态，task scratch 为零。参见 [NUM-07 固定执行](../../built-in_ops/01-numeric/comparison-execution.md)。

## 固定插值 profile

`numeric.mix_<profile>` 和 `numeric.smoothstep_<profile>` 接受三个同 shape 的 Float32/Float64 tensor，输出 `values`，schema 为 `photospider.tensor`，member 为 `samples`。每个选中 tile 生成一个 CPU invocation，声明相交输入 tile 和预分配的精确算术 scratch。各输入的 batch 与 atomic axes 可以不同。Prepare 校验固定 slot coverage；独立 Run 共享不可变准备状态，使用各自的 invocation scratch。

Mix factor 必须有限且位于 `[0,1]`；smoothstep edges 必须有限且严格递增。这些是 unchecked 调用者前提。端点位模式、NaN/Inf 处理和 strict 精确有理数舍入遵循[成员规格](../../built-in_ops/01-numeric/interpolation-execution.md)。Empty 不发 task。CPU profile 可用性在执行前检查。

## 带 profile 后缀的逐元素算子

数值 math 算子族使用三个显式后缀：

| Profile 后缀 | 运行准入 |
| --- | --- |
| `_strict` | 可移植 strict 实现 |
| `_accelerated_apple_silicon` | Apple Silicon 实现，仅在运行 profile 可用时准入 |
| `_accelerated_x86_64` | x86-64 加速实现，仅在运行 profile 可用时准入 |

`numeric_unary.cpp` 和 `numeric_binary.cpp` 共定义 93 个固定 CPU profile key。每个请求输出 tile 生成一个 ABI 8 invocation，prepare 校验各端口 dtype、输入 views、输出和精确 scratch 容量。输入可使用不同 batch/atomic 网格。Rational-pi 保留两个 Int64 输入的完整位宽，再按显式 dtype 输出 Float32/64。固定 scratch 聚合包含算术状态、64-lane 批量缓冲区与 Float32 SIMD 缓冲区；独立 Run 使用各自的可变状态。Empty 不发任务。

整数结果须可表示，rational denominator 须为正，均为 unchecked 调用者前提。浮点 NaN/Inf、signed zero、精确舍入及 accelerated 误差界按成员规格执行。算术 refinement 不改变 Plan placement；取消在每批及精确算法检查点查询。详见[固定数学执行](../../built-in_ops/01-numeric/point-math-execution.md)。

## 固定动态范围 profile

`numeric.clamp_<profile>` 按 value/lower/upper 顺序接受三个同 shape/dtype 输入；`numeric.remap_range_<profile>` 按 value/source-lower/source-upper/target-lower/target-upper 顺序接受五个输入。每个请求输出 tile 生成一个固定 CPU task，声明所有相交输入 tile，并预分配 `RangeMath` scratch。Kernel 写入独立的 dense 输出分配。Empty 不发任务。

Clamp 支持 UInt8、Int64 和 Float32/64，保留选中值位模式并 quiet 输入 NaN；调用者提供非 NaN 且有序的 bounds。Remap 支持 Float32/64，保留端点位模式，计算完整有理公式后最终舍入；调用者提供有限 bounds 和递增源区间。这些 bounds 规则均为 unchecked 前提。参见[固定范围执行](../../built-in_ops/01-numeric/range-execution.md)。未带后缀的 `numeric.clamp` 有独立的静态参数和有限域语义。

## 固定矩阵变换 profile

三个 `numeric.matrix_transform_<profile>` key 接收同 dtype 的 Float32/64 vectors `[...,Cin]`、matrix `[Cout,Cin]` 和 bias `[Cout]`，Cin/Cout 为 2..4。每个选中输出 tile 声明对应 vector prefix 的全部 Cin、所选 matrix 行与 bias 通道。Prepare 校验完整签名，任务使用预分配的输出和 `MatrixWorkspace` scratch；局部 Cout 可以为 1..4，支持部分通道和非零原点。Empty 不发任务。

有限值计算完整点积加 bias 后最终舍入；NaN 优先级、Inf 和零符号遵循 [NUM-14](../../built-in_ops/01-numeric/op_specs/NUM-14_matrix_transform.md)。Apple Float32 候选结果通过独立 enclosure 检查，其他情况使用原始位 ExactDot。固定 kernel 按最多 64 个向量批量计算；prepared state 不可变，每次 invocation 有独立算术状态。

## 固定归约 profile

带后缀的归约族注册 `numeric.reduce_sum`、`numeric.reduce_minimum`、`numeric.reduce_maximum`、`numeric.reduce_mean`、`numeric.reduce_count`、`numeric.reduce_variance` 和 `numeric.reduce_std`，每个 key 都有上述三种后缀。这些 key 使用固定 CPU task，要求以逗号分隔的 String `axes`，其中轴下标是唯一、canonical、非负整数；sum、mean、variance、standard deviation 还要求 String `dtype`（`uint8`、`int64`、`float32` 或 `float64`），并受输入与算子数值域限制；variance 和 standard deviation 要求 Int64 `ddof` 满足 `0 <= ddof < N`，其中 `N` 是被归约轴的 extent 乘积。被归约的轴输出长度为一，未归约轴保留原 extent。实现拒绝无效轴、不支持的 dtype 组合、超大输入 schema 以及错误的自由度参数类型；ddof 数值域为 unchecked 前提。这些 profile key 使用各自精确算术和输出转换，与上文未带后缀、采用固定有序 task 的 `numeric.mean`、`numeric.variance` 是不同注册项。

每个选中输出 tile 声明覆盖其完整归约组的来源 tile，并使用私有的预分配 exact aggregate/moments scratch。逻辑 row-major 顺序决定 NaN 优先级，物理 tile 布局与完成顺序不改变该规则。整数最终结果可表示是 unchecked 调用者前提。Count 仅声明 schema 引用，将静态数量写入选定的 dense Int64 输出 tile，不需要来源 payload 或算术 scratch。Empty 不生成任务。参见[固定归约执行](../../built-in_ops/01-numeric/reductions-execution.md)。

## 固定前缀与积分扫描 profile

六个 `numeric.prefix_sum_<profile>` 和 `numeric.integral_image_<profile>` key 使用固定 CPU task。Prefix 要求 Int64 `axis`；integral 要求逗号分隔的 String `axes`，指定两个不同轴。两者均要求同数值域的 String `dtype`。每个所选轴增长一，前导边界为整数零或浮点 +0。输出 `values` 使用 `photospider.tensor`/`samples`，完整输入 sample shape 转为普通 axes。

非扫描区域相同的 tile 共用一个 task。Task 读取与所需来源前缀包围区域相交的全部输入 tile，并写入独立 dense 输出 tile。仅包含零边界的 tile 不扩大前缀；仅包含这类 tile 的组不读取输入 payload，也不使用算术 scratch。Empty 不生成 task。精确来源 carry 保持 NaN 优先级和浮点溢出后的精确抵消。私有 scratch 包含固定精确状态；integral 还为每个所需来源列保存一个 carry。所有声明写入的整数可表示性都是 unchecked 前提。`select_outputs` 保留完整 task，包括其 reads/writes/scratch；重新编译可以缩小前缀。参见[固定扫描执行](../../built-in_ops/01-numeric/scans-execution.md)。

## 固定布局变换 profile

九个 `array.{reshape,transpose,slice}_<profile>` key 通过固定 CPU task 按位拷贝全部七种数值 dtype，每个选中输出 tile 独立 dense 分配。Reshape 映射全局 row-major ordinal，transpose 置换逻辑轴，slice 使用动态 Int64 starts/steps 和正整数静态 counts。旧 `layout` 参数及 `TransformLayout` 枚举已移除。输出 `values` 使用 `photospider.tensor`/`samples`，目标轴均为普通轴。

Reshape 和 transpose 精确声明静态映射触及的输入 tile。Slice 声明任意合法控制可能访问的 tile 并集，每轴最多贡献两个区间。运行时以加宽算术直接计算来源坐标，完整 slice 端点合法性为 unchecked 前提。全 singleton slice 不读取 steps payload；Empty 不生成 task。Prepare 校验完整 read/write slots。来源仿射 stride 可以为负数或零；运行时使用有界栈状态，无 payload scratch。参见[固定布局变换](../../built-in_ops/01-numeric/layouts-execution.md)。

## 固定索引 profile

十八个 `array.{concatenate,gather,scatter_replace,scatter_sum,scatter_minimum,scatter_maximum}_<profile>` key 使用固定 CPU task，值类型为 UInt8/Int64/Float32/Float64。Concatenate 接受 2..256 个输入，移除布局策略，每个选中输出 tile 只声明相交的来源 slab。Gather/scatter 在共享任务中为每个调用从完整动态 Int64 indices 构造 table，indices 合法性为 unchecked 前提；scatter 还稳定排序 target/position 对。结果写入私有 dense Int64 `[M,2]` tile，由输出任务消费。

每个消费者声明对应非 axis 区域以及全部可能的 axis 位置；scatter 另读取其 base 输出区域。No-hit 和 Replace 保留原始位。Aggregate 按 base 优先、updates 原 position 顺序执行精确算术和最终转换；整数 sum 可表示性是 unchecked 调用者前提。读取来源时直接信任私有 normalized position 及其来源关联，不做边界检查。每个选中输出 tile 独立分配；Empty 不生成 task。排序使用 `16*M` 私有 payload 和 `16*M` task scratch，并受执行器逐 lane scratch 预留规则约束。参见[固定索引执行](../../built-in_ops/01-numeric/indexing-execution.md)。

## 固定 constant 与 broadcast profile

六个 `numeric.{constant,broadcast}_<profile>` key 通过固定 CPU task 保留全部七种数值 dtype 的原始位。Constant 要求来源 sample shape 为 `[1]`；broadcast 将每个完整来源轴映射到不同目标轴，来源 extent 为一或等于对应目标 extent。两者均使用静态目标 shape、普通输出轴，并为选中 tile 独立 dense 分配。Layout 参数和 `ArrayLayout` 枚举已移除。

每个 constant task 读取一次标量，以最多 64 KiB 的无重叠块拷贝填充 tile。Broadcast 声明精确映射的来源矩形，使用有界栈状态拷贝原始样本。两者 scratch 均为零；Empty 无任务。超大目标的稀疏请求只为所选输出 tile 预留容量。Prepare 在进入 kernel 前验证 canonical 输出及完整来源 views。参见[固定数组执行](../../built-in_ops/01-numeric/arrays-execution.md)。

## 固定序列 profile

六个 `numeric.{linspace,arange}_<profile>` key 接受两个标量 Result、`[1,1048576]` 范围内的静态 count 和 dtype。浮点模式允许混合 Float32/Float64 输入，输出指定的 Float32/Float64 values；arange 另支持两个 Int64 输入及 Int64 输出。每个选中 values tile 对应一个固定 CPU task；axis 是独立选择的原子 `[3]` task，使用 Float64 或 Int64。Count 为一时只读 start，仍校验两个 schema；其他 count 读取两个标量 payload。

每个样本按全局 index 执行精确算术后直接舍入。实际读取的输入和选中输出须满足有限性／可表示性前提，运行时不做数值校验。未选中坐标不计算。任务共享一个 IR call 的发布结果，使用各 invocation 私有 scratch 和独立输出分配。Empty 无任务。参见[固定序列执行](../../built-in_ops/01-numeric/sequences-execution.md)。

## 固定排序与分位数 profile

六个 `array.sort_<profile>` 和 `numeric.quantile_<profile>` key 接受 UInt8/Int64/Float32/Float64 source 和静态 axis。Sort 按非 axis 矩形合并所选 values／indices tiles，每个 task 对每条完整源轴线排序一次，仅分配所选输出 tiles。Quantile 对每条选中轴线生成一个值，精确计算二进制 rank 与插值后最终 RN。源 NaN、Inf 和 signed zero 遵循指定的排序／转换语义；sort 保留原始位模式。

每个 task 按源 grid 声明完整轴线的 tile 支持。Quantile 在轴长大于 1 时读取标量 q，否则只记录 schema。活动 q 须有限且位于 `[0,1]`，地址与移位运算直接使用这一 unchecked 前提，不设置 guard。scratch 包含两个 L-word 缓冲和固定比较／精确算术状态，在轴线之间复用。各 Run 独占 scratch，任务共享其 IR call 的发布结果。参见[固定排序执行](../../built-in_ops/01-numeric/ordering-execution.md)。

## 固定微积分 profile

六个 `numeric.{derivative_1d,integrate_1d}_<profile>` key 使用同 dtype 的 Float32/Float64 `[N]` samples 与标量控制输入。Derivative 每所选输出 tile 声明上下两点 stencil 的来源 tile 并集及 step；source axis 为普通轴且 extent 为一时，内部点不依赖自身独占的 source tile；原子 source tile 的取整仍可覆盖该中心点。Integration 用一个 task 处理所有所选正索引 tiles，精确累计到最大选中坐标，只转换选中输出。独占索引零的 tile 具有单独的 initial-only task，零 scratch，samples／step 只有 schema 引用。

活动 step 由调用方保证有限且非零。IEEE source 和 initial 按离散公式处理。前缀 carry 在最终转换后保留，各 invocation 使用固定 ExactCalculus scratch，不分配 payload。并发 Run 仅共享不可变 preparation。进入 kernel 前校验 source／control 闭包、canonical writes 与 scratch。参见[固定微积分执行](../../built-in_ops/01-numeric/calculus-execution.md)。

## 固定表达式 profile

三个 `numeric.sample_expression_<profile>` key 解析一个有界不可变 AST，接受混合 Float32/Float64 标量端点和具名系数。每个所选 values tile 由一个固定 CPU task 执行，使用全局 index 的端点加权 RN64 坐标。Axis 是单独的原子 Float64 三元组 task，不读系数。Count 为一时排除 end payload，所有静态 schema 仍校验。Strict 对每个 AST 原语舍入后传给父节点；accelerated 批量包围完整 strict 参考结果，并严格重算不确定样本。

函数域、输入／中间结果／输出有限性和采样精度均为 unchecked 调用方前提。Preparation 校验 canonical 输出、精确的活动标量读取和固定 scratch。稀疏请求只分配所选 tiles；各 Run 共享 AST、独占算术状态。选中任务共享 call 发布结果。参见[固定表达式执行](../../built-in_ops/01-numeric/expression-execution.md)。

## 固定曲线插值 profile

十二个 `curve.interpolate_{linear,pchip}[_multi]_<profile>` key 使用固定 CPU task。各所选输出 tile 声明完整 x、对应 query 行与所选列范围内的完整 y 行，按每个源自身 grid 闭包。任务直接搜索输入视图，在本 tile 各列间复用查询分类，只计算选中全局输出。Scratch 是每 invocation 独占的完整 ExactCurve 算术状态。

实际消费的输入有限性、x 严格递增、reject 域和输出可表示性为调用方前提。Strict／Float64 完整公式一次 RN 舍入，accelerated Float32 仅接受唯一舍入的区间结果，否则精确重算。四个插值 baking 与四个 resampling 模板可通过固定执行器运行；其 query／axis／positions 依赖由展开节点决定。参见[CRV-01 固定契约](../../built-in_ops/01-numeric/op_specs/CRV-01_interpolate.md#fixed-execution-resources-and-caller-preconditions)。

## 固定反曲线 profile

六个 `curve.invert_{linear,pchip}_<profile>` key 与正向插值共享固定适配器。每个所选输出 tile 读取完整 x/y 和对应 query 行，在 y 中按单调方向搜索，再求原数学曲线的反函数。各 invocation 独占预分配 ExactCurve scratch；只计算并分配所选全局输出，不保留提升后的 x/y 数组。

x/y 全部元素有限、x 严格递增、y 全局严格递增或递减且无相等的相邻值，均为调用方前提；消费的 query 须有限、reject 下处于 y 域内，选中输出须可表示。精确格点／中点舍入、零符号、区间认证和内部容量检查保持。参见[CRV-10 固定契约](../../built-in_ops/01-numeric/op_specs/CRV-10_invert.md#fixed-execution-resources-and-caller-preconditions)。

## 固定动态轴 LUT1D profile

六个 `curve.apply_lut1d[_channels]_<profile>` key 使用固定 CPU task，保持完整输入 shape。L>1 按各源 grid 读取选中 query、所选列的完整 table 行及 axis 端点。二分查找按需重建原定义的端点加权 RN64 knot，使用固定大小的 ExactSampling/ExactCurve scratch；只分配选中输出 tiles。

L=1 直接转换所选 table 行，零 scratch；input/axis 保留静态 schema 检查但没有 payload 依赖。完整轴有效性、step 一致性、消费值有限性、reject 域及输出可表示性均为 unchecked 调用方前提，包括单项表 reject 的相等要求。参见 [CRV-05 固定契约](../../built-in_ops/01-numeric/op_specs/CRV-05_apply_lut1d.md#fixed-execution-resources-and-caller-preconditions)。

## 固定参数 Bezier profile

三个 `curve.evaluate_bezier_<profile>` key 为每个所选输出 tile 执行一个固定 CPU task。Segment/t 读取对应 query 行；anchors/handles 读取所选列的全部候选段，并按各源 grid 闭包。动态段索引合法性为 unchecked 前提，不设置地址 guard。任务在各列间复用行分类，只写所选全局输出，各 invocation 独占固定 ExactPolynomial/ExactSampling scratch。

t 有限且位于 [0,1]、消费的 controls 与 RN64 重建值有限、输出有限且可表示，均为 unchecked 调用方前提。端点选择与精确多项式舍入保持零符号。当前全部 profile 使用同一精确数值算法。参见 [CRV-03 固定执行](../../built-in_ops/01-numeric/op_specs/CRV-03_evaluate_bezier.md#fixed-execution-and-physical-demand)。

## 固定 Bezier 函数采样

三个 `curve.sample_bezier_function_<profile>` key 为每个所选 values tile 读取完整 controls 和活动 endpoints，直接搜索 anchor 并对所选段作 RN64 重建。精确求逆／求值保持原数学函数。Axis 是单独的 scalar-only task；count=1 排除 end 且 axis 零 scratch。所有静态 schema 仍校验，选中任务共享 call 有序发布结果。

全局 x 拓扑与所选数值条件为调用方前提。Values 要求所选坐标与邻点分离；仅在 N>1 且 axis 被选中时要求 step 非零且可表示。各 invocation 独占固定算术 scratch，只分配所选输出 tiles。Bezier LUT baking 模板通过此 sampler 执行。参见 [CRV-02 固定执行](../../built-in_ops/01-numeric/op_specs/CRV-02_sample_bezier_function.md#fixed-execution-and-dependency-projection)。

## 固定对数 shaper

六个 `curve.log2_shaper[_inverse]_<profile>` key 为每个所选输出 tile 执行一个固定 CPU task，按源 grid 读取对应 input 区域及两个同 dtype 的 scalar bounds。有限 0<lower<upper 为调用方前提。Query 的 NaN payload、signed zero、Inf、域扩展及成功的 IEEE overflow/underflow 按族契约处理，不适用通用 finite-input 假设。

各 invocation 独占预分配 ExactShaper 状态。完整公式的精确／认证算术用固定字段记录 retry、range 和 failure，这些状态转换不分配异常对象。只分配所选输出 tiles，所需任务共享 call 有序发布结果。线性 shaper 保持固定 remap/constant 组合。参见 [CRV-08 固定执行](../../built-in_ops/01-numeric/op_specs/CRV-08_shaper.md#fixed-log-execution)。

## 显式 NUM 颜色描述

CRV-06/07/09 使用算子 SDK 的 `TensorDescription` 现有 v4/v5 codec。
`ps::numeric::color_tuple_description` 构造单个逻辑 tuple：`channel_axis=0`、
一个按规范顺序排列的完整颜色 group，以及可选的独立 straight alpha。
物理端口仍按各自契约使用末尾分量轴。FMT 描述的是实际输入张量的轴；
CRV 接 FMT 时须显式编写该轴映射，坐标单位不变。

CIELAB/CIELCh 存储 `l=L*/100`，opponent/chroma 与 hue 圈数不变。
RGB ramp 输入输出为 straight，内部精确 alpha 加权保持不变；其他 ramp
和 LUT 消费者保留各自的分量数限制。旧 ColorArray 类型／codec 和 RGB
association 选项已移除。描述不附着于 Result，也不增加样本扫描。

烘焙内部 owned table 和 report 为 schema v4。报告仍含 13 个成员／361 字节；
输入、输出的完整 TensorDescription 编码均进入
`photospider.bake-configuration.v2` 配置摘要域。旧 v3 表／报告被拒绝。
内核接口和 artifact 容器未改变。参见
[迁移记录](../../built-in_ops/01-numeric/NUM-CRV_color_migration.md)。

## 固定三维 LUT 应用

六个 trilinear/tetrahedral LUT3D key 为每个所选完整颜色输出 tile 生成一个固定 CPU task。读取区域包含对应 query、完整 table 和六个 axis 端点，各按源网格闭包。数值轴一致性与顺序、原始 query 合法性、贡献 table 颜色合法性、reject 域内条件及有限输出是调用方前提。仅正权完整顶点进入精确算术。每个 invocation 独占固定 knots/算术 scratch，写入具有一个 atomic trailing component axis 的 dense 输出 tile；Empty 不生成 task。参见 [CRV-07](../../built-in_ops/01-numeric/op_specs/CRV-07_apply_lut3d.md)。

## 固定 color ramp

45 个 color-ramp key 为每个所选完整颜色输出 tile 生成一个固定 CPU task。Query 读取对应输出 tile；stops/colors 和可选 rational hue 数组按各自网格形成完整保守闭包。Kernel 直接二分读取 stops，并以固定精确／区间算术 scratch 计算一行或两行完整颜色。有限有序 stops、所选模型／association 合法性、正 rational denominator、reject 域内条件和输出可表示性是调用方前提。Empty 不生成 task；CMYK 仍在 operation preparation 中校验所选冻结 ICC 资源。参见 [CRV-06](../../built-in_ops/01-numeric/op_specs/CRV-06_color_ramp.md#fixed-execution-and-caller-preconditions)。

## 固定 uniform low-pass

15 个 `curve.lowpass_uniform_*` key 为每个所选输出 tile 生成一个 ABI 8 CPU task。静态非零 tap 与 reflect/replicate/zero/wrap 映射确定源区间，再按源自身的 tile 网格闭包。闭包附带的物理样本不成为数值操作数。输出将完整 shape/dtype 保留为普通轴，atomic trailing axes 为零，仅分配所选 tiles。每个 invocation 借用 affine 源 views，独占固定算术、样本及系数 scratch。Empty 不生成 task。

NaN/Inf 样本保留规格规定的逻辑 tap 顺序聚合。Strict 对精确归一化总和舍入一次；accelerated 候选通过最终 FP32 界检查，否则执行 strict 算术。正 normalizer 为 unchecked 前提；区间细化用于计算除法和最终舍入，不增加单独的定义域校验。Checked 算术在数值重试之前直接返回取消和资源失败。参见 [uniform low-pass 契约](../../built-in_ops/01-numeric/op_specs/CRV-11_uniform_lowpass_contract.md)。

## 固定 nonuniform low-pass

15 个 `curve.lowpass_nonuniform_*` key 为每个所选输出 tile 生成一个 ABI 8 CPU task。读取区域包括完整 positions，以及所选其他轴坐标对应的完整过滤轴 values，并按各自源网格闭包。Kernel 直接搜索 positions；每个 invocation 流式遍历精确的正长度片段，仅在固定 scratch 中保留当前两个片段。多项式数组包含 513、513 和 1025 个区间。仅分配所选输出，输出使用普通轴且 atomic trailing axes 为零；Empty 不生成 task。

有限且严格递增的 positions、有限的贡献端点及有限可表示的所选输出是调用方前提。Strict 和 accelerated key 当前使用相同的认证连续积分与精确 paired-affine 捷径。局部常量保留原始位，包括 -0。取消和资源失败在捷径或数值重试前直接传播。大支持区可能跨越很多周期，每个片段和算术循环都检查取消。参见 [nonuniform low-pass 契约](../../built-in_ops/01-numeric/op_specs/CRV-11_nonuniform_lowpass_contract.md)。

## 固定 LUT3D 烘焙几何

15 个 `curve.bake_lut3d_{axis,grid,points,points_extra,color}` profile key 为每个所选输出 tile 生成一个固定 CPU task。Axis 复制所选单元。Grid 和 center task 读取六个端点，以固定 ExactSampling scratch 重建 RN64 knot；center 对实际相邻 knot 求平均。纯 extra tile 仅读取所选 extra 行，不读取 axis payload，scratch 为零。Color 同 dtype 复制原位，或按 RN 转换一次，scratch 为零。源读取按各自网格闭包；输出保留全局坐标，除 axis 外均有一个 atomic trailing component axis。Empty 不生成 task。

有限一致的 axis、模型合法颜色、域内 extra 及有限可表示的转换结果是调用方前提。直接 invocation 借用有符号 stride／非对齐 views，独占 scratch，仅写入所选 dense tiles。Pack/Measure/Unpack/Gate 使用固定 CPU invocation。Pack/Unpack/Gate 将所选样本复制到独立 dense tile。非空 Measure 等待完整 owned table，读取全部验证点、reference 和 applied 颜色，并以固定精确整数 scratch 写出全部 13 个报告成员。报告和 table 投影保留全局测量闭包。即使后续 Gate 失败，退休 observer 仍可保留报告。报告 v4 从 invocation 关联数组取得实际 owned-table ObjectId。Gate 直接信任配置／table 关联与报告一致性，只读取 passed 和 first_failure_index；显式测量未通过时返回 OperationFailed/InvalidDomain。Gate 不需要算术 scratch。SDK reader 使用静态准备的 Lut3dBakeReportLayout 解码，不重查 schema、身份或 payload。参见 [CRV-09](../../built-in_ops/01-numeric/op_specs/CRV-09_bake_lut3d.md#fixed-geometry-tasks)。

## Backend 选择

主机无法选用指定 profile 时，准备阶段会失败；需要可移植路径时应选择 strict 后缀。Profile 名称不代表 GPU 执行。

## 输入、所有权与错误

Numeric programs 使用 Result inputs 和 outputs。固定 kernel 借用物理 Plan 声明的 views，并写入预分配输出 tile。共享中间计算使用显式 task 与私有 value。具体 preparation、shape 和 batch 映射按算子族规定。

静态参数非法返回 `InvalidArgument`。dtype、shape 或 schema metadata 不支持或不兼容返回 `TypeMismatch`。八个未带后缀的数值逐元素 key 要求满足有限域输入前提。固定 profile 算子族遵循各自数值规则，包括规格定义的 IEEE 特殊值。固定有序归约和 scan 同样要求满足有限域输入前提。固定插值 profile 按规格保留 IEEE 特殊值，调用者须提供有效 factor 或 edges。资源耗尽和取消保留各自操作状态码。固定执行器在所有声明的 task 写入成功后，按退休顺序公开成功调用的 Result。

## 公开 workflow 与检查

算子修改期间，使用现场编写的独立数值参考和公开 Result workflow 验证算术，
完成后删除检查程序，具体规则见[测试与验证](../../development/zh/Testing-and-Validation.zh.md)。
保留的内核 [test_fixed_execution](../../../tests/integration/kernel/test_fixed_execution.cpp) 使用合成算子检查并发发射、有序退休、取消排空和 observer 失败。它不构成内置算子的数值正确性验证。

通用有限域算术接受同完整 sample shape 的兼容单 Tensor Float32/Float64 输入，包括结构合法的空间输入。不要求独立 planar capability 声明，也不自动绑定 RGBA validator。
