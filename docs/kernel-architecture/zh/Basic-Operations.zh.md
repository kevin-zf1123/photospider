# 基础算子

## 输入与输出契约

| Key | 输入和输出 | 必需参数 |
| --- | --- | --- |
| `curve.sample_linear`、`curve.sample_monotone` | 一个 Result 中的 generic Float32/Float64 控制点 `[K,2]`，输出 `photospider.tensor` v1/member `samples` `[count]`；无 batch，`K >= 2`，控制点有限且 x 严格递增；Whole | 有限 Float64 `domain_min < domain_max`；String `out_of_domain`（`reject` 或 `clip`）；Int64 `count` 范围 `[2,1048576]` |
| `field.apply_lut_1d` | Field Result 与同 dtype generic Table Result `[N]`，`2 <= N <= 2^53`；输出为保留 field sample shape 的 `photospider.tensor` v1/member `samples`；Whole | 有限 Float64 `domain_min < domain_max`；String `out_of_domain`（`reject` 或 `clip`） |
| `field.smoothstep` | Field Result 到 canonical Float32 `photospider.image` v1/member `pixels`，sample shape `[1,1,H,W]`；Dependency | 有限 Float64 `edge0 < edge1` |
| `numeric.minimum`、`numeric.maximum` | 两个 dtype 相同、`sample_shape()` 相同的 Float32/Float64 tensor Result；输出 port `value` 为带 `samples` member 的 `photospider.tensor` v1 Result | 无 |
| `numeric.abs` | 一个 Float32/Float64 tensor Result；输出 port `value` 使用相同 generic tensor schema 和 shape | 无 |

上表七个操作均接受恰含一个 tensor 且无 fields 的 Result；schema id、version 和 tensor key 可为任意结构有效值。输入使用 Float32/64。曲线控制点未批处理；Field 的 cell shape 为二维，`batch_axes` 只能为空或 `[1,1]`。Table 未批处理，并与 field dtype 相同。所有操作的输出端口均为 `value`。除 smoothstep 外，输出使用 generic `photospider.tensor` v1/member `samples`；smoothstep 恢复 image coverage Result schema。

## 数学语义与区域

曲线采样器在闭区间内均匀放置 `count` 个坐标，包含两端。线性插值经过每个控制点。Monotone 插值使用保持形状的三次 Hermite 曲线，内部斜率采用加权调和平均，端点斜率受限；两个控制点退化为线性段。`reject` 要求采样域处于控制点 x 范围内；`clip` 在范围外返回最近端点。查询坐标重合时报错。

`field.apply_lut_1d` 将闭合输入域映射到表首尾样本，并在表样本之间线性插值。String 策略选择拒绝越域的场值，或将它们裁剪到端点值。表输入是普通 generic tensor member。

## 执行与错误

这些 Result program 从授权 tensor windows 读取输入，并经 Result builder 发布。它们检查取消、恢复调用方浮点环境，并在失败时丢弃未发布的输出。PCHIP 为每个控制点保留三个 Float64 workspace 数组。Whole 操作要求其声明的完整输入能够满足执行资源预算。

非法静态参数返回 `InvalidArgument`。dtype、shape 或图像结构不兼容返回 `TypeMismatch`。非有限算术样本、数值坐标塌缩、计数溢出或不可表示的结果返回 `OperationFailed`。取消和资源耗尽保留各自状态码。这些 Result 操作仅在 Result transaction 成功后发布。

图像 Result 契约是结构 schema `photospider.image` v1，包含单个 `pixels` tensor 和 tensor spec 声明的 layout。通道是 RGBA 还是 coverage 属于各算子的显式契约，而不是数据上的标签；要求 premultiplied RGBA 的算子在该端口绑定算子 SDK 的 RGBA 校验器。
