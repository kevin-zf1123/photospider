# 数值算子

`make_default_operation_registry()` 的默认 registry 通过公开 WorkflowDocument、
Compiler、ExecutionContext 提供以下 CPU 算子。接受边界见
[ADR 0020](../../adr/0020-composable-operation-foundations.md)，
[英文实现说明](../Numeric-Operations.md) 为权威来源。

Clamp 和算术使用 Whole 输入/输出需求；归约通过有界顺序阶段读取完整
逻辑输入支持。全部输出 packed generic Value，facets 为空。Shape
仍要求 rank 1..8 且各轴非零。Clamp 和算术保留 shape，归约输出
Float64 `{1}`。二元输入 dtype 和 shape 必须相同。无隐式广播、转换或语义保留：
这些说明适用于 generic 数组及允许的非图像输入。旧 coverage/image Value 被 planar
门禁拒绝，不能通过乘二将旧 coverage mask 适配为 generic 数组。

| Key | 输入 | 静态参数 |
| --- | --- | --- |
| `numeric.add`、`numeric.subtract`、`numeric.multiply`、`numeric.divide` | 两个 Float32 或两个 Float64 数组 | 无 |
| `numeric.clamp` | 一个 Float32/Float64 数组 | finite inclusive Float64 `min`、`max`，`min <= max` |
| `numeric.mean`、`numeric.variance` | 一个 Float32/Float64 数组 | 可选 Int64 `block_size`，范围 [1,65536]、默认 64 |
| `numeric.ordered_scan` | 一个 rank-1 Float64 数组；同 shape generic 输出 | 可选 Int64 `block_size`，范围 [1,65536]、默认 64 |

包 0.20.0 移除 `numeric.cast` 与 `numeric.encode_range`。数值格式转换归属
Proposed 的 FMT-06，见[退休记录](../../built-in_ops/02-format-color/op_specs/FMT_legacy_retirement.md)。

算术使用输入 dtype 计算，拒绝非有限输入/结果和正负零除数。Clamp 在 Float32 收窄前
检查实际选中结果，允许未被选中的巨大 Float64 端点。Mean 按固定逻辑 row-major 顺序
使用 Float64 累加；population variance 使用两遍计算（mean，然后平方偏差，`ddof=0`）。
非有限累加结果失败，不隐式并行或重排归约。

实现根据 storage origin、byte offset 和 signed strides 读取逻辑坐标，包括非对齐和
zero-stride 视图。输出通过调用分配器计量，IR 发布前检查 dense 输出可表示性，遍历中
及发布前检查取消，错误时释放未发布缓冲区。非法参数为 `InvalidArgument`；不支持/
不匹配 dtype 或 shape 在 callback 前返回 `TypeMismatch`；非法数值为
`OperationFailed`，适用时附样本下标。资源不足和取消保留各自错误码，不发布部分成功值。

## 公开 workflow 与验证

[test_numeric_operations.cpp](../../../tests/integration/test_numeric_operations.cpp)
构造公开 producer-to-operation workflow，检查算术、clamp、mean/variance、strided
和非对齐输入、类型与形状拒绝、资源失败及取消。期望结果包括
`[3,2,1]-[4,4,4]=[-1,-2,-3]`、`[1,2,3]` 的 mean=2、population variance=2/3。

```sh
cmake --build build --target test_numeric_operations -j 8
ctest --test-dir build -R '^test_numeric_operations$' --output-on-failure
```

[Foundations 示例](../../../examples/foundations_workflow/README.zh.md)提供维护中的
通用数值、expression/LUT 和 field-filter workflow。
[退休回归](../../../tests/integration/test_format_color_retirement.cpp)检查旧格式 key
无法编译，并提供结果为 0.5 的最小绑定 `numeric.add_strict` 图。新规格尚未成为运行时接口。

Mean/variance 使用分阶段依赖协议，输出仍为单个 scalar 观察。可选 Int64 参数
`block_size` 范围 [1,65536]，默认 64，控制每阶段请求样本数。精确 row-major
区间跨 rank-1..8 轴分解，不读取 bounding-box 的间隙。
各块直接继续 incoming Float64 累加器，保持非有限输入、sum overflow、variance
溢出检查的顺序和全局 sample index。Variance 第一遍完成后保留精确 mean，再用于
全部第二遍块。块大小表示输入读取粒度，不表示多个输出观察的合批。

允许的非图像语义输入在算术前通过供给 fragment validator 校验。旧图像被拒绝
不代表已支持 planar 归约。Opaque vendor facets 不增加验证扫描。
State 和活跃 fragment 使用当前 ExecutionContext worker/admission/allocator。精确 Empty
scalar 查询不读样本，资源、发现和取消限额保持显式。源数据可以超过 live payload
预算，只需正在读取的块能够容纳。已完成精确 demand 的缓存命中保留完整全局源支持；
任何被观察输入变化均使 scalar 结果失效。已完成内部 transition 也可复用下述块缓存。

`test_ordered_reduction` 检查五种块大小、rank 1/4/8、Float32/Float64、冷热缓存的位级
结果，原错误 sample index、第二遍取消与恢复、旧图像拒绝，以及 1 KiB
受控预算下的 32 KiB 源。独立算术 oracle 显式执行 binary64 left fold。
[G4 公开 workflow](../../../examples/g4_workflow/README.md) 用有界读取验证重复
`[0,1,2,3]` 的 mean=1.5、variance=1.25。


`numeric.ordered_scan` 使用正零 Float64 初始 carry、严格从左到右加法、nearest-even
舍入和渐进下溢，计算 inclusive prefix，随后恢复调用方环境。输出 j 观察输入 `[0,j]`，
不越过 j 读取或计算。首个非有限输入或累加器分别报 `nonfinite scan input i` 或
`scan overflow i`。因此 `[1,inf]` 查询 `{0}` 成功得到 1，查询 `{1}` 或 `{0,1}` 在
输入 1 失败。RequestFailureOnly 仍逐输出独立调用。

同一活跃输入 bundle 可以通过只保存完成状态的 checkpoint 复用成功前缀 carry。
借用时导入完整直接输入证据和上游结构记录，不缓存失败、不等待 worker。Checkpoint
使用现有宿主 allocator lease 和有界可选元数据保留，驱逐后可以重算。256 个密集输出
的源测试恰好读取 256 个输入一次。私有 execution hook 测试暂停真实成功前缀发布，
检查两种 waiter 启动顺序、发布者取消、暖结果缓存和精确源支持导入。Direct/manual
协议测试检查 allocator 归属、scope/sequence 拒绝及证据限额。Scan 与 mean/variance 现通过既有结果 LRU 跨 bundle 保留已完成内部 transition。
Key 包含精确供给集合/输入 bits、实际 incoming state bits、phase、range 及固定
nearest-even/渐进下溢模式。Variance 第二遍每个 incoming state 包含固定 mean。
宿主在普通 fragment 验证后哈希；命中将 state 复制到当前阶段 allocator，保留当前
依赖证据。仅保存成功 transition，不额外读取输入、不合批输出观察。Incoming 改变时
当前块重算；后续 incoming 重汇合且输入相同时才可命中。`block_cache_hits/misses`
单独计数内部查找；可选 cache work 耗尽时跳过查找/保留。公开 workflow 与测试验证
`[1,2^54]` 重汇合边界、frozen/current 输出差异及 mean 改变使第二遍失效。
已完成输出缓存仍核验完整传递前缀支持。
