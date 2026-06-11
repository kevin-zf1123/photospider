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

执行粒度是另一层概念。一个 graph 可以包含多个 node，每个 node/operator 实现可以是
monolithic 或 tiled。tiled 执行还可以进一步使用 macro 或 micro task 粒度。这些选择是
node 执行细节，独立于 HP/RT intent 语义。

HP/RT compute domain 与 Micro/Macro 粒度是正交关系。当前实现默认偏向用 RT Micro_16
服务交互，用 HP Macro_256 服务吞吐，但模型中仍然存在四种不同的 domain/granularity 组合：

| 情况 | 当前 tile size | 含义 |
| --- | --- | --- |
| `rt-micro` | RT proxy space 中的 16x16 | 低延迟 RT 代理 tile。 |
| `rt-macro` | RT proxy space 中的 64x64 | 更粗的 RT 代理 tile 或聚合后的 RT work。 |
| `hp-micro` | HP full-resolution space 中的 64x64 | 小 HP tile。 |
| `hp-macro` | HP full-resolution space 中的 256x256 | 吞吐优先的 HP tile。 |

`rt-macro` 和 `hp-micro` 当前同为 64x64，但它们不是同一种 task type，因为它们处在不同坐标空间和不同 task pool 中。Compute plan 不得创建 RT task 到 HP task 的依赖，也不得创建 HP task 到 RT task 的依赖。Realtime intent 协调分离的 HP 与 RT sibling work；尺度转换只用于表达对应 ROI、downsample 状态或 inspection 数据。

## 计算意图

内核识别两个正式计算意图：

| 意图 | 含义 |
| --- | --- |
| `GlobalHighPrecision` | 完整质量 HP 计算，拥有高精度输出。非 realtime 计算只启用这条 HP 路径。 |
| `RealTimeUpdate` | 交互式 realtime 更新，需要 dirty ROI，并启用 HP/RT 双路径。 |

意图模型是正式的。`ComputeService` 仍是 compute facade 和 planning boundary，而 parallel
planned work 会通过每个 intent 配置的 `IScheduler` task runtime dispatch。

HP/RT 双路径语义属于 realtime intent，而不是 parallel 执行模式。Realtime 模式下，HP
计算完整尺寸的权威 node 工作，RT 计算降采样代理版本，目前为宽高各四分之一，也就是像素数的
十六分之一。HP 和 RT 工作应被视为分离的 intent task pool。之后由 scheduler/resource
policy 决定每个 pool 如何执行：HP 可以使用 single-thread scheduler，RT 可以使用 GPU
scheduler，realtime 和 non-realtime 模式也可以使用不同的 scheduler 配置。

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

并行计算从 `topo_postorder_from` 构建 DAG，跟踪依赖计数器，并通过已配置 scheduler 的
`SchedulerTaskRuntime` 提交 ready 节点任务。Tiled 操作可能产生微任务并递增 scheduler-owned
完成计数器。

`ParallelGraphExecutor` 将依赖计数、稀疏 node-id 映射、临时结果存储、事件日志、异常传播和最终目标选择保留在 compute-service 边界内。它通过 scheduler task-runtime queue
dispatch 已经 planned 的 work；它不会让 scheduler 拥有 dirty propagation 或 compute-task
derivation。

## GlobalHighPrecision

`GlobalHighPrecision` 是完整质量路径。没有 dirty ROI 时，它执行普通完整计算。在当前代码中，global compute 上的 dirty ROI 在某些入口路径中仍可能触发完整重算。

HP 脏区更新会计算反向 ROI 计划，将脏区对齐到 HP tile 边界，更新受影响 HP tile，记录 HP ROI/版本元数据，并可调度 downsample 工作刷新 RT 临时状态。

Dirty-region state planning 现在通过图级 `DirtyRegionPlanner` 运行，产生的
`DirtyRegionSnapshot` 会输入 compute task planning，并提供给交互层 inspection 摘要。

## RealTimeUpdate

`RealTimeUpdate` 需要 dirty ROI。没有 `dirty_roi` 的请求是非法的，应通过内核和交互层 API 返回清晰错误。它不会隐式表示全帧 RT 更新。

带有效 dirty ROI 时，realtime 计算会启用两条路径。HP 更新受影响 graph 工作的完整尺寸权威输出，
RT 更新受影响区域的代理输出。当 scheduler task runtime 可用时，实现可以把 HP 和 RT 更新并发提交到各自 intent-specific scheduler；当选择单线程执行时，它仍会 inline 运行 HP 和 RT 工作。
这个区别是执行模式选择，而不是启用或禁用 HP/RT 双路径的开关。

Realtime planning 有意按路径分别执行，而不是通过一次混合 domain 的 planner 调用生成两份任务池。
`IntentUpdateCoordinator` 会分发 sibling HP 与 RT update callback。HP callback 调用
`DirtyRegionPlanner::plan_high_precision()`，随后以 `GlobalHighPrecision` 调用共享的
`ComputeTaskPlanner`；RT callback 调用 `DirtyRegionPlanner::plan_real_time()`，随后以
`RealTimeUpdate` 调用同一个 `ComputeTaskPlanner`。每次 planner 调用只产生一个单一 domain
的 task graph。这样能保持 `ComputeTaskPlanner` 简洁，并让未来新增 task pool 或 task mode 时继续复用同一 planner contract。

传入的 dirty ROI 会在当前请求中转换为图级 planner state。TODO：未来 frontend-driven
dirty-region update 应由 node-local dirty report 作为 origin source。

当前默认值：

| 参数 | 当前值 | 状态 |
| --- | --- | --- |
| RT downscale factor | `4` | 可调实现默认值。 |
| RT micro tile size | `16` | 可调实现默认值。 |
| RT macro tile size | `64` | 可调实现默认值；与 HP micro 数值相同，但 domain 不同。 |
| HP micro tile size | `64` | 可调实现默认值。 |
| HP macro tile size | `256` | 可调实现默认值。 |

这些常量不是永久 ABI。

## 事件和计时

`GraphEventService` 收集每节点计算事件。启用计时时，`TimingCollector` 存储节点计时和总计算耗时。`NodeOutput` 中的调试元数据记录 worker id、时间戳、执行时间、设备和可选范围检查。

## 错误处理

计算失败会尽可能抛出带 `GraphErrc` 分类的 `GraphError`。`Kernel` 捕获这些错误，并为前端检查存储每图 `LastError`。
