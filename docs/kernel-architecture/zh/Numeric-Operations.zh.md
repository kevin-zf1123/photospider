# 数值算子

默认 registry 提供未带后缀的有限域算术与 clamp、分阶段标量归约、有序扫描，以及带 profile 的数值算子族。各 key 的参数、执行和输出契约不同，调用方应按完整 key 选择。Operation plugin 使用 Result operation C ABI 2。

## 未带后缀的 Result 数值算子

| Key | 输入与输出 | 参数和区域 |
| --- | --- | --- |
| `math.add` | 两个 Float64 tensor Results，各含一个 sample（`sample_shape() == [1]`）；输出 port `value` 为 `photospider.tensor` v1 Result/member `samples`，shape `[1]` | 无参数；Dependency；IEEE addition |
| `numeric.add`、`numeric.subtract`、`numeric.multiply`、`numeric.divide` | 两个 Float32/Float64 Result 输入，完整 `sample_shape()` 和 dtype 相同；输出 port `value` 为 `photospider.tensor` v1 Result，tensor member `samples` 保留 shape 和 dtype | 无参数；Whole |
| `numeric.clamp` | 一个 Float32/Float64 Result 输入；输出 port `value` 使用相同 Result schema 并保留 shape 和 dtype | 必填有限 Float64 `min`、`max`，满足 `min <= max`；Whole |
| `numeric.abs` | 一个 Float32/Float64 tensor Result；输出 port `value` 为 `photospider.tensor` v1/member `samples`，sample shape 与 dtype 相同 | 无参数；分阶段 Dependency |
| `numeric.minimum`、`numeric.maximum` | 两个完整 sample shape、dtype 相同的 Float32/Float64 tensor Results；输出 port `value` 为 `photospider.tensor` v1/member `samples` | 无参数；分阶段 Dependency |

每个输入 Result 恰含一个 tensor 且无 fields；接受结构有效的任意 schema id/version 与 tensor member key。rank 为 1..8，`sample_shape()` 包含 batch axes。输出将完整 shape 作为普通 tensor axes，不复制 source facets；输出 schema 自行选择 resources。各 program 从授权窗口读取输入，并按各自 region rule 通过 Result transaction 发布输出。

`numeric.add`、`numeric.subtract`、`numeric.multiply`、`numeric.divide` 和 `numeric.clamp` 在输入 dtype 中计算，并拒绝非有限输入或结果。除法拒绝正负零。Clamp 要求静态边界有限，且在不可变 preparation 中检查 `min <= max`。Clamp 在 Float32 收窄前检查所选结果；未选端点可以超出 Float32 范围。算子不广播或转换 dtype。不可变 preparation 还会检查 dense output 字节数；若 64 位计数无法表示，则在 execution 前以 `ResourceExhausted` 失败。

`math.add` 是独立的标量 Result 算子。Metadata preparation 要求每个输入恰含一个 Float64 tensor、没有 fields，完整 `sample_shape()` 为 `[1]`；输出保持标量 shape。非空请求以 role-13 Needs 读取两侧的单个 sample。Callback 执行普通 IEEE Float64 addition，因此 NaN 和 infinity 按该加法传播，与 finite-domain `numeric.add` 契约不同。Empty demand 不读取输入。

`numeric.add`、`numeric.subtract`、`numeric.multiply`、`numeric.divide` 和 `numeric.clamp` 的非空 Whole 请求对每个完整输入 tensor 声明 Data、Validation 和 Descriptor needs（role 13）。程序从授权 Result 窗口读取，不收集输入 tensor，也不发布部分结果。Empty 请求只执行静态 preflight，不读取样本 payload。没有额外的 2^40 逻辑元素上限，因此只要 dense output 可表示，大型合法零步长 source 仍可使用。算术 callback 会恢复调用方的浮点环境。

未带后缀的有限 `abs`、`minimum` 和 `maximum` 接受恰含一个 tensor、没有 fields 的输入 Result，schema id、version 和 member key 可以任意但必须结构有效；各端口必须具有相同 Float32/Float64 dtype 和完整 `sample_shape()`（rank 1..8，含 batch axes）。输出为通用 `photospider.tensor` v1 Result/member `samples`，保留完整 shape 并将其作为普通 axes，facets 为空。它们使用 CPU staged Dependency Needs，不采用同名 suffixed profile 的 Whole 契约。

对非空输出 Footprint Q，算子先按 `close_samples(Q)` 对每个输入执行 validation；该范围会闭合 typed tuple 和 atomic sample group，再请求 Q 上的 Data。若 `close_samples(Q)` 等于 Q，算子将 Data/Validation roles（1 和 4）合并到一个 Need；若闭合扩大 validation，则先发 role-4 Validation Need，再发 role-1 Data Need。算术 Data 读取仅限 Q；Validation 会读取并检查 `close_samples(Q)`，其中可能包含 Q 外的 typed tuple 或 atomic-group 成员。Need 不会扩展到 `close_samples(Q)` 之外。其 relation 分别记录 Data、Validation mapping 和 input descriptor basis。Empty 请求执行静态 preflight，返回没有 tensor coverage 的 sealed Result，不发 input Needs。

这些有限 kernel 在算术遇到非有限输入或中间值时返回 `OperationFailed`。Typed Result source validation 可在算术前以 `ErrorCode::InvalidArgument` 和 `FailureReason::InvalidDomain` 失败，并保留来源 `input_id`。`abs(-0)` 返回 `+0`；`minimum(-0,+0)` 返回 `-0`，`maximum(-0,+0)` 返回 `+0`。Callback 会恢复调用方浮点环境。其 finite-only 错误和稀疏 Dependency 契约不同于独立注册的 suffixed Whole profile keys；suffix key 各自遵循 IEEE 规则，包括每个操作各自的 signed-zero 行为。

## 分阶段 Result 归约与有序 scan

| Key | 输入与输出 | 参数和区域 |
| --- | --- | --- |
| `numeric.mean`、`numeric.variance` | 一个 rank 1..8 的 Float32/Float64 tensor Result；`value` 输出 port 为 Float64 scalar Result，member `samples` shape `[1]` | 可选 Int64 `block_size`，范围 `[1,65536]`，默认 64；分阶段 tensor Needs |
| `numeric.ordered_scan` | 一个 rank-1 Float64 tensor Result；`value` 输出 port 为同 shape Float64 Result，member `samples` | 可选 Int64 `block_size`，范围 `[1,65536]`，默认 64；prefix tensor Needs 与 checkpoints |

每个输入都是携带一个 tensor 的 Result。Mean 和 variance 接受 rank 1..8 的 Float32 或 Float64 完整逻辑 `sample_shape()`。它们按 row-major 顺序请求分阶段区间，只读取 Need 授权的 samples。Typed source validation 在算术前完成。Mean 按逻辑 row-major 顺序使用 Float64 累加。Variance 分两遍执行：第一遍求 mean，第二遍以总体方差分母（`ddof=0`）累加平方偏差。Carry state 保留阶段边界，不会重新结合 sum。两者都从 `value` port 发布单个 Float64 scalar tensor Result。`block_size` 限定 sample interval，不合批输出观察。非有限样本和不可表示的累加结果会失败。

`numeric.ordered_scan` 从正零 Float64 carry 开始，按从左到右顺序做 inclusive prefix 加法，使用 nearest-even 舍入和渐进下溢。非空输出请求从索引零读取到最大请求索引；Empty 请求不读取样本 payload。输出使用 `value` port。每个已发布 prefix 带有精确 prefix relation，继续执行前可从 checkpoint 恢复成功 carry。首个非有限输入或累加溢出会报告全局输入下标。

这三个操作遇到 Empty 输出 demand 时执行静态 preflight，并返回没有 tensor coverage 的 sealed Result，不发出 input Need。

`ResultProgramPhase::block` callback 执行一个显式状态转换。输入和返回状态必须是相同 schema、相同 Root 的 sealed `CompleteBundle` Result，各含一个完整覆盖的 tensor 且不含 fields。Tensor 必须保存后续转换所需的所有值，包括 carry controls。Callback 收到转换 kind、半开范围 `[begin,end)`、mode、输入状态和 compute callback。可选内部 retention 在 proof 或资源限制不允许时可以跳过；算子仍会正常计算。Block cache 命中数与转换复用属于实现细节，不构成性能保证。

## 带 profile 后缀的逐元素算子

数值 math 算子族使用三个显式后缀：

| Profile 后缀 | 运行准入 |
| --- | --- |
| `_strict` | 可移植 strict 实现 |
| `_accelerated_apple_silicon` | Apple Silicon 实现，仅在运行 profile 可用时准入 |
| `_accelerated_x86_64` | x86-64 加速实现，仅在运行 profile 可用时准入 |

`numeric_binary.cpp` 为每种后缀注册 `add`、`subtract`、`multiply`、`divide`、`minimum`、`maximum`、`pow`、`atan2` 和 `atan2pi`。精确 elementary kernel 对适用操作接受同 dtype 的 UInt8、Int64、Float32 和 Float64；divide 仅支持 Float32/Float64。认证超越函数使用 Float32/Float64，并检查各自定义域和可表示性。这些 key 使用 Whole Region，保留 dtype 和 shape。NaN 按对应精确 kernel 的规则传播或分类；定义域与范围错误遵循各自算子族的类型化契约。`numeric.clamp_<profile>` 和 `numeric.remap_range_<profile>` 的无效边界返回 `InvalidArgument/InvalidDomain`。加速 profile 不可用或不适合当前后端策略时可使用 strict key。

带后缀的归约族注册 `numeric.reduce_sum`、`numeric.reduce_minimum`、`numeric.reduce_maximum`、`numeric.reduce_mean`、`numeric.reduce_count`、`numeric.reduce_variance` 和 `numeric.reduce_std`，每个 key 都有上述三种后缀。这些是 Whole 操作，要求以逗号分隔的 String `axes`，其中轴下标是唯一、canonical、非负整数；sum、mean、variance、standard deviation 还要求 String `dtype`（`uint8`、`int64`、`float32` 或 `float64`），并受输入与算子数值域限制；variance 和 standard deviation 要求 Int64 `ddof` 满足 `0 <= ddof < N`，其中 `N` 是被归约轴的 extent 乘积。被归约的轴输出长度为一，未归约轴保留原 extent。实现拒绝无效轴、不支持的 dtype 组合、超大输入以及非法自由度。这些 profile key 使用各自精确算术和输出转换，与上文未带后缀、采用分阶段依赖的 `numeric.mean`、`numeric.variance` 是不同注册项。

`numeric.clamp_<profile>` 和 `numeric.remap_range_<profile>` 使用相同后缀。Clamp 按值、下界、上界顺序接收三个 shape/dtype 相同的输入；remap 按值、来源下界、来源上界、目标下界、目标上界顺序接收五个输入。两者保留输入 shape 和 dtype，要求 rank 1..8，使用 Whole Region，并在执行时拒绝无效边界。该 clamp 算子族与未带后缀、使用两个静态参数的 `numeric.clamp` 不同。

Continuation 在完整 tensor Result 中携带归约状态。Variance 第二遍的 state 包含第一遍 mean。Block-state retention 是可选实现细节；逐个重算转换时算子结果保持不变。

Profile-specific 逐元素和归约实现通过执行资源预算计量 work 和取消。主机无法选用指定 profile 时，准备阶段会失败；需要可移植路径时应选择 strict 后缀。Profile 名称不代表 GPU 执行。

## 输入、所有权与错误

本页所述 numeric programs 使用 Result inputs 和 outputs。Tensor program 保留其 Result schema，并仅读取 Need 授权的 windows；具体 preparation、shape、batch 映射和发布要求按算子族规定。`Value` 仍可作为私有 typed backing。

静态参数非法返回 `InvalidArgument`。dtype、shape 或 schema metadata 不支持或不兼容返回 `TypeMismatch`。未带后缀有限域算术、分阶段归约和 scan 会按契约将非有限或不可表示情况报告为 operation failure。带后缀精确 kernel 按操作保留或分类 IEEE 值；类型化定义域错误可返回 `InvalidArgument/InvalidDomain`。资源耗尽和取消保留各自状态码。Whole Result 操作仅在完整输出成功后发布；staged Result operation 按已声明的 prefix 或 dependency contract 发布。

## 公开 workflow 与检查

[`tests/integration/test_numeric_operations.cpp`](../../../tests/integration/test_numeric_operations.cpp) 覆盖注册的 arithmetic 行为。分族 Result numeric oracle 位于 [`tests/integration/numeric`](../../../tests/integration/numeric)，分别覆盖 arithmetic、arrays、sequences、statistics、matrix、calculus、curves、Bezier、LUT、baking、expression、filters 和 color ramps。Ordered reduction 与 scan 行为由 [`test_ordered_reduction.cpp`](../../../tests/integration/test_ordered_reduction.cpp)、[`test_ordered_scan.cpp`](../../../tests/integration/test_ordered_scan.cpp) 和 [`test_scan_waiters.cpp`](../../../tests/integration/test_scan_waiters.cpp) 覆盖。这些测试使用独立数值期望；block-cache 观察不构成性能证据。`installed_result_numeric` 是 `tests/consumer/CMakeLists.txt` 中的已安装 package consumer 入口。

Generic 算术 key 未声明 planar image capability。注册这些 key 不代表 structural planar image 可以作为 generic numeric array 输入。
