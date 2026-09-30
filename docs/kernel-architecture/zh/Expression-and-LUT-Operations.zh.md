# 表达式与一维 LUT 算子

默认 registry 提供两类表达式采样器：未带后缀的 `numeric.sample_expression` Value 算子，以及带 profile 后缀的 `numeric.sample_expression_<profile>` 算子族。`lut.apply_1d` 消费 SampledSignal 查询和类型化一维表。接受的组合边界见 [ADR 0020](../../adr/0020-composable-operation-foundations.md)；本页说明当前注册的 key。[英文镜像](../Expression-and-LUT-Operations.md) 为权威说明。

## 未带后缀的表达式采样器

`numeric.sample_expression` 接收一个无 facet 的 generic Float64 系数向量 `[K]`，其中 `1 <= K <= 256`。必需静态参数为 String `expression`、范围 `[1,1048576]` 的 Int64 `count`、Float64 `start` 和正 Float64 `step`。输出为带 SampledSignal metadata 的 Float32 `[count]`；样本轴采用给定 start 和 step，值通道名称与 role 均为 `value`。构造端不会自动提供默认值。

表达式语法允许十进制或科学计数法数字、`x`、`c[index]`、括号、一元 `+`、`-`，二元 `+ - * / ^`，一元 `abs`、`sqrt`、`exp`、`log`、`sin`、`cos`，二元 `min`、`max`。幂运算右结合且优先级高于一元负号：`-2^2` 为 `-4`，`2^-2` 为 `0.25`，`2^3^2` 为 `512`；`0^0` 为 `1`。任意标识符、十六进制字面量、NaN/Infinity 名称、错误的系数下标和函数调用都会被拒绝。源码最多 4096 字节，AST 最多 256 个节点且高度不超过 32；括号不增加 AST 节点。

未带后缀算子要求 `start` 有限、`step` 有限且为正。`count > 1` 时，`fma(count-1,step,start)` 必须有限且大于 `start`。采样器按 `fma(i,step,start)` 计算坐标，并在受控 Float64 nearest-even、gradual-underflow 环境内求值。所有系数都会校验，包括表达式未使用的项。每个子表达式都必须有限；除零和函数定义域错误会失败，最终结果须能表示为有限 Float32。输出和 4096 字节求值 workspace 使用 invocation allocator。求值过程中及发布前检查取消；失败时释放未发布分配并恢复调用方浮点环境。

## 带 profile 的具名系数采样器

`numeric.sample_expression_strict`、`numeric.sample_expression_accelerated_apple_silicon` 和 `numeric.sample_expression_accelerated_x86_64` 接收动态标量 `[1]` 输入。输入 0 是 `start`，输入 1 是 `end`，之后每个输入对应必需 String `coefficient_names` 中的一个名称。辅助函数 `ps::numeric::sample_expression_node` 根据表达式和具名系数输入映射构造这种节点。

必需静态参数为 String `expression`、String `coefficient_names`、范围 `[1,1048576]` 的 Int64 `count` 和 String `dtype`（`float32` 或 `float64`）。`values` 输出为所选 dtype 的 generic `[count]`；`axis` 输出为 Float64 `[3]`，count 大于 1 时依次存储 start、end 和计算得到的 step；count 为 1 时输出 `[start,start,0]`。count 为 1 的 values 请求不读取 end，但仍读取表达式所需的系数。count 大于 1 的 values 请求要求端点有限且不相等、相邻采样坐标可表示；实际求值的 coefficients 必须有限。两个输出均为 Whole，并通过 invocation allocator 物化。只请求 axis 时跳过系数求值；空请求跳过 payload 处理。

该算子族有独立的 parser 与函数集合。它允许十进制/科学计数法字面量、`x`、`pi`、`e`、具名系数标识符、括号、一元 `+ -`、二元 `+ - * / ^`，一元 `abs`、`sqrt`、`exp`、`ln`、`sin`、`cos`、`tan`，以及二元 `min`、`max`。未带后缀 Value key 使用函数名 `log`；profile key 使用 `ln`，并额外支持 `tan`、`pi` 和 `e`。后缀选择 CPU numeric profile。主机不支持指定 profile 时，准备阶段失败。Strict 按从左到右的 Float64 表达式顺序求值，再一次性舍入到输出 dtype。Accelerated profile 将候选结果按固定 strict 结果进行认证，对不确定样本回退到 strict evaluator。Whole 和 Region 请求使用相同 profile 结果。这些 key 有两个命名输出和动态端点绑定；输入形式不同于未带后缀采样器的系数向量，也不同于其静态 start/step 参数。

公开 workflow helper 定义于 [`numeric/expression.hpp`](../../../include/photospider/numeric/expression.hpp)。它生成带 `values` 和 `axis` 输出的普通 `WorkflowNode`。Helper 按字节序排序 free coefficient 名称，并在追加节点前拒绝缺失、未使用或重复名称。

## 线性 LUT application

`lut.apply_1d` 的输入 0 是 Float32 SampledSignal 查询，输入 1 是 Float32 rank-1 表。表必须带单通道 SampledSignal 或 Lut domain，并至少含两个样本。查询的 sample-value unit 必须与表采样轴 unit 相同；查询轴 unit 与表值 unit 相互独立。必需 String 参数 `out_of_domain` 可设为 `reject` 或 `clip`。结果保留查询 shape，dtype 为 Float32，facets 为空。

表 domain 要求 origin 有限、sample step 有限且为正，并且 `fma(N-1,step,origin)` 得到有限递增的端点。端点查询精确返回表端点，定义域内部使用带补偿的 Float64 插值。插值 index 大于等于 `2^53`、算术不可表示，或丢弃的补偿量大于 Float32 ULP 的八分之一时，操作会在写入输出前拒绝。`reject` 会拒绝越域查询；`clip` 返回最近表端点。支持有符号和 HDR 表值；该操作不裁剪颜色 gamut，也不会在结果中建立表值语义 metadata。

## 错误与校验

表达式语法错误、静态参数非法和系数名映射非法返回 `InvalidArgument`。类型、shape 或语义不匹配返回 `TypeMismatch`。非有限系数、数学未定义、坐标塌缩、插值精度不足或输出溢出返回 `OperationFailed`，适用时附样本下标。取消和资源耗尽保留各自状态码。宿主在 callback 前校验闭合语义契约，也校验 C operation 声明兼容性。

## 公开 workflow 与验证

[`test_expression_operations.cpp`](../../../tests/integration/test_expression_operations.cpp) 执行未带后缀的表达式和 LUT fixture、分配/取消场景及 C expression contract fixture。表达式案例覆盖语法与数值边界；LUT 案例覆盖端点、单位匹配和接近精度边界的插值。

[`examples/foundations_workflow`](../../../examples/foundations_workflow/README.zh.md) 提供维护中的 `numeric` 和 `expression-lut` 场景。具名系数 authoring API 和 profile 示例位于 [`examples/numeric_workflow`](../../../examples/numeric_workflow/README.md)。直接绑定的结果缓存准入见[宿主缓存模型](Cache-Model.zh.md)。

```sh
cmake --build build --target test_expression_operations photospider_foundations_workflow -j 8
ctest --test-dir build -R '^(test_expression_operations|test_workflow_expression_lut)$' --output-on-failure
```
