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
  -> RoiPropagationService / GraphExtentResolver
```

`Kernel` 拥有多图 API。`GraphRuntime` 拥有一个图模型、每图 `GraphStateExecutor`、事件服务、平台 context 和调度器实例。

`InteractionService` 是面向前端的 kernel interaction facade。它的总体职责是将 CLI/TUI/frontend
命令与 kernel 内部解耦。在 dirty-region 语境中，它应暴露图级 dirty snapshot 查询和可视化 hook；
它不是 dirty-region generation 或 propagation 的权威来源。

frontend compute 命令现在构造 `Kernel::ComputeRequest`，不再把位置式 boolean flag
沿调用栈继续传递。`Kernel` 负责 graph lookup、runtime start、quiet-mode 与 skip-save
副作用、async scheduling、image extraction 和 LastError 映射。随后它把请求转换为
`ComputeService::Request`，后者只承载 node target、cache、telemetry、intent 和 dirty ROI
数据。parallel/runtime 选择则通过独立的 `ComputeService::ExecutionStrategy` 承载。

当前 CLI/REPL 前端是批处理取向。它不承诺提供 `compute rt` 或 `--dirty-roi` 这样的 realtime
update 交互命令。`RealTimeUpdate` 是面向未来 GUI/interaction 环境的 kernel intent，不应把 CLI
视为生产 realtime 控制面。

`GraphTraversalService` 现在只负责拓扑。它从 `GraphModel` 邻接索引提供遍历顺序和显式的上游/下游拓扑查询。脏区需求和 ROI 投影使用 `RoiPropagationService`，正式传播范围来自 `GraphExtentResolver`。

`ComputeService` 拆分后的目标 planning 流程会把请求级静态规划与每次 update 的 dirty work
选择分开：

```text
ComputeService facade
  -> GraphModel topology / GraphTraversalService queries
  -> FullTaskGraphExpander
  -> FullTaskGraph
  -> NodeCacheTaskGraphPruner
  -> ComputePlan / pruned ComputeTaskGraph
  -> DirtyRegionPlanner
  -> DirtyRegionSnapshot
  -> DirtySnapshotTaskGraphPruner
  -> DirtyUpdateWorkSet
  -> task pools / scheduler / execution resources
```

`FullTaskGraphExpander` 会把原始 graph 展开为一个 compute domain 的完整 node/tile task graph。
它不依赖请求目标、cache 状态或 dirty snapshot。`NodeCacheTaskGraphPruner` 随后把该 graph
裁剪到请求目标和依赖锥，并记录所选节点的 cache 可用性。Dirty update 会再经过独立的
`DirtySnapshotTaskGraphPruner`，用 dirty metadata 标注已选择的 graph，并产生活跃的
`DirtyUpdateWorkSet`。

单线程和并行执行应共享同一个已裁剪的 `ComputePlan` 或 `ComputeTaskGraph`。执行模式应在
task pool、scheduler policy 和执行资源上不同，而不是在图级 dirty propagation、完整 task
展开或 task-graph 裁剪上不同。

`ComputePlan` 是当前 compute request 和 domain 的静态分析结果。它在图状态稳定时推导，并在该
request 内作为 topology contract 保持不变；无论当前 commit policy 直接写入可见图状态，还是未来
commit policy 在提交前暂存 buffer，这一点都成立。Dirty update 不会重新定义拓扑语义。它们使用当前
`DirtyRegionSnapshot` 和 dirty ROI，从 plan 中为每条 HP 或 RT update 队列激活或裁剪
`DirtyUpdateWorkSet`。

请求 plan 必须枚举该请求可用的真实 compute task；当选择的实现是 tiled 时，也包括 tile task。
Dirty state 只从这个已枚举 task graph 中裁剪或激活 task。Dirty clipping 阶段不得展开新的 tile task；
这样 full-frame tiled parallelism 和 dirty ROI execution 会共享同一个 task model。

当 scheduler task 仍可能从某个 request 的 `ComputeTaskGraph` 派生并运行时，该 task graph 必须保持
immutable。连续 RT dirty update 会基于同一个 plan 和最新 dirty snapshot 创建新的
`DirtyUpdateWorkSet` generation，并在 work ready 时提交具体 ready task callback；不得在活跃
scheduler runtime 底下析构并替换 task graph。

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

顺序计算适合简单执行和调试。它现在会先创建与并行路径相同的内部完整展开和 node/cache
裁剪后的 task graph 语义，再执行递归路径。

## 并行计算

并行计算先展开完整 task graph，再从 `topo_postorder_from` 通过 `NodeCacheTaskGraphPruner`
裁剪得到 `ComputePlan`。随后它把 plan 中的 `ComputeTaskGraph` materialize 为 scheduler task，
跟踪依赖计数器，并通过已配置 scheduler 的 `SchedulerTaskRuntime` 提交 ready 节点任务。
Tiled 操作可能产生微任务并递增 scheduler-owned 完成计数器。

`ComputeTaskDispatcher` 将 plan execution、依赖计数、稀疏 node-id 映射、临时结果存储、事件日志、异常传播和最终目标选择保留在 compute-service 边界内。它通过 scheduler task-runtime queue
dispatch 已经 planned 的 work；它不会让 scheduler 拥有 dirty propagation、compute-task
derivation 或 task graph 本身。

对于 dirty 执行，dispatcher 应只 materialize 当前 dirty snapshot 从该 request 的
`ComputeTaskGraph` 中选出的活跃 `DirtyUpdateWorkSet`。运行期依赖计数器和 ready-task queue
是执行期产物；它们不存放在 `DirtyRegionSnapshot` 中，也不由 scheduler 拥有。

Dirty-region signal 是 node-originated 状态更新，不是 compute trigger。`DirtyRegionNode`
应暴露生命周期状态，例如开始创建 dirty region、用当前 ROI 更新 dirty region，以及结束创建 dirty
region。Frontend brush input 可以更新某个 node，computed node 也可以发现新的 dirty state，但 dirty
region 仍由图中的 node 发出。Compute request 可以在 dirty region 关闭后创建；活跃 realtime
request 也可以在 dirty node 仍在变化时合并更新。Dispatcher 的 realtime 截止条件必须综合
dirty-node lifecycle 和当前 running work；ready queue 为空本身不能证明交互已经结束。

Dirty-node lifecycle update 进入串行的 `DirtyControlLane`，该 lane 更新图级 dirty snapshot 中的
dirty source state，运行 propagation 刷新 `actual_dirty_region`，并把 wakeup/cutoff 决策返回给
`Kernel` / `InteractionService` facade。Scheduler 只接收带 epoch/generation metadata 和 optional
scheduler-specific hint 的 ready task callback；它不接收 task graph，不拥有 dirty control lane，
node 也不拥有 compute-service dirty queue。

## 图状态访问与提交策略

YAML 加载、cache 命令、inspection 和 ROI projection 等 graph-state operation
都是对可见 `GraphModel` 的操作。它们不是 compute-task dispatch，不应通过
`SchedulerTaskRuntime` 路由。

当前默认语义是通过 `GraphStateExecutor` 提供 per-graph exclusive access：同一个 graph 的
graph-state operation 和非并行 compute 不会并发读取或修改可见 `GraphModel`。这能在不把非
compute 命令路由到 scheduler queue 的情况下，保持 graph topology、cache 字段、dirty
snapshot、timing 和 node runtime state 一致。

后续可以新增独立于 `ComputeIntent` 的 `ComputeCommitPolicy`。`DirectGraphCommit`
保留当前行为：compute 在请求期间写入可见图状态，graph-state operation 等待。
未来的 `StagedInterruptibleCommit` 策略会把输出暂存在可见图状态之外，允许 graph-state
operation 在 commit 前请求取消，在取消时丢弃未提交 buffer，并且只提交一致的结果。
该策略有意不属于 `ComputeIntent`，因为 HP/RT intent 语义独立于提交和中断行为。

## GlobalHighPrecision

`GlobalHighPrecision` 是完整质量路径。没有 dirty ROI 时，它执行普通完整计算。带 dirty ROI 时，它会进入 HP dirty update 路径，而不是过去的完整重算 fallback。

HP 脏区更新是一等的 dirty-ROI 消费方，而不只是完整重算 fallback。它会计算反向 ROI 计划，
将脏区对齐到 HP tile 边界，从该 request 的 `ComputeTaskGraph` 中裁剪 HP work set，
更新受影响 HP tile，记录 HP ROI/版本元数据，并可调度 downsample 工作刷新 RT 临时状态。
`IntentUpdateCoordinator` 会把 global HP dirty request 路由到该路径，并记录
`intent_coordinator_global_dirty_update`。

Dirty-region state planning 现在通过图级 `DirtyRegionPlanner` 运行，产生的
`DirtyRegionSnapshot` 会输入 dirty work-set materialization，并提供给交互层 inspection 摘要。

## RealTimeUpdate

`RealTimeUpdate` 需要 dirty ROI。没有 `dirty_roi` 的请求是非法的，应通过内核和交互层 API 返回清晰错误。它不会隐式表示全帧 RT 更新。

带有效 dirty ROI 时，realtime 计算会启用两条路径。HP 更新受影响 graph 工作的完整尺寸权威输出，
RT 更新受影响区域的代理输出。当 scheduler task runtime 可用时，`IntentUpdateCoordinator`
会并发启动 HP 和 RT dirty sibling；每个 sibling 再把 ready dirty work 提交到各自
intent-specific scheduler runtime。Coordinator 会先等待 RT、再等待 HP，然后返回 RT 输出。
当选择单线程执行时，它仍会 inline 运行 HP 和 RT 工作。这个区别是执行模式选择，而不是启用或禁用
HP/RT 双路径的开关。

Realtime planning 有意按路径分别执行，而不是通过一次混合 domain 的 planner 调用生成两份任务池。
`IntentUpdateCoordinator` 会分发 sibling HP 与 RT update callback，并为 parallel Dirty RT
request 记录 scheduler-runtime submission、wait 和 completion 阶段。每条路径都使用一个
single-domain request plan 和同 domain 的 dirty snapshot：HP callback 使用
`GlobalHighPrecision` node/cache-pruned plan 与 HP dirty snapshot，RT callback 使用
`RealTimeUpdate` node/cache-pruned plan 与 RT dirty snapshot。Dirty snapshot 会从该路径的
task graph 中裁剪或激活 update work set。这样会把完整 task expansion、node/cache pruning
和 dirty snapshot pruning 保持为独立契约，使未来新增 task pool 或 task mode 时继续复用这些边界。

传入的 dirty ROI 会在当前请求中转换为图级 planner state。`Kernel` 和 `InteractionService` 已暴露
begin/update/end dirty source lifecycle 方法，使 frontend 或 node-facing 代码可以通过同一个图级 owner
写入 source lifecycle state。TODO：未来 frontend-driven dirty-region update 应由 node-local dirty report
作为 origin source。

TODO：设计 node 到 `InteractionService` 的 realtime dirty update 边界。该设计必须定义 node 如何发出
realtime event、dirty region 和 update request；哪一层负责 dirty-region generation；
node 与 interaction facade 的边界；以及未来 GUI 如何消费这些事件，同时不把 CLI 扩展成 realtime
interaction surface。

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
