# Kernel 架构

这些文档描述破坏性产品边界重置后的当前 kernel 行为。英文是权威来源；本目录保存
忠实中文镜像。

[ADR 0015](../../adr/zh/0015-breaking-product-boundary-scope-reset.zh.md) 是最高
active 产品边界权威。

公开 live delivery state 由 GitHub Issue 维护。GitHub Project 是这些 Issue 的
maintainer operational view。Checked-in
[当前开发计划](../../development/zh/Current-Development-Program.zh.md)汇总 baseline 与
执行顺序，不修改本架构。

## 阅读顺序

1. [概览](Overview.zh.md)
2. [术语](Terminology.zh.md)
3. [Compiler 与执行](Compiler-and-Execution.zh.md)
4. [数据模型](Data-Model.zh.md)
5. [Graph 生命周期](Graph-Lifecycle.zh.md)
6. [Compute 流程](Compute-Flow.zh.md)
7. [Compute 边界](Compute-Boundaries.zh.md)
8. [Cache 模型](Cache-Model.zh.md)
9. [Region 语义](Region-Semantics.zh.md)
10. [Plugin ABI](Plugin-ABI.zh.md)

Kernel 是 session-agnostic 的，不拥有 daemon Job、network service、durable work、
process supervisor、policy DSO、plugin security product、durable result object 或
release evidence。重置前文档只能从 Git 历史和
`pre-breaking-scope-reset-2026-09-01` 取得。
