# Kernel 架构

英文为权威来源，中文读者版位于本目录。[Compiler 与 Executor 设计](Compiler-and-Execution.zh.md)、[并行模型](Parallel-Execution-Model.zh.md)、[tile 存储](Cache-Model.zh.md)、[依赖](Dependency-Data.zh.md)和[术语](Terminology.zh.md)规定提出的 breaking replacement，并明确区分目标与当前 C++ runtime。执行器工作从这些页面开始。

其他指南，包括 Overview、Plugin ABI、Global Results、GPU Discovery、Compute Boundaries 和 Managed Resources，描述既有实现；其中 continuation、认证、缓存、同步 GPU 和 fallback API 不约束替换设计。既有 ADR 保持决策记录，新 Plan 设计列出替换范围。本次文档修改不表示实现或 ABI 已升级。

## 核心模型

1. [概览](Overview.zh.md)：模块所有权、执行上下文与结果生命周期。
2. [术语](Terminology.zh.md)：架构指南中的统一用语。
3. [Compiler 与执行](Compiler-and-Execution.zh.md)：静态 Task/Tile/Call/Block 表、设备 prepare、有序退休和 breaking artifact 契约。
4. [数据模型](Data-Model.zh.md)：Tensor、Region、布局与保留的存储。
5. [Graph 生命周期](Graph-Lifecycle.zh.md)：图版本与编译阶段的生命周期。
6. [Compute 边界](Compute-Boundaries.zh.md)：CPU/GPU 放置与回退规则。
7. [Cache 模型](Cache-Model.zh.md)：独立 tile 分配、生命周期 hold，以及无 payload cache 的 block admission。
8. [Plugin ABI](Plugin-ABI.zh.md)：可信进程内 operation 接口。
9. [图像算子](Image-Operations.zh.md)：图像描述符、端口与当前图像契约。
10. [Region 语义](Region-Semantics.zh.md)：逻辑覆盖、输入需求与边界行为。

## 工作流与专题

- [Executor 语义原型](../prototypes/ooo_executor.py)：使用 Python 3 运行，`--test` 检查调度、退休、失败和 buffer 所有权。GPU completion 为模拟。

- [Compiler 产物](Compiler-and-Execution.zh.md#compiler-产物)：目标 `.psir`/`.psxp` v5 边界与 checked import；当前 codec 仍为 v4。
- [版本契约](../../development/zh/Compiler-Version-Contract.zh.md)：package、ABI、schema、artifact 和 identity 的独立版本。

- [GPU 集成测试](../../../tests/integration/kernel/gpu/)：已注册的 GPU 行为覆盖；硬件相关用例在设备不可用时明确 skip。
- [基础算子](Basic-Operations.zh.md)：内置数值、颜色和图像算子的输入输出与执行规则。
- [受管资源](Managed-Resources.zh.md)：资源预算、所有权、保留和释放。
- [Atom 错误与质量](Atom-Errors-and-Quality.zh.md)：原子执行错误范围与数值质量等级。
- [全局结果](Global-Results.zh.md)：静态 Tensor 成员、发布、固定授权与结果生命周期。

算子专题指南分别说明其输入、输出和执行规则。[内置算子索引](../../built-in_ops/README.md)提供这些契约的导航。
