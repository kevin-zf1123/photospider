# 数值算子

`make_default_operation_registry()` 的默认 registry 通过公开 WorkflowDocument、
Compiler、ExecutionContext 提供以下 CPU 算子。接受边界见
[ADR 0020](../../adr/0020-composable-operation-foundations.md)，
[英文实现说明](../Numeric-Operations.md) 为权威来源。

Cast、range、clamp 和算术使用 Whole 输入/输出需求；归约通过有界顺序阶段读取完整
逻辑输入支持。全部输出 packed generic Value，facets 为空。Shape
仍要求 rank 1..8 且各轴非零。Cast、range、clamp 和算术保留 shape，归约输出
Float64 `{1}`。二元输入 dtype 和 shape 必须相同。无隐式广播、转换或语义保留：
coverage mask 乘二得到 generic 数组，之后可由其他算子显式赋予解释。

| Key | 输入 | 静态参数 |
| --- | --- | --- |
| `numeric.cast` | 一个 UInt8/Int64/Float32/Float64 数组 | String `dtype`：`uint8`、`int64`、`float32`、`float64`；String `rounding`：`ties_even`；String `overflow`：`reject` 或 `clip` |
| `numeric.encode_range` | 现有四 dtype 中任一数组 | Cast 参数以及 finite Float64 `src_min`、`src_max`、`dst_min`、`dst_max`；两个区间均严格递增 |
| `numeric.add`、`numeric.subtract`、`numeric.multiply`、`numeric.divide` | 两个 Float32 或两个 Float64 数组 | 无 |
| `numeric.clamp` | 一个 Float32/Float64 数组 | finite inclusive Float64 `min`、`max`，`min <= max` |
| `numeric.mean`、`numeric.variance` | 一个 Float32/Float64 数组 | 可选 Int64 `block_size`，范围 [1,65536]、默认 64 |

默认行为由构造端显式写入 `rounding="ties_even"`、`overflow="reject"`；registry
不补参数。Dither 固定关闭，无 dither 参数。Range 对声明区间进行仿射映射，源区间外
的值按相同公式外推。Clip 只显式作用于目标 dtype 极限，不代表夹到目标区间。

Cast 不缩放。同 dtype cast 保持样本位模式，包括 signed zero 与支持的 NaN payload。
整数源直接转换到目标 dtype，不经 binary64 中转。浮点到整数先 ties-even 舍入，再在
转换前检查边界；Int64 的正 binary64 边界 `2^63` 为 exclusive。有限收窄溢出默认拒绝，
显式 clip 夹到目标有限边界。浮点 cast 接受支持的 NaN/infinity；整数 cast 与 range
在两种 overflow 策略下都拒绝非有限输入。

Range 使用 nearest-even 环境下的 Float64 仿射运算（就近端点锚定、无误差高低位差及补偿乘加）和显式 `fma`；区间差超出
Float64 时缩放宽度计算。无法表示的零/无限系数或超过输出 Float64 一个 ULP 的未解决商/累加误差失败；相同区间保留原源值后进行受检查
转换。有限源值的最终仿射结果溢出时遵循 reject/clip，包含浮点输出。不支持系数无法
表示的极端映射。完成后恢复调用方浮点环境。

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
的 `run()` 是最小公开 producer → operation workflow：注册不可变输入 producer
（含合法 strided Value），提交 `WorkflowNodeOutput` 引用，调用 `Compiler::compile`
并执行 plan。也可使用普通 WorkflowDocument 输入声明及每次运行绑定，完整 scalar cast
代码见[英文示例](../Numeric-Operations.md#public-workflow-and-validation)：将 bound
Float64 `-2.5` 转为 generic Float32 `{1}`，样本仍为 `-2.5`。

运行维护中的可执行入口（可替换为已有配置目录，静态/共享内核使用同一源码）：

```sh
cmake --build build/issue257-static --target test_numeric_operations -j 8
ctest --test-dir build/issue257-static -R '^test_numeric_operations$' --output-on-failure
```

退出零验证全部 256 个 UInt8 值经过每种 dtype 和 range 往返；正负 halfway、Int64
边界；`Int64→Float32` midpoint bits `0x5e800001`；下降减法
`[3,2,1]-[4,4,4]=[-1,-2,-3]`；`[1,2,3]` mean 为 2、variance 为 `2/3`；
typed mask 语义移除；非有限、溢出、shape/dtype、分配与取消错误。极端对称区间压缩
独立检查 `±2→±1`；`2^53` 以上的相邻区间检查 `2` 和 `1/3`，零原点极端区间保持
小正数。测试比较精确字节或指定 Float64 容差，不以实现自身重复求值作 oracle。

修改 `run()` 的 key/参数，或将前一结果节点直接连接到后一节点。单个 generic Float32
结果连接 `image.exposure_gain` 时，保持 `{1}` 和 gain `[0,16]`；bounded consumer
每次消费前检查结果。[通道/颜色算子](Channel-and-Color-Operations.zh.md) 提供显式组合。
[Expression/LUT](Expression-and-LUT-Operations.zh.md) 已提供动态系数采样；
[独立安装 foundations 示例](../../../examples/foundations_workflow)已提供公开组合运行。

Mean/variance 使用分阶段依赖协议，输出仍为单个 scalar 观察。可选 Int64 参数
`block_size` 范围 [1,65536]，默认 64，控制每阶段请求样本数；image 输入向上取整到
完整像素。精确 row-major 区间跨 rank-1..8 轴分解，不读取 bounding-box 的间隙。
各块直接继续 incoming Float64 累加器，保持非有限输入、sum overflow、variance
溢出检查的顺序和全局 sample index。Variance 第一遍完成后保留精确 mean，再用于
全部第二遍块。块大小表示输入读取粒度，不表示多个输出观察的合批。

Typed 输入在算术前完成全域验证阶段，沿用供给 fragment validator，image 保持完整 C。
Opaque vendor facets 不增加验证扫描。
State 和活跃 fragment 使用当前 ExecutionContext worker/admission/allocator。精确 Empty
scalar 查询不读样本，资源、发现和取消限额保持显式。源数据可以超过 live payload
预算，只需正在读取的块能够容纳。已完成精确 demand 的缓存命中保留完整全局源支持；
任何被观察输入变化均使 scalar 结果失效。内部 scan/carry 块共享仍为本轮 G4 的后续工作。

`test_ordered_reduction` 检查五种块大小、rank 1/4/8、Float32/Float64、冷热缓存的位级
结果，原错误 sample index、第二遍取消与恢复、typed channel closure，以及 1 KiB
受控预算下的 32 KiB 源。独立算术 oracle 显式执行 binary64 left fold。
[G4 公开 workflow](../../../examples/g4_workflow/README.md) 用有界读取验证重复
`[0,1,2,3]` 的 mean=1.5、variance=1.25。
