# 内核计算流程

本文档描述当前计算路径和目标调度器方向。

## 入口点

典型前端流程：

```text
CLI / TUI
  -> InteractionService
  -> Kernel
  -> GraphRuntime
  -> ComputeService
  -> OpRegistry / GraphCacheService / GraphTraversalService
```

`Kernel` 拥有多图 API。`GraphRuntime` 拥有一个图模型、事件服务、worker 状态、平台 context 和调度器实例。

`InteractionService` 是面向前端的 kernel interaction facade。它的总体职责是将 CLI/TUI/frontend
命令与 kernel 内部解耦。在 dirty-region 语境中，它应暴露图级 dirty snapshot 查询和可视化 hook；
它不是 dirty-region generation 或 propagation 的权威来源。

`ComputeService` 拆分后的目标 planning 流程：

```text
ComputeService facade
  -> DirtyRegionPlanner
  -> DirtyRegionSnapshot
  -> ComputeTaskPlanner
  -> ComputePlan / ComputeTaskGraph
  -> task pools / scheduler / execution resources
```

单线程和并行执行应共享同一逻辑 `ComputePlan` 或 `ComputeTaskGraph`。执行模式应在 task pool、
scheduler policy 和执行资源上不同，而不是在图级 dirty propagation 或 compute-task derivation 上不同。

## 计算意图

内核识别两个正式计算意图：

| 意图 | 含义 |
| --- | --- |
| `GlobalHighPrecision` | 完整质量 HP 计算，拥有高精度输出。 |
| `RealTimeUpdate` | 交互式更新，需要 dirty ROI。 |

意图模型是正式的。当前实现仍有若干绕过 `IScheduler` 并直接调用 `ComputeService` 的路径。

`ComputeService` 的就地拆分记录在 `Compute-Service-Split.md`。当前实现保留公开
facade，并将内部工作路由到 compute-service 协作者。

## 顺序计算

顺序计算使用递归依赖解析：

1. 验证目标节点。
2. 解析遍历顺序，并可选地清理缓存。
3. 对每个依赖递归计算上游节点。
4. 基于静态参数和参数输入构建 `runtime_parameters`。
5. 为 HP 意图解析操作实现。
6. 执行 monolithic 或 tiled 操作。
7. 存储输出、发出事件、更新时间，并在启用时保存磁盘缓存。

顺序计算适合简单执行和调试。它现在会先创建与并行路径相同的内部 `ComputeTaskPlanner`
plan 语义，再执行递归路径。

## 并行计算

并行计算从 `topo_postorder_from` 构建 DAG，跟踪依赖计数器，并将 ready 节点任务提交到 `GraphRuntime` worker 队列。Tiled 操作可能产生微任务并递增运行时完成计数器。

该路径是当前行为，但也是调度器迁移表面的一部分。正式长期目标是在 dirty-region state 和
compute-task planning 已产生 planned work 后，通过 `IScheduler` 实例路由计算。

遗留 `GraphRuntime` 队列路径已经隔离到 `ParallelGraphExecutor` 后面。TODO：完整
planned-task scheduler routing 仍留给后续 scheduler-focused change。

## GlobalHighPrecision

`GlobalHighPrecision` 是完整质量路径。没有 dirty ROI 时，它执行普通完整计算。在当前代码中，global compute 上的 dirty ROI 在某些入口路径中仍可能触发完整重算。

HP 脏区更新会计算反向 ROI 计划，将脏区对齐到 HP tile 边界，更新受影响 HP tile，记录 HP ROI/版本元数据，并可调度 downsample 工作刷新 RT 临时状态。

Dirty-region state planning 现在通过图级 `DirtyRegionPlanner` 运行，产生的
`DirtyRegionSnapshot` 会输入 compute task planning，并提供给交互层 inspection 摘要。

## RealTimeUpdate

`RealTimeUpdate` 需要 dirty ROI。没有 `dirty_roi` 的请求是非法的，应通过内核和交互层 API 返回清晰错误。它不会隐式表示全帧 RT 更新。

带有效 dirty ROI 时，RT 计算会更新受影响区域的代理输出，并可能通过运行时排入后台 HP 更新。

传入的 dirty ROI 会在当前请求中转换为图级 planner state。TODO：未来 frontend-driven
dirty-region update 应由 node-local dirty report 作为 origin source。

当前默认值：

| 参数 | 当前值 | 状态 |
| --- | --- | --- |
| RT downscale factor | `4` | 可调实现默认值。 |
| RT tile size | `16` | 可调实现默认值。 |
| HP micro tile size | `64` | 可调实现默认值。 |
| HP macro tile size | `256` | 可调实现默认值。 |

这些常量不是永久 ABI。

## 事件和计时

`GraphEventService` 收集每节点计算事件。启用计时时，`TimingCollector` 存储节点计时和总计算耗时。`NodeOutput` 中的调试元数据记录 worker id、时间戳、执行时间、设备和可选范围检查。

## 错误处理

计算失败会尽可能抛出带 `GraphErrc` 分类的 `GraphError`。`Kernel` 捕获这些错误，并为前端检查存储每图 `LastError`。
