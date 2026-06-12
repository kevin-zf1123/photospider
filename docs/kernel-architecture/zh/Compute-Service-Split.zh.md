# ComputeService 拆分计划

本文档记录 `ComputeService` 的就地拆分。第一轮拆分已经在现有公开 facade 后落地。
仍标记为 TODO 的项目，是有意推迟到后续 scheduler、traversal、cache migration
或 planner plugin change 中处理的工作。

## 当前问题

`ComputeService` 是公开计算入口，但它的实现同时包含依赖解析、缓存策略、
monolithic 与 tiled 分发、脏区状态规划、compute task 推导、scheduler task-runtime
dispatch 协调、计时、benchmark 事件和输出 debug 元数据。

拆分应先保持行为不变。它不是重写图引擎，也不是插件 ABI 变更。

## 目标形态

```text
ComputeService facade
  -> ComputeCachePolicy
  -> NodeInputResolver
  -> NodeExecutor
  -> DirtyRegionPlanner
  -> DirtyRegionSnapshot
  -> ComputeTaskPlanner
  -> IntentUpdateCoordinator
  -> ComputePlanExecutor
  -> ComputeMetricsRecorder
```

## 计划边界

| 边界 | 职责 | 状态 |
| --- | --- | --- |
| `ComputeService` facade | 保留当前公开 compute 入口并构造内部协作者。 | 已实现 |
| `ComputeCachePolicy` | 集中 HP/RT 缓存选择。 | 已在 `src/kernel/services/compute-service/compute_cache_policy.*` 实现 |
| `NodeInputResolver` | 构建运行时参数并收集已就绪图像输入。 | 已在 `src/kernel/services/compute-service/node_input_resolver.*` 实现 |
| `NodeExecutor` | 一致地执行 monolithic 与 tiled 算子。 | 已在 `src/kernel/services/compute-service/node_executor.*` 实现 |
| `DirtyRegionPlanner` | 基于 node-local dirty report 和算子 propagation 构建图级脏区状态。 | 已在 `src/kernel/services/compute-service/dirty_region_planner.*` 实现 |
| `DirtyRegionSnapshot` | 使用稳定 id 而不是原始指针枚举 dirty tiles、dirty monolithic nodes、per-node dirty ROI 和 per-edge ROI mapping。 | 已作为内部 snapshot model 实现 |
| `ComputeTaskPlanner` | 将 compute request 和 dirty snapshot 转换为共享 `ComputePlan` / `ComputeTaskGraph` 语义。 | 已作为内部规划边界实现；plugin ABI 仍是 TODO |
| `IntentUpdateCoordinator` | 协调 `GlobalHighPrecision` 与 `RealTimeUpdate` intent 语义，包括与执行模式无关的 realtime HP/RT 双路径行为。 | 已在 `src/kernel/services/compute-service/intent_update_coordinator.*` 实现 |
| `ComputePlanExecutor` | 执行 `ComputeTaskPlanner` 的 plan 语义：materialize task-graph work，通过 `SchedulerTaskRuntime` dispatch ready task，并提交结果。 | 已在 `src/kernel/services/compute-service/compute_plan_executor.*` 实现 |
| `ComputeMetricsRecorder` | 集中事件、计时、benchmark 事件和 debug 元数据。 | 已在 `src/kernel/services/compute-service/compute_metrics_recorder.*` 实现 |

## 缓存规则

拆分必须保留现有缓存契约：

- `cached_output_high_precision` 是唯一正式可复用缓存。
- `cached_output_real_time` 是临时交互式状态。

正式 HP 写入和磁盘缓存权威都通过 `cached_output_high_precision` 处理；RT 输出不会被提升为可复用缓存权威。

## 脏区边界

Dirty region 来自 node-local change，但 propagation 语义是算子契约。算子应显式定义
dirty 和 forward propagation 行为，并可使用节点参数、空间元数据、缓存依赖信息或
data-dependent LUT。当前 identity propagation fallback 是迁移支持，不应被视为新算子的充分行为。

`DirtyRegionPlanner` 已经为当前 HP 和 RT dirty update 路径拥有图级 dirty-region state。
它通过使用稳定 node id、tile 坐标、pixel ROI、graph generation 元数据和 edge mapping
的 `DirtyRegionSnapshot` 暴露状态。`ComputeService` 会在 graph 上保存 inspection 摘要，
`InteractionService` 会把该摘要暴露给 frontend/debug 查询。

TODO：在 frontend 具备具体 mask/tile 渲染契约后，添加更丰富的 dirty snapshot 可视化。

## Compute Task Planning 边界

单线程和并行计算应共享一个逻辑 `ComputePlan` 或 `ComputeTaskGraph`。`ComputeTaskPlanner`
消费 compute request 和 dirty snapshot，然后产生内部 `ComputePlan` 语义，供 sequential、
parallel、HP 和 RT 路径在具体执行分发前共同使用。执行模式只应在 task pool、scheduler
policy 和资源选择上不同。
只要该 planner 边界仍保持内聚，就不额外引入 task-graph composer。

Realtime HP/RT 双路径选择不是执行模式。Non-realtime 请求只启用 HP 路径。`RealTimeUpdate`
请求会针对 dirty ROI 同时启用 HP 和 RT 工作，无论调用方选择 single-threaded、parallel、GPU
还是其他 scheduler/resource policy。当前实现通过 `IntentUpdateCoordinator` 协调这一点；
parallel 执行会将 HP 与 RT sibling work 提交到各自 intent-specific scheduler task runtime，
而 single-threaded 执行会 inline 运行同一份 intent 工作。

HP 与 RT 路径会分别调用共享的 `ComputeTaskPlanner`。HP 路径基于自己的 HP dirty snapshot
创建 `GlobalHighPrecision` plan，RT 路径基于自己的 RT dirty snapshot 创建 `RealTimeUpdate`
plan。单次 `ComputeTaskPlanner` 调用不得同时发出 HP 和 RT 两份 task pool。未来扩展新的
task pool 时也应保持这种按 domain 分别调用 planner 的模式。

TODO：planner plugin ABI 继续明确推迟到后续 change。

## 调度器边界

当前并行计算路径会通过请求 `ComputeIntent` 选中的 scheduler dispatch 已经 planned 的图工作。
`ComputePlanExecutor` 拥有 compute-plan execution、内部 DAG counter、临时结果存储、
tile micro-task accounting、异常传播和最终输出选择，但会把具体任务执行交给
`SchedulerTaskRuntime`。

Scheduler 应从 intent-aware task pool 拉取 planned 或 annotated task，或通过
`SchedulerTaskRuntime` 接收 planned work，然后调度计算资源。它们不应拥有图级 dirty
propagation 或 compute-task derivation。Compute-planning helper 已经从正式 scheduler 表面移除。

目标 task-pool 模型拥有分离的 HP 和 RT pool，并且可以分别选择 scheduler 配置。例如，HP 可以使用
single-thread scheduler，RT 可以使用 GPU scheduler。Realtime 与 non-realtime 模式也可以使用不同的
scheduler 配置。Planned parallel work 现在会在 compute-service planning 之后到达
scheduler-owned task runtime。后续 scheduler-focused 工作可以增加 richer annotated task pool、
planner plugin ABI 和更详细的 scheduler policy metadata。

## Traversal 边界

脏区规划现在直接消费收窄后的拓扑边界和传播边界。`GraphTraversalService` 只负责拓扑，通过
`GraphModel` 邻接索引提供遍历顺序和显式的上游/下游拓扑查询。`DirtyRegionPlanner` 使用
`RoiPropagationService` 计算上游 ROI 需求，并使用 `GraphExtentResolver` 进行 HP 权威范围解析。
Traversal 不再暴露 ROI 投影、上游 ROI 计算、依赖树格式化，也不保留已移除 API 的兼容 wrapper。

Dependency-tree inspect 是结构化的：`GraphInspectService` 基于 topology adjacency 构建 dependency-tree snapshot，`Kernel`/`InteractionService` 返回这些 snapshot，CLI/TUI/frontend code 再渲染人类可读文本。

## 交互边界

`InteractionService` 是 CLI/TUI/frontend 与 kernel 之间面向前端的 facade。在 dirty-region
语境中，它应暴露图级 dirty snapshot 检查和可视化 API。它不是 dirty-region generation 或
propagation 的权威来源。

`InteractionService` 现在已经暴露 dirty snapshot debug 摘要，供 inspection 使用。它也会暴露结构化 dependency-tree 和 graph-inspection snapshot，使 frontend 可以先解析图结构，再选择展示格式。

TODO：在图级 dirty state 具备 frontend 展示契约后，添加更丰富的可视化 API。

## Global HP Dirty ROI

当前 global HP compute 带 dirty ROI 时，在某些入口路径中仍可能触发完整重算。
本次拆分应记录该行为，并避免意外改变它。

TODO：在后续 change 中决定 global HP dirty ROI 是否应使用优化后的局部 HP 更新规划。

## 验证预期

每个抽取步骤都应在从 `compute_service.cpp` 删除重复逻辑前完成聚焦验证。

必需回归区域：

- 缓存语义
- propagation 契约
- dirty-region tiled computation
- 调度器行为
- kernel contracts

OpenSpec change `split-compute-service` 持有实现工作的详细任务清单。
