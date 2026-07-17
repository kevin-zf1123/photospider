# 内核计算流程

本文档说明当前 compute request、planning、execution、HP/RT、commit、event 和 error 行为。
模块所有权概述位于 `Compute-Boundaries.zh.md`。

## 入口点

典型前端流程：

```text
CLI / TUI
  -> ps::Host
  -> embedded Host adapter
  -> InteractionService
  -> Kernel
  -> GraphRuntime
  -> ComputeService
  -> OpRegistry / GraphCacheService / GraphTraversalService
  -> RoiPropagationService / GraphExtentResolver
```

`Kernel` 拥有多图 API。`GraphRuntime` 拥有一个图模型、每图 `GraphStateExecutor`、事件服务、平台 context 和调度器实例。

`ps::Host` 是面向 frontend 的 public interface。embedded Host adapter 会复制 public
request/result value，并在内部使用 `InteractionService` wrapper 与 `Kernel`；CLI/TUI code
不会直接包含或调用这些 backend facade。在 dirty-region 语境中，Host 暴露图级 dirty snapshot
和 lifecycle value；`InteractionService` 仍是内部转换边界，不是 dirty-region generation 或
propagation 的权威来源。

frontend compute 命令构造 public `ps::HostComputeRequest`，不会把位置式 boolean flag
或内部 request type 穿过 public seam。embedded Host adapter 会把该 value 转换为
`Kernel::ComputeRequest`。`Kernel` 负责 graph lookup、runtime start、quiet-mode 与 skip-save
副作用、async scheduling、image extraction 和 LastError 映射。随后它把内部请求转换为
`ComputeService::Request`，后者只承载 node target、cache、telemetry、intent 和 dirty ROI
数据。parallel/runtime 选择则通过独立的 `ComputeService::ExecutionStrategy` 承载。

Dirty ROI 从 `HostComputeRequest` 复制到 `Kernel::ComputeRequest`，再经过 graph propagation、
planning、task selection、staged execution 与 `NodeExecutor` 时，始终保持为内核自有的
`PixelRect`；extent 使用 `PixelSize`。这条路径不会进行 OpenCV geometry 转换；provider 只有在
真实 matrix 或算法 call 处才能创建局部 OpenCV rectangle 或 size。

CLI/REPL 前端是固定的批处理取向界面。它不暴露 RT intent 命令、dirty ROI 创建命令，
也不暴露 `compute rt`、`--dirty-roi`、`dirty begin`、`dirty update` 或 `dirty end`
这类 dirty source lifecycle 命令。`RealTimeUpdate` 和 dirty source lifecycle API 可由 Host
与 kernel/test caller 使用，但 CLI 不暴露这些能力，也不应被视为生产 realtime 控制面。

Compute 不会按请求获取 scheduler 容量。Graph 发布前，load 会解析配置的 worker 请求（`0` 变为
`min(max(1, hardware_concurrency()), 8)`，显式 `1..8` 保持精确），规划 HP 与 RT scheduler
计费，并从进程级 32-slot ledger 原子预留二者的合计需求。已接受的 pair 为每个 scheduler owner
各含一个 move-only reservation，并跨越每个同步或异步 compute request 保持。只有内置
`serial_debug` 是零 slot scheduler；内置 CPU 与 ABI v2 plugin scheduler 按解析后的授权计费，
内置 GPU/heterogeneous scheduler 还要计入潜在 device worker。该准入步骤约束当前
scheduler-owned worker，而不是 callback 数，也不是 compute 及其 operation 使用的所有 thread。

`GraphTraversalService` 只负责拓扑。它从 `GraphModel` 邻接索引提供遍历顺序和显式的上游/下游拓扑查询。脏区需求和 ROI 投影使用 `RoiPropagationService`，正式传播范围来自 `GraphExtentResolver`。

当前 compute planning 流程会把请求级静态规划与每次 update 的 dirty work 选择分开：

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
  -> TaskSubmissionPlan / ComputeTaskDispatcher
  -> ready-task scheduler dispatch
```

`FullTaskGraphExpander` 会把原始 graph 展开为一个 compute domain 的完整 node/tile task graph。
它不依赖请求目标、cache 状态或 dirty snapshot。`NodeCacheTaskGraphPruner` 随后把该 graph
裁剪到请求目标和依赖锥，并记录所选节点的 cache 可用性。Dirty update 会再经过独立的
`DirtySnapshotTaskGraphPruner`，用 dirty metadata 标注已选择的 graph，并产生活跃的
`DirtyUpdateWorkSet`。

单线程和并行执行使用相同的 full-expansion 与 node/cache-pruning 语义。二者的 execution
mechanism 不同，而 graph traversal、dirty planning 与 task-graph pruning 仍由 compute 拥有。

`ComputePlan` 是当前 compute request 和 domain 的静态分析结果。它在图状态稳定时推导，并在该
request 内作为 topology contract 保持不变；无论当前路径直接写入可见图状态，还是使用现有
dirty-path staged buffer，这一点都成立。Dirty update 不会重新定义拓扑语义。它们使用当前
`DirtyRegionSnapshot` 和 dirty ROI，从 plan 中为每条 HP 或 RT update 队列激活或裁剪
`DirtyUpdateWorkSet`。

请求 plan 必须枚举该请求可用的真实 compute task；当选择的实现是 tiled 时，也包括 tile task。
Dirty state 只从这个已枚举 task graph 中裁剪或激活 task。Dirty clipping 阶段不得展开新的 tile task；
这样 full-frame tiled parallelism 和 dirty ROI execution 会共享同一个 task model。

当 scheduler task 仍可能从某个 request 的 `ComputeTaskGraph` 派生并运行时，该 task graph 保持
immutable。每个 dirty compute request 会基于自身 plan 与 snapshot 创建 generation-local
selection overlay，随后只推送 ready handle 或 callback。Scheduler 不会收到可替换的 task-graph
object。

执行粒度是另一层概念。选中的 operation implementation 可以是 node-wide、monolithic 或 tiled。
当前 full-task population 会把 `MICRO` metadata 映射为 16-pixel task，把 `MACRO` metadata 映射为
256-pixel task，未指定 preference 时映射为 128-pixel task。边界 task 会裁剪到 output extent。

Dirty snapshot grid 是不同 metadata：HP snapshot 在 64-pixel HP grid 上物化 Micro key，RT
snapshot 在 16-pixel proxy grid 上物化 Micro key。Dirty selector 会让这些 record 与 full graph
中已经存在的 task shape 相交，而不会创建 `ReTileTask`、RT Macro_64 record 或动态
Micro/Macro conversion。HP 与 RT plan 都保持 single-domain；realtime intent 通过分离的 sibling
work 协调二者，不会创建 cross-domain task dependency。

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
十六分之一。`IntentUpdateCoordinator` 会启动两个 sibling call，每个 sibling 再查找为自身 intent
route 注册的 per-graph scheduler。`ComputeIntent` 本身不指定 QoS 或 physical priority。

当前协作者职责与非所有权边界记录在 `Compute-Boundaries.zh.md`。

## 顺序计算

顺序计算使用递归依赖解析：

1. 验证目标节点。
2. 解析遍历顺序，并可选地清理缓存。
3. 对每个依赖递归计算上游节点。
4. 把静态 `ParameterMap` 复制到 `runtime_parameters`，并直接覆盖连接的命名
   `ParameterValue` output，期间不发生格式转换。
5. 为 HP 意图解析操作实现。
6. 执行 monolithic 或 tiled 操作。
7. 存储输出、发出事件、更新时间，并在启用时保存磁盘缓存。

顺序计算适合简单执行和调试。它会先创建与并行路径相同的内部完整展开和 node/cache
裁剪后的 task graph 语义，再执行递归路径。

## 并行计算

并行计算先展开完整 task graph，再从 `topo_postorder_from` 通过 `NodeCacheTaskGraphPruner`
裁剪得到 `ComputePlan`。`ComputeDispatchPlanBuilder` 会记录这个 cache-pruned plan 供检查使用。
`TaskSubmissionPlan` 随后把 plan 中的 `ComputeTaskGraph` materialize 为 scheduler closure、
dependency counter、ready handle、operation variant 和临时结果槽，并通过已配置 scheduler 的
`SchedulerTaskRuntime` 提交 ready 节点任务。Tiled 操作可能产生微任务并递增 scheduler-owned 完成计数器。

在提交这些 callback 前，选定 scheduler 已完成全生命周期准入。运行中的 compute 因而不会取得新的
ledger slot。Scheduler inspection 与 replacement 和 compute 共用 per-graph `GraphStateExecutor`
边界；replacement 不能与一串 compute callback 重叠，并且必须在旧 scheduler 保持存活时预留
candidate transient headroom。Candidate planning、attach 或 start 失败会保留旧 scheduler 及其
compute 行为，只归还 candidate 容量。

`ComputeTaskDispatcher` 将 plan execution、依赖计数、稀疏 node-id 映射、临时结果存储、事件日志、异常传播和最终目标选择保留在 compute-service 边界内。它通过 scheduler task-runtime queue
dispatch 已经 planned 的 work；它不会让 scheduler 拥有 dirty propagation、compute-task
derivation 或 task graph 本身。如果裁剪后的 planned dispatch 为空，但目标没有可复用 HP 输出，dispatcher 会报告 planning contract error，而不是回退到递归顺序计算。

对于 dirty 执行，`DirtySnapshotTaskGraphPruner` 只 materialize 当前 dirty snapshot 从 request
`ComputeTaskGraph` 中选出的 active `DirtyUpdateWorkSet`。Runtime dependency counter 与 ready-task
queue 是 execution artifact；它们不存放在 `DirtyRegionSnapshot` 中，也不由 scheduler 拥有。

显式 Host begin/update/end call 会指定 dirty source node 并更新 graph-scoped lifecycle fact，但
不会触发 compute。当前 backend 不存在 node subscription、automatic request launch 或
active-request coalescing contract。

Lifecycle update 会进入 per-graph serialized path 并调用 `DirtyControlLane`。该 lane 更新 source
state，为 event domain 重建 source-local derived record，再通过 `DirtyControlLaneResult` 返回
wakeup/cutoff hint。该 lifecycle path 不遍历 downstream edge，这些 hint 当前也没有 production
compute consumer。Embedded Host 暴露 copied inspection value，不暴露内部 control field。
Scheduler 只接收 pushed ready handle 或 callback，以及自身的 batch epoch 与 priority hint；dirty
generation 不会作为 scheduler epoch 转发。

## 图状态访问与提交策略

YAML 加载、cache 命令、inspection 和 ROI projection 等 graph-state operation
都是对可见 `GraphModel` 的操作。它们不是 compute-task dispatch，也不会通过
`SchedulerTaskRuntime` 路由。

当前默认语义是通过 `GraphStateExecutor` 提供 per-graph exclusive access：同一个 graph 的
graph-state operation 和 compute request 不会并发读取或修改可见 `GraphModel`。这也包括 scheduler-backed parallel compute：外层 Kernel request 进入 `GraphStateExecutor`，而 ready node/tile callback 在该边界内通过 scheduler runtime dispatch。这能在不把非 compute 命令路由到 scheduler queue 的情况下，保持 graph topology、cache 字段、dirty snapshot、timing 和 node runtime state 一致。

Scheduler 与 required-session lifetime 会配合这一边界进行协调。同步/异步 compute、scheduler
information、scheduler replacement、required graph save、node-YAML replacement、ROI
projection 的 graph-state 部分，都由 per-graph executor 串行化。Required-session lifetime
admission 还会覆盖 timing inspection 与 all-cache clearing，直至完成 public result/status
translation；这些调用本身不会引入新的 scheduler task boundary。Embedded close 期间，Host 会先
发布 close marker，并让该 marker 之前已准入的同步调用完成 public translation；graph-state user
会在 lane 仍 accepting 时完成 submission。
随后 Kernel 会停止 lane admission，Host 才等待 async submission placeholder 与 status
publication。这样不需要 FIFO 出现空位，就能唤醒被满队列阻塞的 producer；此前已准入的 callback
仍会排空，之后 Kernel 才 join executor 并调用 runtime stop。Runtime startup 可能发生在
graph-state submission 之前；Embedded Host 会让完整调用先通过 close admission，因此 close 无法
在 startup 期间或 graph-state completion 前删除 runtime。Node replacement 与 ROI projection
还会在同一个 work item 内完成 required-node lookup 与 operation，避免 clear/reload 造成
check-then-act gap。Scheduler information 会在离开边界前复制 name/statistics，不会让 raw
scheduler pointer 逃逸。

当前 dirty update 为 HP/RT sibling 安全使用 staged output commit：HP dirty worker 写入
`HighPrecisionDirtyWriteBuffer`，并且只在 RT sibling 已提交之后写入可见 `GraphModel`；
RT dirty worker 写入 `RealtimeProxyWriteBuffer`，并提交到 runtime-owned
`RealtimeProxyGraph`。当前实现没有通用 graph-revision 或 interruptible commit policy；
ADR 0003 和内核演进 roadmap 会把该目标与当前行为分开定义。Commit policy 在概念上仍与
`ComputeIntent` 分离，因为 HP/RT intent 语义不定义可见性或中断。

## GlobalHighPrecision

`GlobalHighPrecision` 是完整质量路径。没有 dirty ROI 时，它执行普通完整计算。带 dirty ROI 时，
它会进入 HP dirty update 路径，而不会无条件把该请求替换成 full-frame recompute。

HP 脏区更新是一等的 dirty-ROI 消费方，而不只是完整重算 fallback。普通 dirty ROI request
会计算反向 ROI 计划，将脏区对齐到 HP tile 边界，从该 request 的 `ComputeTaskGraph`
中裁剪 HP work set，更新受影响 HP tile，记录 HP ROI/版本元数据，并可调度 downsample
工作刷新 `RealtimeProxyGraph` 状态。
`IntentUpdateCoordinator` 会把 global HP dirty request 路由到该路径，并记录
`intent_coordinator_global_dirty_update`。

forced HP dirty update 是例外：当 `force_recache=true` 时，HP staging buffer
有意不从旧 HP cache seed 像素，因此 executor 会在提交前把 HP planning ROI 扩展为目标节点
当前完整 HP extent。这样可以保持 authoritative HP output 完整，同时让非 forced dirty ROI
request 继续保持局部执行。

Dirty-region state planning 通过图级 `DirtyRegionPlanner` 运行，产生的
`DirtyRegionSnapshot` 会输入 dirty work-set materialization，并提供给交互层 inspection 摘要。

因此，一次 public Host HP dirty request 会经过一条连续的 kernel-native geometry 路径：
request validation、graph-scoped backward projection、immutable plan selection、source-first
ready dispatch、node execution 与 staged HP commit 都观察 `PixelRect`/`PixelSize` value。

## RealTimeUpdate

`RealTimeUpdate` 需要 dirty ROI。没有 `dirty_roi` 的请求是非法的，并返回清晰的 public
`ps::Host` status/error value；embedded adapter 从内部 Kernel 与 InteractionService 诊断派生该
value。该请求不会隐式表示全帧 RT 更新。

带有效 dirty ROI 时，realtime 计算会启用两条路径。RT 会先启动并更新低分辨率
`RealtimeProxyGraph`；HP 会通过 staged buffer 更新受影响 graph 工作的完整尺寸权威输出。
如果 request 是 forced，HP sibling 会遵循与 Global HP dirty update 相同的 full-frame HP
planning 规则；RT proxy work 仍然按 RT dirty plan 的局部范围执行。
当 HP 和 RT scheduler runtime 都可用时，`IntentUpdateCoordinator` 会并发启动两个 sibling，
先等待 RT，并通过 sibling commit gate 保证 HP 只在 RT proxy commit 成功后才修改
`GraphModel`。没有 scheduler runtime 时，同一批 callback 会按 RT 后 HP 的顺序 inline 执行。

并发路径还会让两个 sibling 共享一个 request-owned 的 per-node synchronization object。它会保护
同一节点的 live `Node` snapshot 与 format-neutral parameter resolution，但不会合并两个 domain plan：
不同节点与 operation body 仍可并发；即使发生 failure cleanup，该对象也会在两个 sibling future
都 drain 后才释放。

Realtime planning 有意按路径分别执行，而不是通过一次混合 domain 的 planner 调用生成两份任务池。
`IntentUpdateCoordinator` 会分发 sibling HP 与 RT update callback，并为 Dirty RT request
记录 RT-first/concurrent 阶段。每条路径都使用一个 single-domain request plan 和同 domain 的
dirty snapshot：HP callback 使用
`GlobalHighPrecision` node/cache-pruned plan 与 HP dirty snapshot，RT callback 使用
`RealTimeUpdate` node/cache-pruned plan 与 RT dirty snapshot。HP dirty node execution 写入
`HighPrecisionDirtyWriteBuffer`；RT dirty node execution 写入 `RealtimeProxyWriteBuffer`，
并且只提交到 `RealtimeProxyGraph`。Dirty snapshot 会从该路径的 task graph 中裁剪或激活
update work set。这样会把完整 task expansion、node/cache pruning、dirty snapshot pruning
和 output commit 保持为每个 compute domain 的独立契约。

传入的 dirty ROI 会在当前请求中转换为图级 planner state。Public `ps::Host` 的
begin/update/end 方法会通过 embedded adapter 转换到内部 `Kernel` / `InteractionService`
dirty-source lifecycle 方法，使 frontend 或 node-facing code 通过同一个图级 owner 写入 source
lifecycle state。Host lifecycle call 是当前公共写入面；当前没有会自动创建或调度 compute request
的 public node-event subscription。

RT task graph expansion 是 domain-aware 的。当某个 operation 拥有不同的 HP 与 RT metadata 时，
`RealTimeUpdate` plan 会使用 RT metadata 进行 tile size 和 dependency ROI planning，而 HP sibling
会使用 HP metadata。这能让 RT Micro_16 planning 独立于 HP Macro_256 吞吐默认值。

当前实现没有 node-to-backend realtime event subscription。Node 和 Host lifecycle call 可以更新
graph-owned dirty state；`InteractionService` 仍只是内部转换边界，CLI 仍面向 batch。任何
subscription surface 都不属于当前软件契约。

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

`GraphEventService` 把每节点计算事件发布到线程安全、固定容量的 ring。Production 容量是每图
8,192 个事件。每条被接纳的 publication 都会获得 `1..UINT64_MAX-1` 范围内单调递增的
unsigned 64-bit sequence；`UINT64_MAX` 是 exhaustion sentinel，绝不会分配给事件。Ring 满时
只淘汰恰好一条最旧 retained event，并通过饱和 drop counter 记录这次淘汰。

事件 name 与 source 在复制进 retained storage 前通过 `std::string::size()` 测量，因此 public
1,024-byte 上限是 UTF-8 byte 上限。任一字段超过上限时，整条 publication 会被丢弃且不截断；
该 attempt 仍消耗一个有效 sequence，并增加一次 drop。Sequence 耗尽后，每次 publication
attempt 只增加一次 exhaustion drop，sequence 与 drop counter 都不会回绕。

`Host::drain_compute_events(session, limit)` 接受 1 至 1,024 的 limit，并返回
`ComputeEventBatch`：带 sequence 的 `events`、`next_sequence`、`has_more` 与
`dropped_count`。成功调用只移除返回的最旧 page，并原子重置 shared drop counter；空 page
也遵循该规则。Invalid limit 会在移除或重置前返回 `GraphErrc::InvalidParameter`。Event service
在移动事件前预留 result capacity，因此 `std::bad_alloc` 不会移除 caller 未收到的事件。CLI
consumer 只在 `has_more` 为 true 时继续请求最大 bounded page，但每轮 polling pass 都有按
`ceil(8192 / 1024)` 推导出的固定 8-page 预算。无并发 producer 时，该预算足以覆盖完整的
production retained ring；有活跃 producer 时，它可防止单轮无限追页，并把预算后剩余或新发布
的事件留给未来 polling pass 重新检查。Host 不存在无上限 vector drain。

`TimingCollector` 独立地在启用 timing 时存储节点计时与总计算耗时。`NodeOutput` 中的 debug
metadata 记录 worker id、时间戳、执行时间、设备和可选范围检查。

## 错误处理

计算失败会尽可能抛出带 `GraphErrc` 分类的 `GraphError`。同步 Kernel 路径会存储由 mutex
保护的每图 `LastError`，供 best-effort observation 使用。异步 work item 则返回自身拥有的
`AsyncComputeResult`，其中包含精确 failure code/message，并且只把该 value 镜像到
`LastError`；它的 Host future 绝不会从这个可变镜像重建 status。Embedded adapter 会把这些
value 映射为 public `OperationStatus`、`Result<T>` 或 `ps::Host::last_error()` value；frontend
只观察 public Host surface，绝不直接检查 Kernel 或 `InteractionService`。

Scheduler 准入失败发生在 graph load 或 replacement，而不是 ready-task loop。无效的超八请求或未知
type 映射为 `InvalidParameter`；固定进程 worker ledger 耗尽时，`GraphErrc::ComputeError` 会通过
embedded Host 与 IPC status 边界保持不变。

## 边界与原理

- 同一份 request plan 同时提供顺序与并行执行语义；execution strategy 只改变机制，不改变
  topology 或 dirty 含义。
- `GraphStateExecutor` 保护可见 graph 一致性，`SchedulerTaskRuntime` 只接收 ready compute work，
  因而 graph-state command 绝不会成为 scheduler task。
- HP cache 与 RT proxy state 使用不同 staged commit path，因此 preview state 不会被隐式提升为
  authoritative HP output。
- Scheduler epoch 只拒绝陈旧的 queued callback。当前流程没有通用 graph revision、supersession、
  deadline 或 cooperative cancellation contract。

这些拆分使 planning、physical dispatch 与 visible commit 可以独立测试。
[ADR 0001](../../adr/zh/0001-graph-state-access-is-not-scheduler-dispatch.zh.md)约束当前
graph-state/dispatch 区分。已接受的
[进程执行域目标](../../roadmap/zh/Kernel-Evolution.zh.md#进程执行域)描述后续 revision 与 cancellation
边界，但不会让它们成为当前流程的一部分。

## 实现与验证入口

- `src/lib/runtime/kernel_compute.cpp`
- `src/lib/compute/compute_service.*`
- `src/lib/compute/compute_dispatch_plan_builder.*`
- `src/lib/compute/compute_task_dispatcher.*`
- `src/lib/compute/intent_update_coordinator.*`
- `src/lib/compute/dirty_update_executor.*`
- `src/lib/runtime/graph_event_service.*`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_scheduler.cpp`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_host_adapter.cpp`
- `tests/unit/test_event_stream_boundaries.cpp`
