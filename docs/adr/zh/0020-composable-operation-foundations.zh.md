# ADR 0020：描述静态 Tensor 并推导算子输出

- 状态：Accepted
- 英文权威文档：[English](../0020-composable-operation-foundations.md)

## 1. 核心摘要

组合要求每个输出结构在执行前已知。Result 端口、具名输出、结构约束、纯 preparation 和显式绑定 validator 提供此契约。领域解释属于算子参数和 SDK helper。

## 2. 模型与边界

Preparation 接收有序输入 schema 和静态参数，解析输出 schema 与输入投影、选择不可变资源并固定共享 stage 声明。编译器把这些契约纳入 identity。运行期 callback 在固定结构下计算样本。Tensor 存储不认证数值域，也不传播颜色描述。

## 3. 正式契约与 API

`TensorDescriptor` 指定 dtype 和正 shape。完整样本 rank 仍为 1 到 8。`SchemaTemplate` 包含 1 到 16 个具名静态 Tensor 成员，具有显式 batch/atomic 轴和可选空间拓扑。Extent 固定或由静态输入轴/元素数推导。运行期样本不选择成员数、dtype、rank 或 shape。

OperationTraits 27 和 Result C ABI 5 暴露 Result-only 端口。`OperationPortConstraint` 提供结构谓词和显式 validator identity；`OperationOutputTraits` 提供每个输出 schema 和输入投影。不再具有端口 kind、内核 scalar-bounds 区间或 Region 执行规则字段。

只有算子显式绑定 ops SDK `ops.scalar-range` validator 时才检查 Float32 有限闭区间。成员 slot、上下界和 validator 实现进入准备后的校验 identity。普通 scalar `[1]` 输入不自动附加区间要求。完整 validator 要求其范围内完整授权，且不自行增加 Need。

数据依赖数量使用静态容量 Tensor 与普通 `count`/mask payload；消费者读取这些值需要 payload Need。已发布 tile 必须完整，并按算子规定填充未使用位置；完整结果算法容量不足时失败。

需要完整输入的算法使用非阻塞 SDK 获取流程和准备后的 Run 共享 stage。输出从共享结果发布所选成员 tile。独立输出保留各自投影，联合 stage 具有独立固定投影。见 [Global Results](../../kernel-architecture/zh/Global-Results.zh.md)。

## 4. 非目标与明确边界

内核不附着领域描述、不推断 RGBA、不按 shape 选择语义 validator、不插入 cast 或颜色转换，也不增加动态 Run 输出长度。显式 association 转换和 ops 领域配置仍是合法计算选项。

## 5. 影响

非法 schema、参数和缺失资源在 payload callback 前失败。运行期发布必须匹配准备后的 schema 和 tile 范围。空间 tile 完整覆盖非 H/W cell 轴，独立于 atomic closure。结构、相关投影、stage 定义、validator 或数值实现改变时，相应 identity 随之改变。舍入、有限性检查和归约顺序仍属于算子契约，独立数学 oracle 不为实现而改变。
