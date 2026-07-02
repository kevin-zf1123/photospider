# 调度器架构

内核具有用于 intent-aware 资源 dispatch 的正式调度器接口。本文档定义 planned parallel work
已经通过 scheduler-owned task runtime 路由之后，如何理解当前实现。

## 正式目标：IScheduler

`IScheduler` 是正式调度器接口。调度器附着到 `GraphRuntime`，启动 worker 资源，调度计算，并干净关闭。

核心生命周期：

```text
create -> attach(runtime) -> start -> dispatch planned tasks -> shutdown -> detach
```

`GraphRuntime` 拥有已注册 scheduler 的生命周期顺序。Scheduler 必须先 attach
到 runtime，再启动；`GraphRuntime::start()` 会启动此前已注册的 scheduler，
`GraphRuntime::set_scheduler()` 在 runtime 已运行时只会在 attach 之后启动新
scheduler，`GraphRuntime::stop()` 会关闭已注册 scheduler。`Kernel` 的
bootstrap 代码必须通过 `GraphRuntime` 注册 scheduler，而不是预先启动 scheduler。
Scheduler 关闭时必须先在 idle worker wait 和 completion wait 使用的同一同步保护下发布
stop state，再通知这些 waiter。这样可以避免一个 worker 在批次最后一个任务完成后正要进入
condition-variable sleep 时，shutdown 丢失唤醒。

按 `ComputeIntent` 路由计算：

| 意图 | 预期调度器角色 |
| --- | --- |
| `GlobalHighPrecision` | 面向吞吐的 HP 计算。 |
| `RealTimeUpdate` | 低延迟交互式更新。 |

`GraphRuntime` 保存以 `ComputeIntent` 为 key 的调度器映射。

长期 scheduler 职责是 planning 之后的资源 dispatch。Scheduler 应从 intent-aware task pool
中拉取 planned 或 annotated task，选择队列顺序、批处理、worker 策略、取消和具体执行资源，
然后分发工作。它们不应拥有图级 dirty-region propagation 或 compute-task derivation。

在当前 `DirectGraphCommit` 路径中，一个 `RealTimeUpdate` 请求仍会为同一脏区执行 HP 和 RT dirty
work，但 `IntentUpdateCoordinator` 会先运行 HP sibling，再运行 RT sibling。RT 路径会把代理写入
stage 到 `RealtimeDirtyWriteBuffer`，并在 RT dirty work drain 后提交。跨 intent 的 HP/RT
sibling 并发是后续 buffered-commit 能力，不是 scheduler workaround。Scheduler 应继续只负责
ready task callback 的资源 dispatch、epoch、取消和队列策略，不要让 RT 输出成为正式 HP 缓存来源。

## 当前 Dispatch 状态

并行计算规划和 plan execution 属于 `ComputeService` 协作者：`FullTaskGraphExpander`、
`NodeCacheTaskGraphPruner`、`DirtyRegionPlanner`、`DirtySnapshotTaskGraphPruner`、
`IntentUpdateCoordinator` 和 `ComputeTaskDispatcher`。裁剪完成后，`ComputeTaskDispatcher`
会把 node/cache-pruned task graph 或 dirty 裁剪后的 update work set materialize 为具体 task，
并通过相关 `ComputeIntent` 配置的 `IScheduler` 实例，经由 `SchedulerTaskRuntime` 提交 ready work。

`GraphRuntime` 拥有图状态、`GraphStateExecutor`、scheduler 注册、事件和平台资源。它不再暴露通用
worker queue、task graph 或 completion-counter API。Graph-state operation 和会修改可见
`GraphModel` 的 compute request 都使用 `GraphStateExecutor`，包括 scheduler-backed parallel
compute。新的 scheduler-facing 设计应扩展 `IScheduler` 与 `SchedulerTaskRuntime`；它不应通过 runtime model direct access 绕过 graph-state access。

## 内置调度器

| 类型 | 含义 |
| --- | --- |
| `cpu_work_stealing` | 多线程 CPU 调度器。 |
| `serial_debug` | 确定性单线程调试调度器。 |
| `gpu_pipeline` | 异构 CPU/GPU 调度器。 |
| `heterogeneous` | `gpu_pipeline` 的别名。 |

## 插件发现与图调度器选择

`graph_cli` 会在加载配置后、加载 graph 前扫描 `scheduler_dirs`。该扫描只会让 scheduler factory
发现 plugin 提供的 scheduler 类型，不会改变某个 graph 已经选择的 scheduler 实例。

新加载的 graph 会从 `Kernel::SchedulerConfig` 接收 scheduler 实例；该配置由
`scheduler_hp_type`、`scheduler_rt_type` 和 `scheduler_worker_count` 填充。因此，plugin
scheduler 只有在这些配置键命名了已扫描的 plugin 类型时才会生效，或者在用户随后对当前 graph
运行 `scheduler set <hp|rt> <type>` 后生效。`scheduler plugins` 报告的是发现状态；
`scheduler get all` 报告的是当前 graph 实际挂载的 HP/RT scheduler 实例。

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
planned-task dispatch 的兼容计算路径。空 planned dispatch 只有在目标已经拥有可复用 HP 输出时才是合法的；否则它是 planning contract error。

## Epoch 与取消

调度器队列使用 epoch 取消陈旧排队工作。Epoch `0` 被视为不可取消兼容工作。新的交互式调度应分配真实 epoch，使过时 RT 工作可以被丢弃。

## 可观测性

`GraphRuntime::SchedulerEvent` 记录分配、节点执行和 tile 执行事件。这有助于验证调度器行为。

## 开发方向

- 保持 `IScheduler` 作为正式公共调度器接口。
- 保持 planned parallel work 通过 scheduler-owned task runtime 路由。
- 保持 graph-state 命令和可见 graph compute request 位于 `GraphStateExecutor` 边界内。
- 保持 scheduler runtime 为 ready-task-only：它们接收带 epoch/generation metadata 和 optional
  scheduler-specific hint 的具体 callback，而不是 task graph 或 dirty work-set state。
- 保持插件调度器生命周期与 `Plugin-ABI.md` 兼容。
- 继续后续 richer annotated task pool、planner plugin ABI 和 scheduler policy metadata
  工作，但不把图级 dirty planning 移入 scheduler。
