# Kernel 架构

英文为权威来源，中文读者版位于本目录。[Compiler 与 Executor 设计](Compiler-and-Execution.zh.md)、[并行模型](Parallel-Execution-Model.zh.md)、[tile 存储](Cache-Model.zh.md)、[依赖](Dependency-Data.zh.md)和[术语](Terminology.zh.md)规定 breaking replacement。文档的提案状态独立于实现进度。

当前 C++ 路径编译物理 Task/Tile/Call/Block 表并执行 prepared CPU kernel。[Plugin ABI](Plugin-ABI.zh.md)说明 operation module ABI 8、固定 invocation 与所有权。[Compiler 工具](../../development/zh/Compiler-Tools.zh.md)说明 v5 artifact CLI。[Fragment atlas](Fragment-Atlas.zh.md)说明独立的 Tensor packing 工具。固定执行器已准备并执行真实 Metal/Vulkan kernel，支持 Shared/Device 存储与显式 COPY。[原生测量](Parallel-Execution-Model.zh.md#原生测量)记录工作负载、容量、计时和 drain 证据。既有 ADR 保持决策记录，新 Plan 设计列出替换范围。

## 核心模型

1. [概览](Overview.zh.md)：模块所有权、执行上下文与结果生命周期。
2. [术语](Terminology.zh.md)：架构指南中的统一用语。
3. [Compiler 与执行](Compiler-and-Execution.zh.md)：静态 Task/Tile/Call/Block 表、设备 prepare、有序退休和 breaking artifact 契约。
4. [数据模型](Data-Model.zh.md)：Tensor、Region、布局与保留的存储。
5. [Graph 生命周期](Graph-Lifecycle.zh.md)：图版本与编译阶段的生命周期。
6. [Compute 边界](Compute-Boundaries.zh.md)：固定 CPU/GPU placement、显式传输与完成所有权。
7. [Cache 模型](Cache-Model.zh.md)：独立 tile 分配、生命周期 hold，以及无 payload cache 的 block admission。
8. [Plugin ABI](Plugin-ABI.zh.md)：可信进程内 operation 接口。
9. [图像算子](Image-Operations.zh.md)：图像描述符、端口与当前图像契约。
10. [Region 语义](Region-Semantics.zh.md)：逻辑覆盖、输入需求与边界行为。

## 工作流与专题

- [Executor 语义原型](../prototypes/ooo_executor.py)：使用 Python 3 运行，`--test` 检查调度、退休、失败和 buffer 所有权。GPU completion 为模拟。

- [Compiler 产物](Compiler-and-Execution.zh.md#compiler-产物)：`.psir`/`.psxp` v5 边界与 checked import。
- [版本契约](../../development/zh/Compiler-Version-Contract.zh.md)：package、ABI、schema、artifact 和 identity 的独立版本。

- [固定 native 集成](../../../tests/integration/kernel/test_fixed_native.cpp)与 [Vulkan 检查](../../../tests/unit/kernel/test_vulkan_gpu.cpp)：真实 native 行为覆盖；硬件相关用例在设备不可用时明确 skip。
- [基础算子](Basic-Operations.zh.md)：内置数值、颜色和图像算子的输入输出与执行规则。
- [受管资源](Managed-Resources.zh.md)：资源预算、所有权、保留和释放。
- [Atom 错误与质量](Atom-Errors-and-Quality.zh.md)：原子执行错误范围与数值质量等级。
- [全局结果](Global-Results.zh.md)：静态 Tensor 成员、captured coverage、owning publication 与结果生命周期。

算子专题指南分别说明其输入、输出和执行规则。[内置算子索引](../../built-in_ops/README.md)提供这些契约的导航。
