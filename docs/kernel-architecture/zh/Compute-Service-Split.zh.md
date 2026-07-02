# ComputeService 拆分计划

本文档记录 `ComputeService` 的就地拆分。第一轮拆分已经在现有公开 facade 后落地。
仍标记为 TODO 的项目，是有意推迟到后续 scheduler、traversal、cache migration
或 planner plugin change 中处理的工作。

## 当前问题

`ComputeService` 是公开计算入口，但它的实现同时包含依赖解析、缓存策略、
monolithic 与 tiled 分发、脏区状态规划、compute task 推导、scheduler task-runtime
dispatch 协调、计时、benchmark 事件和输出 debug 元数据。

拆分应先保持行为不变。它不是重写图引擎，也不是插件 ABI 变更。

compute facade 现在在公开边界和 service 边界都使用结构化请求对象。
`Kernel::ComputeRequest` 从 frontend 承载 graph name、cache、execution、telemetry、
intent 和 dirty ROI 控制项。`ComputeService::Request` 只接收 target node、cache、
telemetry、intent 和 dirty ROI 数据，而 `ComputeService::ExecutionStrategy` 承载 runtime
和 scheduler-backed execution policy。这样可以保持 CLI 行为稳定，同时避免继续扩展长的
位置式 boolean 参数列表。

## 目标形态

```text
ComputeService facade
  -> ComputeCachePolicy
  -> NodeInputResolver
  -> FullTaskGraphExpander
  -> FullTaskGraph
  -> NodeCacheTaskGraphPruner
  -> ComputePlan / pruned ComputeTaskGraph
  -> ComputeDispatchPlanBuilder
  -> TaskPopulationStrategy / task dependency population helpers
  -> DirtyRegionPlanner
  -> DirtyRegionSnapshot
  -> DirtySnapshotTaskGraphPruner
  -> IntentUpdateCoordinator
  -> ComputeTaskDispatcher
  -> TaskSubmissionPlan / dispatch_planned_tasks
  -> DirtyUpdateWorkSet
  -> NodeExecutor
  -> ComputeMetricsRecorder
```

## 计划边界

| 边界 | 职责 | 状态 |
| --- | --- | --- |
| `ComputeService` facade | 接收结构化 compute request，保留既有行为，并构造内部协作者。 | 已实现 |
| `ComputeCachePolicy` | 集中 HP/RT 缓存选择。 | 已在 `src/kernel/services/compute-service/compute_cache_policy.*` 实现 |
| `NodeInputResolver` | 构建运行时参数并收集已就绪图像输入。 | 已在 `src/kernel/services/compute-service/node_input_resolver.*` 实现 |
| `NodeExecutor` | 一致地执行 monolithic 与 tiled 算子。 | 已在 `src/kernel/services/compute-service/node_executor.*` 实现 |
| `DirtyRegionPlanner` | 基于 node-local dirty report 和算子 propagation 构建图级脏区状态。 | 已在 `src/kernel/services/compute-service/dirty_region_planner.*` 实现 |
| `DirtyRegionSnapshot` | 使用稳定 id 而不是原始指针枚举 dirty tiles、dirty monolithic nodes、per-node dirty ROI 和 per-edge ROI mapping。 | 已作为内部 snapshot model 实现 |
| `FullTaskGraphExpander` | 将原始 graph 展开为一个 compute domain 的完整 node/tile `FullTaskGraph`，不使用请求目标、cache 状态或 dirty snapshot。 | 已在 `src/kernel/services/compute-service/task_graph_planning.*` 实现 |
| `NodeCacheTaskGraphPruner` | 将 `FullTaskGraph` 裁剪到请求目标/依赖锥，并记录所选节点的 cache 可用性。 | 已在 `src/kernel/services/compute-service/task_graph_planning.*` 实现 |
| `ComputeDispatchPlanBuilder` | 构建并记录 scheduler-backed dispatcher execution 使用的 cache-pruned high-precision plan。 | 已在 `src/kernel/services/compute-service/compute_dispatch_plan_builder.*` 实现 |
| `TaskPopulationStrategy` 和 task population helpers | 填充 graph-backed 或 graphless planned task record 以及 task dependency；dirty snapshot 不得用来创建新的 task shape。 | 已在 `src/kernel/services/compute-service/task_population_strategy.*` 和 `task_graph_planning.*` 实现 |
| `DirtySnapshotTaskGraphPruner` | 将 `DirtyRegionSnapshot` 应用于 node/cache-pruned `ComputeTaskGraph`，并 materialize 活跃的 `DirtyUpdateWorkSet`。 | 已在 `src/kernel/services/compute-service/task_graph_planning.*` 实现；plugin ABI 仍是 TODO |
| `IntentUpdateCoordinator` | 协调 `GlobalHighPrecision` 与 `RealTimeUpdate` intent 语义，包括与执行模式无关的 realtime HP/RT 双路径行为。 | 已在 `src/kernel/services/compute-service/intent_update_coordinator.*` 实现 |
| `ComputeTaskDispatcher` | 执行 node/cache-pruned task graph 语义：收集 source task、检查 task-graph readiness、通过 `SchedulerTaskRuntime` dispatch ready task，并提交结果。 | 已在 `src/kernel/services/compute-service/compute_task_dispatcher.*` 实现 |
| `TaskSubmissionPlan` 和 `dispatch_planned_tasks` | 将 cache-pruned plan 转为一次 dispatcher 调用所需的 scheduler closure、dependency counter、ready handle 和 empty-plan validation。 | 已在 `src/kernel/services/compute-service/compute_task_submission.*` 实现 |
| `ComputeMetricsRecorder` | 集中事件、计时、benchmark 事件和 debug 元数据。 | 已在 `src/kernel/services/compute-service/compute_metrics_recorder.*` 实现 |

`NodeExecutor` 将 tiled 输入准备保持在 per-tile 循环之外。`TiledInputNormalizer`
helper 会在每次节点调用时一次性 materialize image_mixing secondary 输入的 resize/crop
和通道转换，然后 `NodeExecutor` 复用这个 normalized context，为每个 tile task 构造只读
`InputTile` view 和可写 `OutputTile` view。这个边界避免把上游 `NodeOutput` buffer cast
成可变 tile 输入，也避免整图输入归一化在每个 tile 上重复执行。

## 缓存规则

拆分必须保留现有缓存契约：

- `cached_output_high_precision` 是唯一正式可复用缓存。
- `RealtimeProxyGraph` 在 `GraphModel` 之外持有临时 RT 交互式状态。

正式 HP 写入和磁盘缓存权威都通过 `cached_output_high_precision` 处理；RT 输出不会被提升为可复用缓存权威。

## 脏区边界

Dirty region 来自 node-local change，但 propagation 语义是算子契约。算子应显式定义
dirty 和 forward propagation 行为，并可使用节点参数、空间元数据、缓存依赖信息或
data-dependent LUT。当前 identity propagation fallback 是迁移支持，不应被视为新算子的充分行为。

`DirtyRegionPlanner` 已经为当前 HP 和 RT dirty update 路径拥有图级 dirty-region state。
它通过使用稳定 node id、tile 坐标、pixel ROI、graph generation 元数据和 edge mapping
的 `DirtyRegionSnapshot` 暴露状态。`ComputeService` 会在 graph 上保存 inspection 摘要，
`InteractionService` 会把该摘要暴露给 frontend/debug 查询。

`DirtyRegionSnapshot` 是与 graph topology 同级的图级状态，不是 scheduler graph。它记录当前
dirty 事实，用于从请求级 `ComputeTaskGraph` 中激活或裁剪 work。

Dirty-region lifecycle 与 compute triggering 分离。Dirty signal 是 node-originated：
frontend interaction 可以输入给某个 node，compute 也可以让某个 node 发现 dirty state，但图级
snapshot 必须通过该 node 的 begin/update/end dirty-region lifecycle 更新。Dirty signal 不应自动
enqueue compute request。这样，画笔笔画之类的交互可以通过它的 node 累积或持续流式更新 ROI，而不需要为每个盖印事件重建完整 compute plan。

Dirty lifecycle update 应通过 dirty-state/executor 边界拥有的串行 `DirtyControlLane`。
Control lane 更新 `dirty_source_nodes` 和 source lifecycle state。`dirty_updating_count`
从该 lifecycle state 派生，propagator 再从 source set 派生传播后的 `actual_dirty_region`
snapshot，然后唤醒 materialization。它不应被建模为 scheduler-owned compute task queue，也不应下放成 node-local compute ownership。

当前面向 production 的 lifecycle 写入通过 `Kernel` 和 `InteractionService` 的 begin/update/end dirty source
方法暴露。它们通过 `GraphStateExecutor` 串行化 graph state mutation，并将 node/frontend lifecycle
event 路由到 `DirtyControlLane`。该 lane 复用 `DirtyRegionPlanner` 更新 membership、lifecycle
和 actual dirty ROI，并把 wakeup/cutoff 决策返回给 facade。Frontend event subscription
和可复用 request-plan coalescing 仍是后续工作。

当前约束：`DirtyControlLane` 不拥有也不缓存 `ComputeTaskGraph`。它记录图级 dirty lifecycle state
和 wakeup intent；每次 compute request 仍在 compute-service planning 边界内从当前 snapshot
物化 work。跨多代 dirty generation 复用同一个 immutable request plan 是后续优化，不是
scheduler 职责。

TODO：在 frontend 具备具体 mask/tile 渲染契约后，添加更丰富的 dirty snapshot 可视化。

## Compute Task Planning 边界

Task graph planning 被拆成明确的 expansion 和 pruning 边界。`FullTaskGraphExpander`
会把原始 graph 展开成一个 compute domain 的完整 node/tile task graph。该完整展开不依赖
请求目标、node cache 状态或 dirty snapshot；它只回答“这个 graph 和 domain 中存在哪些可执行
node/tile task”。

当前没有一个单一 planner 类拥有全部 plan 创建职责。当前实现是一条模块链：
`ComputeDispatchPlanBuilder` 推导请求 traversal 并记录 high-precision plan，
`FullTaskGraphExpander` 枚举完整 domain 的 task shape，`NodeCacheTaskGraphPruner`
将其收窄到请求/cache 锥，`TaskPopulationStrategy` 与 task dependency helper 填充可执行
task record，`DirtySnapshotTaskGraphPruner` 随后从已经裁剪的 graph 中激活 dirty work。

`GraphModel` 会按 topology generation、compute intent 和 task-shape 配置缓存 immutable
`FullTaskGraph` expansion。`force_recache` 请求会在 planning 前清空该 cache，因为输入数据或
source 参数可能在不改变 graph topology 的情况下改变输出 extent；tiled task ROI 必须基于当前
extent 重新展开，而不能复用旧 expansion。

`NodeCacheTaskGraphPruner` 消费该 `FullTaskGraph`、请求目标节点/依赖锥和当前 node/cache
状态，然后产出 sequential 与 parallel execution 使用的 pruned `ComputePlan` /
`ComputeTaskGraph`。它会记录所选节点的 cache 可用性，同时保留现有执行契约：cache hit 仍在
task execution 阶段解析。

`DirtySnapshotTaskGraphPruner` 是独立的 dirty-source pruner。它消费 node/cache-pruned
`ComputeTaskGraph` 和 `DirtyRegionSnapshot`，用 dirty metadata 标注已选择的 graph，并
materialize 活跃的 `DirtyUpdateWorkSet`。它可以裁剪或激活已经展开的 task，但不得创建新的 tile
或 node task shape。这样 full-frame tiled compute 与 HP/RT dirty update 会共享同一个 task model。

Realtime HP/RT 双路径选择不是执行模式。Non-realtime 请求只启用 HP 路径。`RealTimeUpdate`
请求会针对 dirty ROI 同时启用 HP 和 RT 工作，无论调用方选择 single-threaded、parallel、GPU
还是其他 scheduler/resource policy。当前实现通过 `IntentUpdateCoordinator` 协调这一点。
当两个 scheduler task runtime 都可用时，coordinator 会先启动 RT dirty sibling，再启动
HP sibling，先等待 RT，并通过 sibling commit gate 保证 HP 对 `GraphModel` 的修改发生在
RT proxy commit 之后。没有 scheduler runtime 时，同一批工作按 RT 后 HP 的顺序 inline 执行。

HP 与 RT 路径保持分离的 single-domain plan 和 dirty snapshot。HP 路径使用
`GlobalHighPrecision` plan，并通常从 HP dirty snapshot 中裁剪 HP work。当 HP dirty execution
是 forced 时，它会把 HP planning ROI 扩展到目标节点当前完整 HP extent，因为 HP staging buffer
不会从旧像素 seed。RT 路径使用 `RealTimeUpdate` plan，并从 RT dirty snapshot 中裁剪 RT work。
RT dirty node execution 会通过 `RealtimeProxyWriteBuffer` stage 代理输出，并且只在 RT work set drain 后提交到
`RealtimeProxyGraph`。HP dirty node execution 会通过 `HighPrecisionDirtyWriteBuffer` stage
HP 输出，并在 RT gate 打开后提交到 `GraphModel`。单次 full graph expansion 或 pruner pass
不得同时发出 HP 和 RT 两份 task pool。未来扩展新的 task pool 时也应保持这种按 domain 分别
expansion、pruning 和 commit 的模式。

TODO：planner plugin ABI 继续明确推迟到后续 change。

## 调度器边界

目标并行计算路径会通过请求 `ComputeIntent` 选中的 scheduler dispatch 已经 planned 的图工作。
`ComputeTaskDispatcher` 拥有 compute-plan execution、内部 DAG counter、临时结果存储、
tile micro-task accounting、异常传播和最终输出选择，但会把具体 ready task callback 交给
`SchedulerTaskRuntime`。

对于完整 high-precision parallel dispatch，`TaskSubmissionPlan` 会将 cache-pruned plan
转换为 dense node index、dependency counter、scheduler task handle、operation variant
和临时结果槽。`dispatch_planned_tasks` 随后提交初始 ready handle，并验证 empty plan。
Empty plan 只有在目标已经拥有可复用 high-precision 输出时才合法；未缓存目标的 empty plan
是 planning contract error，而不是递归顺序计算 fallback。

对于 dirty update，production HP 和 RT executor 会先从请求 plan 和 dirty snapshot 构建
request-local dirty `TaskExecutor` handle，然后调用公开静态
`ComputeTaskDispatcher::submit_dirty_ready_tasks_source_first` helper 作为 source-first
submission 边界。运行期依赖计数器、task 引用计数和 ready queue 在 compute-service dispatcher
state 内拥有。Scheduler 接收带 scheduler priority 和 completion accounting 的具体 ready task
handle；它不接收 task graph，不拥有 dirty-state lookup 或 task-graph derivation。

Realtime materialization 必须同时考虑当前 running work 和 dirty node lifecycle。如果 node
仍在创建 dirty region，ready queue 为空不应强制 realtime compute request 结束并为下一次 ROI
update 重建完整 plan。某个 dirty generation 的 source-node task 会由 dispatcher helper
source-first 提交，并在依赖的 downstream dirty work 释放前完成，而不是单独的 dirty source queue，
也不是 scheduler-wide priority contract。后续工作可以把 work-set selection 逻辑抽取到
task-pruner plugin interface 后面。

一旦 scheduler-visible task 已经从 `ComputeTaskGraph` 派生，dispatcher 必须把该 graph 视为
immutable。新的 dirty update 会基于同一个 plan 和最新 snapshot 产生新的
`DirtyUpdateWorkSet` generation，并在 work ready 时提交具体 ready task callback。Dispatcher
可以附带 generation、epoch metadata，以及 optional scheduler-specific hints，但不应在 scheduler
runtime 正在运行时并发析构并替换 task graph。

Scheduler 应从 intent-aware dispatcher path 接收 ready task callback，然后调度计算资源。它们
不应拥有 task graph、图级 dirty propagation、dependency counter、dirty work pruning 或
compute-task derivation。Compute-planning helper 已经从正式 scheduler 表面移除。

Scheduler 收到新 ready task 后的行为属于策略：它可以通过 epoch drop stale queued work，可以使用
FIFO/LIFO 或 work-stealing queue，可以选择 CPU/GPU resource，也可以在符合延迟/吞吐策略时让旧
work 完成。但 scheduler 的输入仍应是具体 ready task callback 以及 epoch、dirty generation 等
通用 metadata。
它不应需要 dirty-feature-specific queue。

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

CLI/REPL 前端是固定的批处理取向界面。它不暴露 RT intent 命令、dirty ROI 创建命令，
也不暴露 `compute rt`、`--dirty-roi`、`dirty begin`、`dirty update` 或 `dirty end`
这类 dirty source lifecycle 命令。`RealTimeUpdate` 和 dirty source lifecycle API
保留给 kernel/test 以及未来 GUI/WebUI 风格前端。

TODO：设计缺失的 node 到 `InteractionService` realtime dirty update 接口。该接口必须允许 node
提供 dirty-region lifecycle event、realtime update event 和 update request，同时保持
`InteractionService` 只是面向前端消费的 facade，而不是 dirty-region generation 或 compute
scheduling 的 owner。

TODO：在图级 dirty state 具备 frontend 展示契约后，添加更丰富的可视化 API。

## Global HP Dirty ROI

Global HP compute 带 dirty ROI 时会路由到 HP dirty planning。HP dirty update 接收 dirty ROI，
并通常从 high-precision task graph 中裁剪 HP work，使大图和大图像不需要为未受影响的 work
付出代价。如果 `force_recache=true`，executor 会在提交前改为规划完整目标 HP frame，因为
staging 从空输出开始，不能保留旧 HP cache 中 ROI 外的像素。coordinator 会为该路径记录
`intent_coordinator_global_dirty_update`。

## 验证预期

每个抽取步骤都应在从 `compute_service.cpp` 删除重复逻辑前完成聚焦验证。

必需回归区域：

- 缓存语义
- propagation 契约
- dirty-region tiled computation
- 调度器行为
- kernel contracts

OpenSpec change `split-compute-service` 持有实现工作的详细任务清单。
