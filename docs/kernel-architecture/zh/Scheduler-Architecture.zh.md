# 调度器架构

内核具有用于 intent-aware 资源 dispatch 的正式调度器接口。本文档定义 planned parallel work
已经通过 scheduler-owned task runtime 路由之后，如何理解当前实现。

## 正式目标：IScheduler

`IScheduler` 是正式调度器接口。调度器附着到 `GraphRuntime`，启动 worker 资源，调度计算，并干净关闭。

核心生命周期：

```text
create -> attach(runtime) -> start -> dispatch planned tasks -> shutdown -> detach
```

按 `ComputeIntent` 路由计算：

| 意图 | 预期调度器角色 |
| --- | --- |
| `GlobalHighPrecision` | 面向吞吐的 HP 计算。 |
| `RealTimeUpdate` | 低延迟交互式更新。 |

`GraphRuntime` 保存以 `ComputeIntent` 为 key 的调度器映射。

长期 scheduler 职责是 planning 之后的资源 dispatch。Scheduler 应从 intent-aware task pool
中拉取 planned 或 annotated task，选择队列顺序、批处理、worker 策略、取消和具体执行资源，
然后分发工作。它们不应拥有图级 dirty-region propagation 或 compute-task derivation。

在当前并行运行时路径中，一个 `RealTimeUpdate` 请求可以有意为同一脏区同时提交 RT 预览工作与 HP 更新工作。RT 工作保留交互反馈，而 HP 工作让任务池和正式 HP 缓存与图状态保持同步。如果这个双提交路径带来阻塞、饥饿或 worker 重入问题，这些问题属于调度器设计责任：应恰当地保留或窃取 worker，使用 epoch 和取消处理陈旧 RT 工作，并避免可能让 worker 所有的执行死锁的等待策略。不要通过让 RT 输出成为正式 HP 缓存来源来解决。

## 当前 Dispatch 状态

并行计算规划和 plan execution 属于 `ComputeService` 协作者：`FullTaskGraphExpander`、
`NodeCacheTaskGraphPruner`、`DirtyRegionPlanner`、`DirtySnapshotTaskGraphPruner`、
`IntentUpdateCoordinator` 和 `ComputeTaskDispatcher`。裁剪完成后，`ComputeTaskDispatcher`
会把 node/cache-pruned task graph 或 dirty 裁剪后的 update work set materialize 为具体 task，
并通过相关 `ComputeIntent` 配置的 `IScheduler` 实例，经由 `SchedulerTaskRuntime` 提交 ready work。

`GraphRuntime` 仍拥有图状态、scheduler 注册、事件，以及一些供 graph-runtime support path
和测试使用的 runtime queue API。这些队列不再是 compute-service parallel dispatch
路径。新的 scheduler-facing 设计应扩展 `IScheduler` 与 `SchedulerTaskRuntime`，而不是新增对
`GraphRuntime` 内部队列的依赖。

## 内置调度器

| 类型 | 含义 |
| --- | --- |
| `cpu_work_stealing` | 多线程 CPU 调度器。 |
| `serial_debug` | 确定性单线程调试调度器。 |
| `gpu_pipeline` | 异构 CPU/GPU 调度器。 |
| `heterogeneous` | `gpu_pipeline` 的别名。 |

## 调度器 Dispatch 边界

`IScheduler` 不再暴露 compute-planning helper。已移除的 planning interface 不得重新引入，
避免 scheduler 实现意外拥有 graph/task planning。

目标模型是：

```text
GraphModel topology
  -> FullTaskGraphExpander
  -> FullTaskGraph
  -> NodeCacheTaskGraphPruner
  -> ComputePlan / pruned ComputeTaskGraph
  -> DirtyRegionSnapshot
  -> DirtySnapshotTaskGraphPruner
  -> DirtyUpdateWorkSet
  -> Scheduler resource dispatch
```

设备选择、队列选择、批处理、worker reservation、取消和资源专用 dispatch 属于 scheduler。
Dirty propagation、dirty work-set activation、node/tile 展开、monolithic dirty escalation
和逻辑 compute-task derivation 属于 scheduler dispatch 之前的阶段。

Dirty control lane 不是 dirty-feature-specific scheduler queue。Dirty node 通过串行 control
path 更新图级 dirty lifecycle 和 ROI state；dispatcher 再从该状态 materialize dirty work
generation，并且只把具体 ready task callback 提交给 scheduler。Scheduler 实现应根据通用
ready-task metadata 做决策，例如 epoch、dirty generation 和 optional scheduler-specific
priority hint。Scheduler 可以通过 epoch drop stale queued work，可以使用 FIFO/LIFO 或
work-stealing queue，可以路由 CPU/GPU resource，也可以保留旧 work 继续运行，但它不应接收
task graph，也不应需要专门的 dirty-source queue。

compute-service 路径会先推导 planned work，再进入 scheduler dispatch，并且不再暴露绕过
planned-task dispatch 的兼容计算路径。

## Epoch 与取消

运行时和调度器队列使用 epoch 取消陈旧排队工作。Epoch `0` 被视为不可取消兼容工作。新的交互式调度应分配真实 epoch，使过时 RT 工作可以被丢弃。

## 可观测性

`GraphRuntime::SchedulerEvent` 记录分配、节点执行和 tile 执行事件。这有助于验证调度器行为。

## 开发方向

- 保持 `IScheduler` 作为正式公共调度器接口。
- 保持 planned parallel work 通过 scheduler-owned task runtime 路由。
- 避免新增对 `GraphRuntime` 内部队列的永久依赖。
- 保持 scheduler runtime 为 ready-task-only：它们接收带 epoch/generation metadata 和 optional
  scheduler-specific hint 的具体 callback，而不是 task graph 或 dirty work-set state。
- 保持插件调度器生命周期与 `Plugin-ABI.md` 兼容。
- 继续后续 richer annotated task pool、planner plugin ABI 和 scheduler policy metadata
  工作，但不把图级 dirty planning 移入 scheduler。
