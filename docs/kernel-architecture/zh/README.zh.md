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
change-local 计划和清单，但不是独立公开完成权威。已完成迁移和失效提案可以保留在
`docs/outdated/` 或归档 change 记录中；它们是历史资料，不是活跃事实来源。

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

## 阅读顺序

1. [概览](Overview.zh.md)说明产品 seam、模块所有权和顶层调用图。
2. [术语](Terminology.zh.md)定义当前领域语言以及不得混淆的概念。
3. [数据模型](Data-Model.zh.md)和[图生命周期](Graph-Lifecycle.zh.md)说明图状态、
   拓扑、持久化行为和变更语义。
4. [计算边界](Compute-Boundaries.zh.md)和[计算流程](Compute-Flow.zh.md)说明规划、
   裁剪、派发、HP/RT intent 和提交行为。
5. [缓存模型](Cache-Model.zh.md)和[脏区传播](Dirty-Region-Propagation.zh.md)定义
   缓存权威、ROI 数学、脏状态和 tile 映射。
6. [调度器架构](Scheduler-Architecture.zh.md)定义当前 ready-task 调度器接口和物理
   worker 所有权。
7. [ImageBuffer 内存契约](ImageBuffer-Memory-Contract.zh.md)和
   [插件 ABI](Plugin-ABI.zh.md)定义内存、设备、operation、scheduler 和 DSO 契约。

已接受的合并后方向单独记录在
[内核演进目标](../../roadmap/zh/Kernel-Evolution.zh.md)，不能作为当前实现证据。

## 文档结构

维护中的领域文档应按需回答：

1. 哪些术语和状态属于该领域？
2. 调用方可以观察到什么行为？
3. 谁拥有状态，生命周期多长？
4. 当前哪些模块和调用路径实现这些行为？
5. 哪些不变量和禁止依赖定义行为边界？
6. 为什么采用当前机制？
7. 错误、取消和部分工作如何暴露？
8. 哪些源码和长期测试是实现入口？

英文文档是权威来源。`zh/` 下的文件是忠实、面向读者的中文翻译，并在同一变更中更新。
