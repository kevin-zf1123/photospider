# 内核架构文档

本目录说明当前源码树中已经存在的内核。目标读者是需要理解可观察行为、所有权、
实现机制、不变量、故障语义以及对应源码位置的开发者。

## 信息边界

每类信息只有一个权威归属：

| 信息 | 权威位置 |
| --- | --- |
| 当前内核行为与实现 | `docs/kernel-architecture/` |
| 已接受的架构决策 | `docs/adr/` |
| 稳定的未来架构目标 | `docs/roadmap/` |
| 构建、测试与验证指引 | `docs/development/` 和 `docs/CI/` |
| 实施任务与当前进度 | GitHub Projects、Issues 和活动 OpenSpec change |
| 已完成迁移与失效提案 | `docs/outdated/` 和已归档 OpenSpec change |

内核架构文档不得包含任务勾选、实施阶段报告、迁移状态表或没有时间边界的 TODO。
未来概念只有在成为当前软件行为后才进入本目录；在此之前，它属于 roadmap 或 ADR。

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
