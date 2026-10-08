# Kernel 架构

这些指南说明 kernel 当前的契约和运行行为。英文为权威来源；中文读者版位于本目录。从[概览](Overview.zh.md)开始了解所有权和执行流程。

## 核心模型

1. [概览](Overview.zh.md)：模块所有权、执行上下文与结果生命周期。
2. [术语](Terminology.zh.md)：架构指南中的统一用语。
3. [Compiler 与执行](Compiler-and-Execution.zh.md)：源文档校验、规划、调度和运行推进。
4. [数据模型](Data-Model.zh.md)：Value、Region、布局与保留的存储。
5. [Graph 生命周期](Graph-Lifecycle.zh.md)：图版本与编译阶段的生命周期。
6. [Compute 边界](Compute-Boundaries.zh.md)：CPU/GPU 放置与回退规则。
7. [Cache 模型](Cache-Model.zh.md)：冻结输入与有界本地结果复用。
8. [Plugin ABI](Plugin-ABI.zh.md)：可信进程内 operation 与 provider 接口。
9. [图像算子](Image-Operations.zh.md)：图像描述符、端口与当前图像契约。
10. [Region 语义](Region-Semantics.zh.md)：逻辑覆盖、输入需求与边界行为。

## 工作流与专题

- [Result quickstart](../../../examples/result_quickstart/README.md)：使用安装包 operation 与 workflow 的最小示例。
- [Foundations 工作流](../../../examples/foundations_workflow/README.zh.md)：可运行的安装包示例及当前场景。
- [GPU 集成测试](../../../tests/integration/gpu/)：已注册的 GPU 行为覆盖；硬件相关用例在设备不可用时明确 skip。
- [整数统计](Integer-Statistics.zh.md)：分页直方图与流式调色算子。
- [外轴 FFT](External-FFT.zh.md)：分页 FFT 与资源边界。
- [分页连通域](Paged-Components.zh.md)：连通域标签与分页筛选。
- [基础算子](Basic-Operations.zh.md)：内置数值、颜色和图像算子的输入输出与执行规则。
- [受管资源](Managed-Resources.zh.md)：资源预算、所有权、保留和释放。
- [Atom 错误与质量](Atom-Errors-and-Quality.zh.md)：原子执行错误范围与数值质量等级。
- [全局结果](Global-Results.zh.md)：分页结果、发布策略和结果生命周期。

算子专题指南分别说明其输入、输出和执行规则。[内置算子索引](../../built-in_ops/README.md)提供这些契约的导航。
