# 数值算子

默认 registry 同时提供未带后缀的 Value 算术 key 和带 profile 后缀的数值算子族。各族的参数、Region 和归约契约不同，调用方应按完整 key 选择，不能把后缀当作纯命名区别。Operation plugin 使用 C ABI 11。

## 未带后缀的 Value 算子

| Key | 输入与输出 | 参数和区域 |
| --- | --- | --- |
| `numeric.add`、`numeric.subtract`、`numeric.multiply`、`numeric.divide` | 两个 shape、dtype 相同的 Float32/Float64 generic Value；输出保留 shape 和 dtype | 无参数；Whole 输入/输出 |
| `numeric.clamp` | 一个 Float32/Float64 generic Value；输出保留 shape 和 dtype | 必填有限 Float64 `min`、`max`，并满足 `min <= max`；Whole |
| `numeric.mean`、`numeric.variance` | 一个 rank 1..8 Float32/Float64 Value；输出 Float64 scalar `[1]` | 可选 Int64 `block_size`，范围 `[1,65536]`，默认 64；分阶段依赖读取 |
| `numeric.ordered_scan` | 一个 rank-1 Float64 Value；输出同 shape Float64 Value | 可选 Int64 `block_size`，范围 `[1,65536]`，默认 64；前缀依赖读取 |

四个未带后缀的算术 key 在输入 dtype 中计算，并拒绝非有限输入或结果。除法拒绝正负零除数。Clamp 在收窄到 Float32 前检查所选结果；未被选中的端点可以超出 Float32 范围。Callback 使用 invocation allocator，不执行广播、转换或语义 facet 保留。

`numeric.mean` 按逻辑 row-major 顺序使用 Float64 累加。`numeric.variance` 分两遍执行：第一遍求 mean，第二遍以 `ddof=0` 累加平方偏差。两者输出单个 scalar，并将逻辑 row-major 区间拆为精确分块。块可跨张量轴，但不会读取包围盒间隙。`block_size` 限定输入读取粒度，不合批输出观察。非有限样本和不可表示的累加结果会失败。执行上下文持有 continuation 状态和活跃 fragment；处理分块期间受 allocator 与 work 限额约束。

`numeric.ordered_scan` 从正零 Float64 carry 开始，按从左到右顺序做 inclusive prefix 加法，使用 nearest-even 舍入和渐进下溢。输出 `j` 只读取输入 `[0,j]`。首个非有限输入或累加溢出会报告全局输入下标。成功 carry 可通过完成态 checkpoint 复用；失败不缓存，checkpoint 被逐出后可能重算。

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

分阶段 mean/variance callback 的 continuation 状态和每个活跃来源 fragment 使用 execution context allocator，并受其资源限额约束。已完成 scalar 结果的缓存证据保留完整观测输入支持。若给定输入位、阶段、范围和 incoming accumulator 状态相同，成功的内部 block 也可复用；variance 的第二遍状态包含第一遍 mean。失败 block 不缓存。缓存工作预算耗尽时会跳过可选查询或保留；被驱逐的 block 可重新计算。

Profile-specific 逐元素和归约实现通过执行资源预算计量 work 和取消。主机无法选用指定 profile 时，准备阶段会失败；需要可移植路径时应选择 strict 后缀。Profile 名称不代表 GPU 执行。

## 输入、所有权与错误

Generic numeric Value 的 rank 为 1..8，各轴长度非零；除算子族明确声明语义输入验证外，facets 必须为空。算子按逻辑坐标读取数据，支持合法的 storage offset 和 signed stride，并通过 invocation allocator 分配输出。Callback 完成并通过最终取消检查后才发布输出 Value。

静态参数非法返回 `InvalidArgument`。dtype、shape 或 facet metadata 不支持或不兼容返回 `TypeMismatch`。未带后缀的有限域算术、分阶段归约和 scan 将非有限或不可表示结果报告为 `OperationFailed`。带后缀精确 kernel 按操作保留或分类 IEEE 值；类型化定义域错误可返回 `InvalidArgument/InvalidDomain`。资源耗尽和取消保留各自状态码。算子不返回部分成功的 Value。

## 公开 workflow 与检查

[`test_numeric_operations.cpp`](../../../tests/integration/test_numeric_operations.cpp) 覆盖未带后缀的算术、clamp、mean 和 variance，包括 strided/非对齐输入、shape/type 拒绝、资源失败和取消。[`test_ordered_reduction.cpp`](../../../tests/integration/test_ordered_reduction.cpp) 检查归约顺序、块边界、缓存复用和取消。[公开 G4 workflow](../../../examples/g4_workflow/README.md) 对重复 `[0,1,2,3]` 验证 mean `1.5` 和 population variance `1.25`。

```sh
cmake --build build --target test_numeric_operations test_ordered_reduction -j 8
ctest --test-dir build -R '^(test_numeric_operations|test_ordered_reduction)$' --output-on-failure
```

Generic 算术 key 未声明 planar image capability。注册这些 key 不代表 structural planar image 可以作为 generic numeric array 输入。
