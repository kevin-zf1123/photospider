# 数值算子

默认 registry 提供未带后缀的有限域算术与 clamp、分阶段标量归约、有序扫描，以及带 profile 的数值算子族。各 key 的参数、执行和输出契约不同，调用方应按完整 key 选择。这些是 `plugins/ops/01-numeric/` 中的 bundled operations，不是内核代码；内核只提供它们所用的通用 Result 执行、资源、取消和失败契约。Operation plugin 使用 Result operation C ABI 6。数值工作报告、LUT bake 报告和质量报告是算子 SDK 类型（`photospider/ops/numeric/`）；见 [Atom 错误与质量](Atom-Errors-and-Quality.zh.md)。

## 未带后缀的 Result 数值算子

| Key | 输入与输出 | 参数和区域 |
| --- | --- | --- |
| `math.add` | 两个 Float64 tensor Results，各含一个 sample（`sample_shape() == [1]`）；输出 port `value` 为 `photospider.tensor` v1 Result/member `samples`，shape `[1]` | 无参数；Dependency；IEEE addition |
| `numeric.add`、`numeric.subtract`、`numeric.multiply`、`numeric.divide` | 两个 Float32/Float64 Result 输入，完整 `sample_shape()` 和 dtype 相同；输出 port `value` 为 `photospider.tensor` v1 Result，tensor member `samples` 保留 shape 和 dtype | 无参数；complete-input |
| `numeric.clamp` | 一个 Float32/Float64 Result 输入；输出 port `value` 使用相同 Result schema 并保留 shape 和 dtype | 必填有限 Float64 `min`、`max`，满足 `min <= max`；complete-input |
| `numeric.abs` | 一个 Float32/Float64 tensor Result；输出 port `value` 为 `photospider.tensor` v1/member `samples`，sample shape 与 dtype 相同 | 无参数；tile continuation |
| `numeric.minimum`、`numeric.maximum` | 两个完整 sample shape、dtype 相同的 Float32/Float64 tensor Results；输出 port `value` 为 `photospider.tensor` v1/member `samples` | 无参数；tile continuation |

每个输入 Result 恰含一个 tensor 且；接受结构有效的任意 schema id/version 与 tensor member key。rank 为 1..8，`sample_shape()` 包含 batch axes。输出将完整 shape 作为普通 tensor axes；输入的描述或解释不会带到输出。各 program 从授权窗口读取输入，并按成员 tile 发布契约 通过 Result transaction 发布输出。

`numeric.add`、`numeric.subtract`、`numeric.multiply`、`numeric.divide` 和 `numeric.clamp` 在输入 dtype 中计算，并拒绝非有限输入或结果。除法拒绝正负零。Clamp 要求静态边界有限，且在不可变 preparation 中检查 `min <= max`。Clamp 在 Float32 收窄前检查所选结果；未选端点可以超出 Float32 范围。算子不广播或转换 dtype。不可变 preparation 还会检查 dense output 字节数；若 64 位计数无法表示，则在 execution 前以 `ResourceExhausted` 失败。

`math.add` 是独立的标量 Result 算子。Metadata preparation 要求每个输入恰含一个 Float64 tensor，完整 `sample_shape()` 为 `[1]`；输出保持标量 shape。非空请求以 普通 tile Needs 读取两侧的单个 sample。Callback 执行普通 IEEE Float64 addition，因此 NaN 和 infinity 按该加法传播，与 finite-domain `numeric.add` 契约不同。Empty demand 不读取输入。

`numeric.add`、`numeric.subtract`、`numeric.multiply`、`numeric.divide` 和 `numeric.clamp` 的非空 complete-input 请求对每个完整输入 tensor 声明 普通 payload tile Needs。程序从授权 Result 窗口读取，不收集输入 tensor，也不发布部分结果。Empty 请求只执行静态 preflight，不读取样本 payload。没有额外的 2^40 逻辑元素上限，因此只要 dense output 可表示，大型合法零步长 source 仍可使用。算术 callback 会恢复调用方的浮点环境。

未带后缀的有限 `abs`、`minimum` 和 `maximum` 接受恰含一个 tensor 的输入 Result，schema id、version 和 member key 可以任意但必须结构有效；各端口必须具有相同 Float32/Float64 dtype 和完整 `sample_shape()`（rank 1..8，含 batch axes）。输出为通用 `photospider.tensor` v1 Result/member `samples`，保留完整 shape 并将其作为普通 axes。它们使用 CPU tile continuation Needs，不采用同名 suffixed profile 的 complete-input 契约。

对每个非空输出 tile，算子把数学支持闭包后转换为当前输入成员 tile Need，授权可以因 tile 网格而超出精确样本区域。读取、校验和失效使用实际已准入 tile 证据，不使用 Data/Validation 数值 role 或 relation。空 payload demand 不启动 producer。

这些有限 kernel 在算术遇到非有限输入或中间值时返回 `OperationFailed`。`abs(-0)` 返回 `+0`；`minimum(-0,+0)` 返回 `-0`，`maximum(-0,+0)` 返回 `+0`。Callback 会恢复调用方浮点环境。其 finite-only 错误和稀疏 Dependency 契约不同于独立注册的 suffixed complete-input profile keys；suffix key 各自遵循 IEEE 规则，包括每个操作各自的 signed-zero 行为。

## 分阶段 Result 归约与有序 scan

| Key | 输入与输出 | 参数和区域 |
| --- | --- | --- |
| `numeric.mean`、`numeric.variance` | 一个 rank 1..8 的 Float32/Float64 tensor Result；`value` 输出 port 为 Float64 scalar Result，member `samples` shape `[1]` | 可选 Int64 `block_size`，范围 `[1,65536]`，默认 64；分阶段 tensor Needs |
| `numeric.ordered_scan` | 一个 rank-1 Float64 tensor Result；`value` 输出 port 为同 shape Float64 Result，member `samples` | 可选 Int64 `block_size`，范围 `[1,65536]`，默认 64；Run 共享完整输入 stage |

Mean 和 variance 通过 tile Need 按逻辑 row-major 区间读取一个 Float32/Float64 Tensor。在第一遍算术中逐样本检查有限性，不使用默认 typed-image validator。Mean 按顺序用 Float64 累加，variance 执行两遍、使用 population 分母（ddof=0）。有序状态跨 poll 保持加法顺序。block_size 选择名义区间，必要时向完整 atomic tuple 取整。二者发布一个 Float64 scalar；非有限输入或不可表示累加失败。这些普通 scalar actor 可以使用 block 服务，与下述共享 ordered_scan 分开。

`numeric.ordered_scan` 接收 rank-1 Float64 输入，输出 `value` 为同 shape 的 `photospider.tensor` v1/member `samples`。非空 tile 请求启动 Run 内共享的一次完整输入计算。非阻塞 tile Need 授权完整输入后，从正零开始按逻辑索引严格左折执行 Float64 inclusive addition。可选 Int64 `block_size` 范围 1..65536、默认 64，只控制 work 计费和取消检查间隔，不改变加法分组。输入任意位置出现非有限值或累加溢出均按全局索引失败，包括请求输出 tile 之后的位置。输出 actor 复用共享结果。本算子不使用 prefix checkpoint 或 block-cache 复用。

这三个操作遇到 Empty 输出 demand 时执行静态 preflight，并返回没有 tensor coverage 的 sealed Result，不发出 input Need。

通用 block 服务使用已 seal、无 publication scope 的单 Tensor 状态，与上述 Run 共享 scan 分开。共享 stage 不提供 block 或 checkpoint 服务，可选转换保留不承担唯一 producer 保证。见 [Dependency Data](Dependency-Data.zh.md)。

## 带 profile 后缀的逐元素算子

数值 math 算子族使用三个显式后缀：

| Profile 后缀 | 运行准入 |
| --- | --- |
| `_strict` | 可移植 strict 实现 |
| `_accelerated_apple_silicon` | Apple Silicon 实现，仅在运行 profile 可用时准入 |
| `_accelerated_x86_64` | x86-64 加速实现，仅在运行 profile 可用时准入 |

`numeric_binary.cpp` 为每种后缀注册 `add`、`subtract`、`multiply`、`divide`、`minimum`、`maximum`、`pow`、`atan2` 和 `atan2pi`。精确 elementary kernel 对适用操作接受同 dtype 的 UInt8、Int64、Float32 和 Float64；divide 仅支持 Float32/Float64。认证超越函数使用 Float32/Float64，并检查各自定义域和可表示性。这些 key 使用 Run-shared complete-input computation，保留 dtype 和 shape。NaN 按对应精确 kernel 的规则传播或分类；定义域与范围错误遵循各自算子族的类型化契约。`numeric.clamp_<profile>` 和 `numeric.remap_range_<profile>` 的无效边界返回 `InvalidArgument/InvalidDomain`。加速 profile 不可用或不适合当前后端策略时可使用 strict key。

带后缀的归约族注册 `numeric.reduce_sum`、`numeric.reduce_minimum`、`numeric.reduce_maximum`、`numeric.reduce_mean`、`numeric.reduce_count`、`numeric.reduce_variance` 和 `numeric.reduce_std`，每个 key 都有上述三种后缀。这些是 完整输入共享 stage，要求以逗号分隔的 String `axes`，其中轴下标是唯一、canonical、非负整数；sum、mean、variance、standard deviation 还要求 String `dtype`（`uint8`、`int64`、`float32` 或 `float64`），并受输入与算子数值域限制；variance 和 standard deviation 要求 Int64 `ddof` 满足 `0 <= ddof < N`，其中 `N` 是被归约轴的 extent 乘积。被归约的轴输出长度为一，未归约轴保留原 extent。实现拒绝无效轴、不支持的 dtype 组合、超大输入以及非法自由度。这些 profile key 使用各自精确算术和输出转换，与上文未带后缀、采用分阶段依赖的 `numeric.mean`、`numeric.variance` 是不同注册项。

`numeric.clamp_<profile>` 和 `numeric.remap_range_<profile>` 使用相同后缀。Clamp 按值、下界、上界顺序接收三个 shape/dtype 相同的输入；remap 按值、来源下界、来源上界、目标下界、目标上界顺序接收五个输入。两者保留输入 shape 和 dtype，要求 rank 1..8，使用 Run-shared complete-input computation，并在执行时拒绝无效边界。该 clamp 算子族与未带后缀、使用两个静态参数的 `numeric.clamp` 不同。

前述未带后缀 mean/variance 在 seal 的单 Tensor Result 中携带有序状态，可使用可选 block 保留。带 profile 后缀的归约在共享 stage 内计算 exact aggregate/moments，不调用 block 服务。其中 reduce_count 使用空输入投影，根据静态元数据计算数量，不发 payload Need。

Profile-specific 逐元素和归约实现通过执行资源预算计量 work 和取消。主机无法选用指定 profile 时，准备阶段会失败；需要可移植路径时应选择 strict 后缀。Profile 名称不代表 GPU 执行。

## 输入、所有权与错误

本页所述 numeric programs 使用 Result inputs 和 outputs。Tensor program 保留其 Result schema，并仅读取 Need 授权的 windows；具体 preparation、shape、batch 映射和发布要求按算子族规定。`Tensor` 仍可作为私有 typed backing。

静态参数非法返回 `InvalidArgument`。dtype、shape 或 schema metadata 不支持或不兼容返回 `TypeMismatch`。未带后缀有限域算术、分阶段归约和 scan 会按契约将非有限或不可表示情况报告为 operation failure。带后缀精确 kernel 按操作保留或分类 IEEE 值；类型化定义域错误可返回 `InvalidArgument/InvalidDomain`。资源耗尽和取消保留各自状态码。complete-input Result 操作仅在完整输出成功后发布；staged Result operation 按已声明的 固定发布范围 发布。

## 公开 workflow 与检查

算子修改期间，使用现场编写的独立数值参考和公开 Result workflow 验证算术，
完成后删除检查程序，具体规则见[测试与验证](../../development/zh/Testing-and-Validation.zh.md)。
保留的内核 [test_scan_waiters](../../../tests/integration/kernel/test_scan_waiters.cpp)
使用合成 scan 检查并发执行间的 checkpoint 共享、取消隔离、错误传播、缓存清除与 Queue 资源释放。
它不构成内置 scan 算子的数值正确性验证。

通用有限域算术接受同完整 sample shape 的兼容单 Tensor Float32/Float64 输入，包括结构合法的空间输入。不要求独立 planar capability 声明，也不自动绑定 RGBA validator。