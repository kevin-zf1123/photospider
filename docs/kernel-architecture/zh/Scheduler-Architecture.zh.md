# 调度器架构

内核具有正式的调度器目标接口和遗留运行时队列。本文档定义如何理解当前实现。

## 正式目标：IScheduler

`IScheduler` 是正式调度器接口。调度器附着到 `GraphRuntime`，启动 worker 资源，调度计算，并干净关闭。

核心生命周期：

```text
create -> attach(runtime) -> start -> schedule(...) -> shutdown -> detach
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

## 当前迁移状态

实现中仍在 `GraphRuntime` 直接包含 worker 队列、epoch 和任务提交 API。某些计算路径仍调用 `ComputeService::compute_parallel` 并直接向这些队列提交任务。

这些运行时队列应被视为迁移支持，而不是永久调度器 API。新的面向调度器设计应以 `IScheduler` 为目标。

## 内置调度器

| 类型 | 含义 |
| --- | --- |
| `cpu_work_stealing` | 多线程 CPU 调度器。 |
| `serial_debug` | 确定性单线程调试调度器。 |
| `gpu_pipeline` | 异构 CPU/GPU 调度器。 |
| `heterogeneous` | `gpu_pipeline` 的别名。 |

## 节点级调度

`IScheduler` 包含节点级调度钩子，例如 `schedule_node`、`schedule_nodes` 和任务组聚合 helper。
这些接口是迁移接口。现有实现仍可能在 scheduler 内部将 ROI 拆成 tile 或聚合 task group，
但这不是目标职责边界。

目标模型是：

```text
DirtyRegionPlanner
  -> DirtyRegionSnapshot
  -> ComputeTaskPlanner
  -> intent-aware task pools
  -> Scheduler resource dispatch
```

设备选择、队列选择、批处理、worker reservation、取消和资源专用 dispatch 属于 scheduler。
Dirty propagation、node/tile 展开、monolithic dirty escalation 和逻辑 compute-task derivation
属于 scheduler dispatch 之前的阶段。

默认实现可能 fallback 到遗留 `schedule` 路径。

## Epoch 与取消

运行时和调度器队列使用 epoch 取消陈旧排队工作。Epoch `0` 被视为不可取消兼容工作。新的交互式调度应分配真实 epoch，使过时 RT 工作可以被丢弃。

## 可观测性

`GraphRuntime::SchedulerEvent` 记录分配、节点执行和 tile 执行事件。这有助于在迁移期间验证调度器行为。

## 开发方向

- 保持 `IScheduler` 作为正式公共调度器接口。
- 随时间推移将 planned 或 annotated task 路由到调度器实例。
- 避免新增对 `GraphRuntime` 内部队列的永久依赖。
- 保持插件调度器生命周期与 `Plugin-ABI.md` 兼容。
- 在完整 planned-task routing 被视为完成前，将 scheduler-local ROI splitting 和 task-group
  planning decision 迁移到 `DirtyRegionPlanner`、`ComputeTaskPlanner` 或 task annotation。
