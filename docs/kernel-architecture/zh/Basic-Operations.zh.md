# 基础算子

## 输入与输出契约

| Key | 输入和输出 | 必需参数 |
| --- | --- | --- |
| `curve.sample_linear`、`curve.sample_monotone` | 一个 Result 中的 generic Float32/Float64 控制点 `[K,2]`，输出 `photospider.tensor` v1/member `samples` `[count]`；无 batch，`K >= 2`，控制点有限且 x 严格递增 | 有限 Float64 `domain_min < domain_max`；String `out_of_domain`（`reject` 或 `clip`）；Int64 `count` 范围 `[2,1048576]` |
| `field.apply_lut_1d` | Field Result 与同 dtype generic Table Result `[N]`，`2 <= N <= 2^53`；输出为保留 field sample shape 的 `photospider.tensor` v1/member `samples` | 有限 Float64 `domain_min < domain_max`；String `out_of_domain`（`reject` 或 `clip`） |
| `field.smoothstep` | Field Result 到 canonical Float32 `photospider.image` v1/member `pixels`，sample shape `[1,1,H,W]` | 有限 Float64 `edge0 < edge1` |
| `numeric.minimum`、`numeric.maximum` | 两个 dtype 相同、`sample_shape()` 相同的 Float32/Float64 tensor Result；输出 port `value` 为带 `samples` member 的 `photospider.tensor` v1 Result | 无 |
| `numeric.abs` | 一个 Float32/Float64 tensor Result；输出 port `value` 使用相同 generic tensor schema 和 shape | 无 |

上表七个操作均接受恰含一个 tensor 的 Result；schema id、version 和 tensor key 可为任意结构有效值。输入使用 Float32/64。曲线控制点未批处理；Field 的 cell shape 为二维，`batch_axes` 只能为空或 `[1,1]`。Table 未批处理，并与 field dtype 相同。所有操作的输出端口均为 `value`。除 smoothstep 外，输出使用 generic `photospider.tensor` v1/member `samples`；smoothstep 恢复 image coverage Result schema。

## 数学语义与区域

曲线采样器在闭区间内均匀放置 `count` 个坐标，包含两端。线性插值经过每个控制点。Monotone 插值使用保持形状的三次 Hermite 曲线，内部斜率采用加权调和平均，端点斜率受限；两个控制点退化为线性段。`reject` 要求采样域处于控制点 x 范围内；`clip` 在范围外返回最近端点。查询坐标互异且可表示、控制点有限且 x 严格递增，以及输出值可由目标 dtype 表示，均为调用方前提。

`field.apply_lut_1d` 将闭合输入域映射到表首尾样本，并在表样本之间线性插值。`reject` 将消费的场值位于闭区间内作为调用方前提；`clip` 在区间外返回端点值。表输入是普通 generic tensor member。

## 执行与错误

四个 curve／field 算子准备不可变 schema 和数值参数，再 lower 为固定 CPU Host invocation。每个曲线调用生成一个任务，读取全部控制点 tiles，按递增顺序写入所有选中的输出 tiles。计算使用全局输出索引，因此稀疏选择保留原采样网格。线性采样使用零 scratch。PCHIP 在预分配的 `24*K` 字节 Host scratch 中构建一次区间宽度、割线和斜率；工作区由本次 invocation 独占。多个 Run 共享不可变准备状态，各自拥有数值工作区。

每个 field 输出 tile 对应一个任务。LUT 任务读取对应的 field 区域和完整表；smoothstep 任务读取对应的 field 区域，并在源无 batch 时，将 `[1,1,H,W]` 输出坐标投影为 `[H,W]` 输入坐标。输入覆盖按各源自身的 tile grid 确定，包括 batch 和 atomic 轴。两个 field 算子均使用零 scratch。例如，选择一个 smoothstep 空间 tile 只读取覆盖该矩形的源 tiles；选择两个分离的曲线 tiles 仍只构建一次 PCHIP 斜率，并仅计算选中的全局索引。

准备阶段检查规范输出视图、精确输入 tile 集合、任务参数和 scratch 容量，随后取得固定入口。运行时 invocation 读取借用的输入视图，写入预分配输出视图，检查取消并恢复调用方浮点环境。成功任务的输出可供依赖任务读取。当调用所需的全部任务成功并按序退休时，执行器公开 Result 所有权。失败调用不公开部分 Result。

有限算术样本、递增曲线控制点、互异查询坐标、可表示输出及 `reject` 的 payload 域条件属于调用方前提。参数有限性、顺序和 LUT 浮点数转索引的数值域也属于 unchecked 前提，即使 preparation 已知参数值。算子不扫描输入或完成后的输出进行验证。算法保留 PCHIP 割线、可逆距离缩放和补偿乘积对齐所需的精度判定，以及有界 LUT 单元修正。这些检查通过 invocation 内的有界错误状态报告 `OperationFailed`，不分配异常字符串。参数类型、语法或静态结构错误返回 `InvalidArgument`，schema 不兼容返回 `TypeMismatch`，取消和资源耗尽保留各自状态码。

图像含义由算子定义。普通图像默认 straight alpha，不自动绑定 RGBA 合规 validator。显式 association 转换和算法所需检查仍属于算子行为，kernel schema 标签不认证像素。
