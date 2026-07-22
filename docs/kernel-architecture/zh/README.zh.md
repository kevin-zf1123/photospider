# 内核架构文档

本目录说明当前源码树中已经存在的内核。目标读者是需要理解可观察行为、所有权、
实现机制、不变量、故障语义以及对应源码位置的开发者。

## 信息边界

[ADR 0006](../../adr/zh/0006-kernel-documentation-separates-facts-decisions-targets-and-status.zh.md)
规定文档信息架构。四个活跃层具有不同权威来源与时间含义：

| 层级 | 权威位置 | 时间含义 |
| --- | --- | --- |
| 当前事实 | `docs/kernel-architecture/` | 当前源码树中的行为与所有权。 |
| 架构决策 | `docs/adr/` | 持久决策及其决策时背景。 |
| 演进目标 | `docs/roadmap/` | 稳定的已接受方向，不表示当前行为。 |
| 实施状态 | 链接的 GitHub Project 和 Issue | 一个交付切片的实时状态、依赖与验证结果。 |

构建、测试和验证指引继续位于 `docs/development/` 与 `docs/CI/`。活动 OpenSpec change 可以保存
change-local 计划和清单，但不是独立公开完成权威。已完成迁移和失效提案由
[过时文档归档索引](../../outdated/zh/README.zh.md)说明，或保留在归档 change 记录中；它们是历史
证据，不是活跃事实来源。

内核架构文档包含可观察行为、已实现的所有权与机制、当前限制、不变量、故障语义和源码/测试
入口。它们不得包含任务勾选、实施阶段报告、迁移状态表、没有时间边界的 TODO 或未来 runtime
object。未来概念只有在代码和长期验证使其成为当前软件行为后，才会进入本目录。

## 交叉引用与更新规则

- 当前文档可以链接 ADR 说明理由，链接 roadmap 提供明确标记的未来背景；这些链接不会让目标
  对象成为当前事实。
- ADR 背景是决策时历史 snapshot。即使已接受 ADR 的迁移尚未完成，维护中的行为仍由本目录
  权威说明。
- Roadmap 可以概述明确标记的当前基线，但必须服从对应当前文档并链接 governing ADR。
- 每个实施 Issue 或 PR 都要引用相关当前文档、governing ADR、精确 roadmap 目标、实时
  Project/Issue 状态和实际验证证据。
- 目标行为落地时，同一变更更新代码、长期测试、受影响的英文当前状态文档及其中文镜像。
  目标改变时更新 roadmap；决策改变时必须新建或以新 ADR 取代原 ADR。仅状态变化不会改变
  上述任何层。

## 文档地图

应按问题使用维护中文档，而不是按文件年代使用。一个文档可以支撑多个视角，但每项事实都有一个
主要归属：

| 视角 | 回答的问题 | 主要文档 |
| --- | --- | --- |
| 术语 | 当前名称与状态是什么意思，哪些概念必须保持区分？ | [术语](Terminology.zh.md)和[数据模型](Data-Model.zh.md) |
| 行为 | 调用方在图生命周期、计算、缓存和脏区工作中能观察到什么？ | [图生命周期](Graph-Lifecycle.zh.md)、[计算流程](Compute-Flow.zh.md)、[缓存模型](Cache-Model.zh.md)和[脏区传播](Dirty-Region-Propagation.zh.md) |
| 实现 | 当前哪些模块拥有这些行为，调用或派发路径是什么？ | [概览](Overview.zh.md)、[计算边界](Compute-Boundaries.zh.md)和[策略与执行架构](Policy-and-Execution-Architecture.zh.md) |
| 边界 | Consumer 可以依赖哪些值、所有权规则、不变量、限制与故障表面？ | [ImageBuffer 内存契约](ImageBuffer-Memory-Contract.zh.md)、[插件 ABI](Plugin-ABI.zh.md)和[计算边界](Compute-Boundaries.zh.md) |
| 原理 | 为什么当前机制如此拆分，哪些持久决策约束它？ | 当前文档中的原理章节和 governing [ADR](../../adr/zh/) |

首次阅读时，应先看[术语](Terminology.zh.md)，再看[概览](Overview.zh.md)，随后按正在修改的
子系统进入对应行为或边界文档。每份领域文档末尾的源码与长期测试入口构成当前陈述的证据链。

已接受的合并后方向单独记录在
[内核演进目标](../../roadmap/zh/Kernel-Evolution.zh.md)。它是目标，不能作为当前实现证据。历史
阶段报告和迁移计划继续保存在[过时文档归档](../../outdated/zh/README.zh.md)中；使用前必须对照
维护中的当前文档核验。

## 当前文档结构

维护中的领域文档在相应视角适用时采用以下顺序：

1. 术语与当前状态；
2. 可观察行为；
3. 当前实现与所有权；
4. 边界、不变量、限制与故障语义；
5. 当前机制的原理；
6. 源码与长期测试入口。

领域特定标题可以细化这些视角，但不得把未来目标变成当前对象，也不得重复迁移清单。文档需要
未来背景时，必须先说明当前限制，再链接精确 roadmap 目标或 governing ADR。

英文文档是权威来源。`zh/` 下的文件是忠实、面向读者的中文翻译，并在同一变更中更新。
