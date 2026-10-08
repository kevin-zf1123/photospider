# 基础算子

默认 registry 提供 CPU 曲线、场、标量统计、levels、逐元素数值和图像算子，通过公开 workflow、compiler 和 execution API 使用。本页列出的所有算子均使用 Result 输入和输出；输入 schema 与 Region 契约按 key 分别定义。Foundations workflow 也展示了通过 Result 使用未带后缀表达式与 LUT 算子。Result operation C ABI 2 是唯一的 operation plugin C 表；见 [Plugin ABI](Plugin-ABI.zh.md)。

## 输入与输出契约

| Key | 输入和输出 | 必需参数 |
| --- | --- | --- |
| `curve.sample_linear`、`curve.sample_monotone` | 一个 Result 中的 generic Float32/Float64 控制点 `[K,2]`，输出 `photospider.tensor` v1/member `samples` `[count]`；无 batch，`K >= 2`，控制点有限且 x 严格递增；Whole | 有限 Float64 `domain_min < domain_max`；String `out_of_domain`（`reject` 或 `clip`）；Int64 `count` 范围 `[2,1048576]` |
| `field.apply_lut_1d` | Field Result 与同 dtype generic Table Result `[N]`，`2 <= N <= 2^53`；输出为保留 field sample shape 的 `photospider.tensor` v1/member `samples`；Whole | 有限 Float64 `domain_min < domain_max`；String `out_of_domain`（`reject` 或 `clip`） |
| `field.smoothstep` | Field Result 到 canonical Float32 `photospider.image` v1/member `pixels`，sample shape `[1,1,H,W]`；Dependency | 有限 Float64 `edge0 < edge1` |
| `analysis.histogram` | Float32/Float64 Field Result 到 generic Int64 `photospider.tensor` v1/member `samples` `[bins]`；Whole | Int64 `bins` 范围 `[1,1048576]`；有限 Float64 `range_min < range_max` |
| `analysis.histogram_out_of_range` | Float32/Float64 Field Result 到 generic Int64 `[2]`，顺序为 underflow、overflow；Whole | 有限 Float64 `range_min < range_max` |
| `grade.levels` | Float32/Float64 Field Result 到保留完整 sample shape 和 dtype 的 generic `photospider.tensor` v1/member `samples`；Dependency | 有限 Float64 `black < white`、`gamma > 0`、`out_min <= out_max` |
| `core.delay` | 一个 tensor Result；非空请求保持 Result schema 并输出完整 coverage，Empty 不返回 tensor coverage；Whole | 必填 Int64 `milliseconds`，范围 `[0,5000]`；禁用可选本地缓存保留 |
| `core.gpu_fallback_probe` | 一个 tensor Result，按请求 Footprint 输出 identity Result；Dependency execution | 无参数；用于检查启动阶段 CPU fallback |
| `numeric.minimum`、`numeric.maximum` | 两个 dtype 相同、`sample_shape()` 相同的 Float32/Float64 tensor Result；输出 port `value` 为带 `samples` member 的 `photospider.tensor` v1 Result | 无 |
| `numeric.abs` | 一个 Float32/Float64 tensor Result；输出 port `value` 使用相同 generic tensor schema 和 shape | 无 |
| `image.mix` | 两个 RGBA image Result 和一个 coverage image Result；输出 port `value` 保留首图像 Result schema | 无 |

Registry 不会填入参数默认值。Workflow 作者必须提供全部必需参数。示例值 `count=256`、`domain_min=0`、`domain_max=1`、`edge0=0`、`edge1=1`、`range_min=0`、`range_max=1`、`black=0`、`white=1`、`gamma=1`、`out_min=0` 和 `out_max=1` 均由调用方选择。

上表七个操作均接受恰含一个 tensor 且无 fields 的 Result；schema id、version 和 tensor key 可为任意结构有效值。输入使用 Float32/64。曲线控制点未批处理且 facets 为空；Field 的 cell shape 为二维，`batch_axes` 只能为空或 `[1,1]`，facets 只能为空、ScalarField 或 canonical coverage。Table 未批处理且 facets 为空，并与 field dtype 相同。所有操作的输出端口均为 `value`。除 smoothstep 外，输出使用 generic `photospider.tensor` v1/member `samples`，不复制输入 facets；smoothstep 恢复 image coverage Result schema。

曲线采样、LUT 和两个 histogram 使用 Whole execution。非空请求以 Data、Validation 和 Descriptor roles 读取完整输入；Empty 不请求样本 payload。Levels 和 smoothstep 使用分阶段 Dependency execution：Data 只读取 Q，Validation 覆盖 `close_samples(Q)` 的 atomic sample groups。闭合等于 Q 时可合并两个 roles，否则先 Validation Need，再 Data Need；relation 保留独立 support。七个算子都经 Result transaction 发布，保留实际 source association，并在授权 Root 窗口内读取，按 work 预算检查取消。

`image.mix` 接受 `photospider.image` v1 Result，其单个 `pixels` tensor 分别为两个 canonical Float32 HWC RGBA 图像与一个 canonical Float32 HW coverage mask。三个输入的 frame、layer、height、width extents 必须相同。输出使用首图像 schema。`image_program.cpp` 中的图像操作读取和发布 Result tensor windows；`PlanarImage` 可以作为 Result 内部的物理 backing。

`core.delay` 接受结构有效、恰含一个 tensor 且没有 fields 的 Result，保留其 schema 和 metadata；只有新启动的非空 continuation 才等待请求的毫秒数。它每毫秒检查取消并消耗一个 work unit；Empty 输出不请求输入 payload。`cacheable=false` 仅禁用可选本地缓存保留；相同 frozen snapshot、operation contract 和 Q 的请求仍可在外部 owner 保持完成的 shared Result 存活时加入或复用它。因此不能保证每次 execution 都重新等待。`core.gpu_fallback_probe` 对选定 footprint 执行 identity 映射并允许 CPU fallback。它是 execution probe：GPU `start_result` 返回 `BackendUnavailable`，让 coordinator 在任何 Need 或 native dispatch 前测试 CPU retry。

## 数学语义与区域

曲线采样器在闭区间内均匀放置 `count` 个坐标，包含两端。线性插值经过每个控制点。Monotone 插值使用保持形状的三次 Hermite 曲线，内部斜率采用加权调和平均，端点斜率受限；两个控制点退化为线性段。`reject` 要求采样域处于控制点 x 范围内；`clip` 在范围外返回最近端点。查询坐标重合时报错。

`field.apply_lut_1d` 将闭合输入域映射到表首尾样本，并在表样本之间线性插值。String 策略选择拒绝越域的场值，或将它们裁剪到端点值。表输入是普通 generic tensor member，不携带 `lut.apply_1d` 消费的 SampledSignal 契约。

`field.smoothstep` 将归一化边缘坐标裁剪到 `[0,1]`，再计算 `t*t*(3-2*t)`。`grade.levels` 将 `[black,white]` 映射到 `[out_min,out_max]`；`gamma=1` 使用补偿线性插值，其他正 gamma 对裁剪后的坐标计算 `pow(t,1/gamma)`。Levels 保留输入 dtype；smoothstep 建立 canonical Float32 coverage 语义。

Histogram 将 `[range_min,range_max]` 等分为多个区间。每个 bin 左闭右开，最后一个 bin 包含上界。范围外值从 `analysis.histogram` 排除，并由 `analysis.histogram_out_of_range` 计数。实现构造端点精确的 Float64 边界，边界塌缩时报错，使用二分搜索定位，检查 Int64 计数溢出，并扫描完整场。

`numeric.minimum` 的两个操作数都是零时选择负零；`numeric.maximum` 选择正零；`numeric.abs` 将负零转换为正零。这些 CPU 算子使用分阶段 Dependency execution：Data 只在所选输出 Footprint Q 上读取；Validation 覆盖 typed tuple 和 atomic sample group 闭合后的 `close_samples(Q)`。闭合需要时会读取并检查 Q 外的 tuple samples，但不会请求 `close_samples(Q)` 以外的样本。若闭合等于 Q，算子会将 Data/Validation roles（1 和 4）合并到一个 Need；否则先发 role-4 Validation Need，再发 role-1 Data Need。输出 relation 将 Data、Validation 和 Descriptor support 分别记录。`image.mix` 的图像程序按请求的图像区域对每个 RGBA 通道执行线性 blend。曲线和两个 histogram 操作使用 Whole Region；LUT application 读取完整表并处理场输出；levels 和 smoothstep 处理请求的输出区域。

## 执行与错误

这些 Result program 从授权 tensor windows 读取输入，并经 Result builder 发布。它们检查取消、恢复调用方浮点环境，并在失败时丢弃未发布的输出。PCHIP 为每个控制点保留三个 Float64 workspace 数组。Whole 操作要求其声明的完整输入能够满足执行资源预算。

非法静态参数返回 `InvalidArgument`。dtype、shape、facets 或图像 metadata 不兼容返回 `TypeMismatch`。非有限算术样本、数值坐标塌缩、计数溢出或不可表示的结果返回 `OperationFailed`。Typed Result source validation 遵循其输入错误契约，可返回 `ErrorCode::InvalidArgument` 和 `FailureReason::InvalidDomain`，并保留来源 `input_id`。取消和资源耗尽保留各自状态码。这些 Result 操作仅在 Result transaction 成功后发布。

图像 Result 契约使用 `photospider.image` v1、单个 `pixels` tensor、canonical RGBA 或 coverage semantics，以及 tensor spec 声明的 layout。它不使用 legacy `Value` planar capability flag。

## 公开 workflow 与检查

[`examples/numeric_workflow/README.md#basic-curves-fields-and-analysis-results`](../../../examples/numeric_workflow/README.md#basic-curves-fields-and-analysis-results) 展示 Result 曲线、field、levels、histogram 和 smoothstep-to-mask workflow。[`examples/foundations_workflow`](../../../examples/foundations_workflow/README.zh.md) 展示 Result numeric、expression/LUT 和 generator-gain workflow。[`test_basic_operations.cpp`](../../../tests/integration/test_basic_operations.cpp) 覆盖这些基础 Result 契约和其余 field/image 行为；[`test_numeric_result_math.cpp`](../../../tests/integration/test_numeric_result_math.cpp) 覆盖有限逐元素 Result key；[`test_builtin_result_images.cpp`](../../../tests/integration/test_builtin_result_images.cpp) 覆盖 `image.mix` 等 Result 图像程序。

```sh
cmake --build build/kernel-dev --target test_basic_operations photospider_numeric_basic -j8
ctest --test-dir build/kernel-dev -R '^test_basic_operations$' --output-on-failure
build/kernel-dev/examples/numeric_workflow/photospider_numeric_basic
```
