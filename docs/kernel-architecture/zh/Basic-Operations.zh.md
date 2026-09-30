# 基础算子

默认 registry 提供 CPU 曲线、场、标量统计、levels 和逐元素数值算子。这些算子通过公开 workflow、compiler 和 execution API 使用。数值 callback 消费 `Value` 输入；本页的图像端口采用 legacy Value 表示，不代表支持 planar 图像。Operation plugin 使用 C ABI 11 和 planar extension ABI 3。

## 输入与输出契约

| Key | 输入和输出 | 必需参数 |
| --- | --- | --- |
| `curve.sample_linear`、`curve.sample_monotone` | generic Float32/Float64 控制点 `[K,2]` 到 generic `[count]`；`K >= 2`，样本有限且控制点 x 严格递增 | 有限 Float64 `domain_min < domain_max`；String `out_of_domain`（`reject` 或 `clip`）；Int64 `count` 范围 `[2,1048576]` |
| `field.apply_lut_1d` | rank-2 场和同 dtype 的 generic rank-1 表 `[N]`，`N >= 2`；输出保留场 shape；Whole | 有限 Float64 `domain_min < domain_max`；String `out_of_domain`（`reject` 或 `clip`） |
| `field.smoothstep` | Float32/Float64 rank-2 场到 Float32 coverage 场 | 有限 Float64 `edge0 < edge1` |
| `analysis.histogram` | Float32/Float64 rank-2 场到 Int64 `[bins]` | Int64 `bins` 范围 `[1,1048576]`；有限 Float64 `range_min < range_max` |
| `analysis.histogram_out_of_range` | Float32/Float64 rank-2 场到 Int64 `[2]`，顺序为 underflow、overflow | 有限 Float64 `range_min < range_max` |
| `grade.levels` | Float32/Float64 rank-2 场到同 dtype、同 shape | 有限 Float64 `black < white`、`gamma > 0`、`out_min <= out_max` |
| `numeric.minimum`、`numeric.maximum` | 两个 dtype、shape 相同的 Float32/Float64 generic 数组；输出 shape 相同 | 无 |
| `numeric.abs` | 一个 Float32/Float64 generic 数组；输出 shape 相同 | 无 |
| `image.mix` | 两个 RGBA Float32 image Value 和一个 Float32 mask Value；输出保留首图像 schema | 无 |

Registry 不会填入参数默认值。Workflow 作者必须提供全部必需参数。示例值 `count=256`、`domain_min=0`、`domain_max=1`、`edge0=0`、`edge1=1`、`range_min=0`、`range_max=1`、`black=0`、`white=1`、`gamma=1`、`out_min=0` 和 `out_max=1` 均由调用方选择。

场输入是 rank-2 Float32/Float64 Value，可以无 facet、带 ScalarField 语义或带 canonical coverage 语义。Generic 控制点和 LUT 表要求空 facets。数值二元输入的 dtype 与 shape 必须相同。内建 callback 会拒绝非有限样本以及不可表示的中间值或输出，返回 `OperationFailed`。

## 数学语义与区域

曲线采样器在闭区间内均匀放置 `count` 个坐标，包含两端。线性插值经过每个控制点。Monotone 插值使用保持形状的三次 Hermite 曲线，内部斜率采用加权调和平均，端点斜率受限；两个控制点退化为线性段。`reject` 要求采样域处于控制点 x 范围内；`clip` 在范围外返回最近端点。查询坐标重合时报错。

`field.apply_lut_1d` 将闭合输入域映射到表首尾样本，并在表样本之间线性插值。String 策略选择拒绝越域的场值，或将它们裁剪到端点值。表输入是普通 generic 数组，不携带 `lut.apply_1d` 消费的 SampledSignal 契约。

`field.smoothstep` 将归一化边缘坐标裁剪到 `[0,1]`，再计算 `t*t*(3-2*t)`。`grade.levels` 将 `[black,white]` 映射到 `[out_min,out_max]`；`gamma=1` 使用补偿线性插值，其他正 gamma 对裁剪后的坐标计算 `pow(t,1/gamma)`。Levels 保留输入 dtype；smoothstep 建立 canonical Float32 coverage 语义。

Histogram 将 `[range_min,range_max]` 等分为多个区间。每个 bin 左闭右开，最后一个 bin 包含上界。范围外值从 `analysis.histogram` 排除，并由 `analysis.histogram_out_of_range` 计数。实现构造端点精确的 Float64 边界，边界塌缩时报错，使用二分搜索定位，检查 Int64 计数溢出，并扫描完整场。

`numeric.minimum` 的两个操作数都是零时选择负零；`numeric.maximum` 选择正零；`numeric.abs` 将负零转换为正零。这些操作使用 Elementwise Region。曲线和两个 histogram 操作使用 Whole Region；LUT application 读取完整表并处理场输出；levels 和 smoothstep 处理请求的输出区域。`image.mix` Value callback 对每个 RGBA 样本计算 `(1-M)A+MB`，并使用 Elementwise Region，但没有声明 planar storage capability。

## 执行与错误

Callback 通过 invocation allocator 分配输出和 scratch，遍历时检查取消，恢复调用方浮点环境，并在失败时释放未发布的分配。PCHIP 为每个控制点保留三个 Float64 workspace 数组。Whole 操作要求其声明的完整输入能够满足执行资源预算。

非法静态参数返回 `InvalidArgument`。dtype、shape、facets 或图像 metadata 不兼容返回 `TypeMismatch`。非有限样本、数值坐标塌缩、计数溢出或不可表示的结果返回 `OperationFailed`。取消和资源耗尽保留各自状态码。失败 callback 不发布部分 Value。

Legacy `image.mix` 注册项的 callback 端口 schema 描述 RGBA `Value` 输入，但没有 `PlanarImageLayout` 的 workflow declaration 若带 `photospider.image` facet 会在输入校验时被拒绝。其 `OperationTraits::planar_storage_capable` 也保持 false，因此 compiler 会拒绝 structural planar image 输入。注册项本身不能证明存在可执行 workflow 路径。

## 公开 workflow 与检查

[`examples/foundations_workflow`](../../../examples/foundations_workflow/README.zh.md) 提供维护中的 generic numeric 和 expression/LUT 示例。[`test_basic_operations.cpp`](../../../tests/integration/test_basic_operations.cpp) 覆盖曲线、场、histogram、区域输出、视图、参数与数值错误、取消、资源限制和浮点环境恢复。它不测试 `image.mix`。

```sh
cmake --build build --target test_basic_operations photospider_foundations_workflow -j 8
ctest --test-dir build -R '^(test_basic_operations|test_workflow_numeric_reductions|test_workflow_expression_lut)$' --output-on-failure
```
