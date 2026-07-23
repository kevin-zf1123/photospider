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

`Kernel` 拥有多图 API。`GraphRuntime` 拥有一个图模型、每图可见状态
`GraphStateExecutor`、独立的有界串行 compute-request lane、每个 live Graph 的一个
`ComputeRequestCoordinator`、事件服务、平台 context，以及每个 intent 的一个 execution
binding。每个 binding 只包含精确 route ID 与非零 generation。Embedded composition root
还会在 Kernel 之前创建一个私有固定 `ExecutionService`。该 service 独占 Host 权威
resource ledger、policy binding、有界 ready store、reserved-start transaction、物理 route
与 completion callback；Kernel 会把该 owner 注入每个 request-local `ComputeService`。

`ps::Host` 是面向 frontend 的 public interface。embedded Host adapter 会复制 public
request/result value，并在内部使用 `InteractionService` wrapper 与 `Kernel`；CLI/TUI code
不会直接包含或调用这些 backend facade。在 dirty-region 语境中，Host 暴露图级 dirty snapshot
和 lifecycle value；`InteractionService` 仍是内部转换边界，不是 dirty-region generation 或
propagation 的权威来源。

frontend compute 命令构造 public `ps::HostComputeRequest`，不会把位置式 boolean flag
或内部 request type 穿过 public seam。embedded Host adapter 会把该 value 转换为
`Kernel::ComputeRequest`。`Kernel` 负责 graph lookup、runtime start、request-local
quiet/skip-save policy、async scheduling、image extraction 和 LastError 映射。随后它把内部请求转换为
`ComputeService::Request`，后者承载 node target、cache、telemetry、intent、dirty ROI、
session identity 与显式 Run QoS 数据。parallel/runtime 选择则通过独立的
`ComputeService::ExecutionStrategy` 承载。Public
`HostComputeExecutionOptions::maximum_parallelism` 字段会把单次 Run 的一个可选正并发上限
穿过 adapter 传入该 QoS。其余 identity 与 QoS 仍只是私有 descriptor input；plugin ABI 不变。

Dirty ROI 从 `HostComputeRequest` 复制到 `Kernel::ComputeRequest`，再经过 graph propagation、
planning、task selection、staged execution 与 `NodeExecutor` 时，始终保持为内核自有的
`PixelRect`；extent 使用 `PixelSize`。这条路径不会进行 OpenCV geometry 转换；provider 只有在
真实 matrix 或算法 call 处才能创建局部 OpenCV rectangle 或 size。

CLI/REPL 前端是固定的批处理取向界面。它不暴露 RT intent 命令、dirty ROI 创建命令，
也不暴露 `compute rt`、`--dirty-roi`、`dirty begin`、`dirty update` 或 `dirty end`
这类 dirty source lifecycle 命令。`RealTimeUpdate` 和 dirty source lifecycle API 可由 Host
与 kernel/test caller 使用，但 CLI 不暴露这些能力，也不应被视为生产 realtime 控制面。

固定 service thread 属于基础设施，不是 per-Run reservation。Execution 配置会把 `0` 解析
为 bounded automatic value，或保留显式 `1..8`，随后冻结 CPU service 数量、启动固定 CPU
pool，并启动一个私有 Metal worker lane。Host 不暴露 Metal 时，该 lane 保持空闲；它不是可
独立配置的 worker grant。
之后的零或相同请求保持幂等，冲突的正数请求会被拒绝。发布 work 前，每个 Run 都会从
service-owned Host ledger 原子预留完整的 CPU、retained-memory、scratch、ready-entry 与
ready-byte vector。Graph load 只复制 route ID 与 generation，不拥有 worker grant。该契约不
声称覆盖 compute、operation 或私有 GPU backend 使用的全部 thread。

Benchmark 配置不会重新配置该进程池。对于每次 benchmark Run，`execution.threads` 会解析为
一个可选正值 `maximum_parallelism`：缺失或 `0` 会在 `[1,8]` 中选择有界自动值，
`1..8` 则选择精确的 Run 上限。一个 `BenchmarkService` 最多以自动
`worker_count=0` 准备 execution 一次；这会启动尚未配置的 pool，或保留已经固定的 pool。
配置文件解析完成后，`RunAll` 不会校验 disabled session 的 thread 范围，也不会运行这些
session；它会记录并跳过 thread 值无效的 enabled session，并在同一个固定 pool 上执行使用
混合 cap 的有效 session。

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
  -> Run-owned TaskSubmissionPlan / ComputeTaskDispatcher
  -> ExecutionService policy / reserved start / private execution route
```

在上述任何 planning step 前，coordinator 已为 request 分配 canonical `(target, intent)`
supersession key 与经检查的 graph-wide generation。每次非 realtime HP service call 都会创建且
只创建一个 request-owned `ComputeRun`；realtime call 则创建一个含独立 HP 与 RT child Run 的
`RunGroup`。每个 child 都捕获 fresh opaque Run id、caller-visible session label、不复用的强
Graph instance identity、非零权威 `GraphRevision`、target、单 domain intent、full 或
interactive quality、显式 QoS 与不可变 supersession identity。Topology generation 仍是独立的
task-shape cache key，不再充当 Run revision provenance。Sequential、parallel 与 dirty execution strategy 共享此边界。Full HP Run control 拥有 materialized plan
与 runner；每个真实 ready task 在 execution、dependency release、validation 与 commit 期间都会
保留不可伪造的 Run lease 与 `(RunId, RunLocalTaskId)`。Full HP、connected-parameter preflight 与 dirty HP/RT 会把 dependency-ready work 包装为
move-only `ReadyTaskSubmission`，并发送到固定 multi-Run service。Planning 会在准入前冻结选中
的 operation implementation 与 device；每个 submission 会让该 device 稳定穿过 ready storage
与 dependency release。Dirty 阶段使用 heap-owned context，因此没有 stack executor pointer
跨越该边界。每个私有 route 都保留相同 Run lease
与 Host-authored completion identity。Realtime child 通过 request-owned `RunGroup` settle；其稳定 control 拥有两个
observation lease、RT-first gate、cancellation fan-out 与确定性 aggregate outcome。

`FullTaskGraphExpander` 会把原始 graph 展开为一个 compute domain 的完整 node/tile task graph。
它不依赖请求目标、cache 状态或 dirty snapshot。`NodeCacheTaskGraphPruner` 随后把该 graph
裁剪到请求目标和依赖锥，并记录所选节点的 cache 可用性。Dirty update 会再经过独立的
`DirtySnapshotTaskGraphPruner`，用 dirty metadata 标注已选择的 graph，并产生活跃的
`DirtyUpdateWorkSet`。

单线程和并行执行使用相同的 full-expansion 与 node/cache-pruning 语义。二者的 execution
mechanism 不同，而 graph traversal、dirty planning 与 task-graph pruning 仍由 compute 拥有。

`ComputePlan` 是当前 compute request 和 domain 的静态分析结果。它从在某一精确
identity/revision 捕获的 request-owned Graph snapshot 推导，并在该 request 内作为 topology
contract 保持不变。Operation work 期间，每条产品路径都只写 request-owned Graph/proxy state。
Dirty update 不会重新定义拓扑语义。它们使用当前 `DirtyRegionSnapshot` 和 dirty ROI，从 plan
中为每条 HP 或 RT update 队列激活或裁剪 `DirtyUpdateWorkSet`。

请求 plan 必须枚举该请求可用的真实 compute task；当选择的实现是 tiled 时，也包括 tile task。
Dirty state 只从这个已枚举 task graph 中裁剪或激活 task。Dirty clipping 阶段不得展开新的 tile task；
这样 full-frame tiled parallelism 和 dirty ROI execution 会共享同一个 task model。

当 execution task 仍可能从某个 request 的 `ComputeTaskGraph` 派生并运行时，该 task graph 保持
immutable。每个 dirty compute request 会基于自身 plan 与 snapshot 创建 generation-local
selection overlay，随后只发布不可变 ready submission。Policy 只收到标量 frontier candidate，不会收到可替换的
task-graph object。

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

意图模型是正式的。`ComputeService` 仍是 compute facade 和 planning boundary。私有 route
metadata 会选择物理路径。`cpu` 与 `serial_debug` route 只暴露 CPU；Host 报告 Metal 时，
`gpu_pipeline` 依次暴露 Metal、CPU，否则只暴露 CPU。CPU、serial-debug、GPU、
connected-parameter preflight、full 与 dirty 阶段都使用固定注入 service；GraphRuntime 只保存
复制的 route ID 与 generation。

HP/RT 双路径语义属于 realtime intent，而不是 parallel 执行模式。Realtime 模式下，HP
计算完整尺寸的权威 node 工作，RT 计算降采样代理版本，目前为宽高各四分之一，也就是像素数的
十六分之一。`IntentUpdateCoordinator` 会启动两个 sibling call，每个 sibling 再解析自身的
通过 process service 解析复制的私有 route binding。`ComputeIntent` 本身不指定 QoS 或
最终 physical policy。Service 消费显式 `ComputeRunQos`；当前由 Kernel 创建的 Run 使用 descriptor
默认 throughput，除非私有 caller 显式提供 interactive QoS。Intent、quality 与 maximum
parallelism 都不会推断 class、deadline 或 weight。

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
7. 在 request snapshot 中存储输出、发出事件并更新时间。磁盘写入保持关闭，直到最终的精确
   revision commit transaction。

顺序计算适合简单执行和调试。它会先创建与并行路径相同的内部完整展开和 node/cache
裁剪后的 task graph 语义，再执行递归路径。

## 并行计算

并行计算先展开完整 task graph，再从 `topo_postorder_from` 通过 `NodeCacheTaskGraphPruner`
裁剪得到 `ComputePlan`。`ComputeDispatchPlanBuilder` 会记录这个 cache-pruned plan 供检查使用。
Request `ComputeRun` 拥有 `TaskSubmissionPlan`；后者把 plan 中的 `ComputeTaskGraph`
materialize 为 dependency counter、ready value、operation variant、selected device 和临时结果槽。Dispatcher 会创建不可变、由 lease 支撑的 `ReadyTaskSubmission`，并把一个 initial ready
batch 提交到注入的 `ExecutionService`；dependent completion 会通过同一个 active Run 创建
后续 submission，并进入同一个有界 store。Tiled 操作可能产生微任务，并减少选中私有 route
的 logical completion count。

Run 发布前，service 会原子预留完整且经过检查的 CPU、retained-memory、scratch、ready-entry
与 ready-byte vector。CPU slot 及 uniform per-task retained/scratch envelope 使用固定 worker 数、
逻辑 task 数与 Run 可选正值 maximum parallelism 三者的最小值；ready entry 与 byte 仍覆盖每个
逻辑 task。Initial 与 dependent entry 持有 child ready grant；reserved start 会在进入 submission
所冻结的 CPU 或 Metal lane 前，把该 authority 交换为 CPU/memory/scratch。如果 device 不在已配置
route/Host inventory 中，service 会在发布 Run 前拒绝它。Failure、queue purge 与成功 settlement
都会精确释放容量。固定 lane 绝不会按 Run 调整大小，CPU/Metal callback 共用该 Run 已准入的
parallelism ceiling。Execution inspection 与
replacement 和 compute 共用 per-graph compute-request lane，使复制的 route generation 保持
一致。Replacement 会校验封闭词汇中的 route，并在一个 transaction 中发布新的非零
generation；失败保留旧 binding。

Ready-store policy 对每次 dispatch 按
`work_units + ceil(complete_ready_grant_bytes / 4096)` 计费：Graph 公平性在已选 service class
各自独立的 accumulator 中使用原始 cost，而每个 class 不可变的 Run 使用
`ceil(cost / weight)`。Interactive work 会偏好存在且更早的单调时钟 deadline；throughput 排序
采用加权且确定的规则。Store 会先选择 service class；两个 class 都持续 ready 时，它会在至多三次
Interactive dispatch 后强制选择 Throughput。八次 dispatch aging 随后只在该已选 class 内生效，
不能改变 class 决策。

配置的 interactive headroom 只把 active 内建 Throughput root reservation 限制在 general
ceiling。Interactive work 不会消耗这项 class quota，但唯一 ledger 仍会授权它们
共享的物理容量。Throughput charge 与 ledger reservation 原子提交，并且只有在所有 child grant
结束、root reservation 被物理释放时才扣回。Initial 与 dependent submission 使用同一路径，
service 会让每个 Run fairness row 跨越临时为空的阶段继续存在。Policy 策略不拥有 worker、token、
budget、Run 或 Graph；service 与 ledger 仍分别是物理和资源权威。

`ComputeTaskDispatcher` 将 plan execution、依赖计数、稀疏 node-id 映射、temporary-result
indexing、事件日志、异常传播和最终目标选择保留在 compute-service 边界内；对应 plan 与
result-slot storage 由 Run 拥有。Dispatcher 通过私有 execution task runtime dispatch 已经
planned 的 work；它不会让 policy 或 route 拥有 dirty propagation、compute-task derivation 或 task
graph 本身。如果裁剪后的 planned dispatch 为空，但目标没有可复用 HP 输出，dispatcher 会报告
planning contract error，而不是回退到递归顺序计算。

对于 dirty 执行，`DirtySnapshotTaskGraphPruner` 只 materialize 当前 dirty snapshot 从 request
`ComputeTaskGraph` 中选出的 active `DirtyUpdateWorkSet`。Runtime dependency counter 与 ready-task
queue 是 execution artifact；它们不存放在 `DirtyRegionSnapshot` 中，也不由 policy 或 route 拥有。

显式 Host begin/update/end call 会指定 dirty source node 并更新 graph-scoped lifecycle fact，但
不会触发 compute。当前 backend 不存在 node subscription、automatic request launch 或
active-request coalescing contract。

Lifecycle update 会进入 per-graph serialized path 并调用 `DirtyControlLane`。该 lane 更新 source
state，为 event domain 重建 source-local derived record，再通过 `DirtyControlLaneResult` 返回
wakeup/cutoff hint。该 lifecycle path 不遍历 downstream edge，这些 hint 当前也没有 production
compute consumer。Embedded Host 暴露 copied inspection value，不暴露内部 control field。
Execution service 只接收带 Host-authored identity、route 与 priority metadata 的不可变 ready
submission；dirty generation 不会复用为 route 或 policy generation。

## 图状态访问与提交策略

YAML 加载、cache 命令、inspection 和 ROI projection 等 graph-state operation
都是对可见 `GraphModel` 的操作。它们不是 compute-task dispatch，也不会通过
私有 execution task runtime 路由。

可见状态的默认语义是通过 `GraphStateExecutor` 提供 per-graph exclusive access。结构/cache/dirty
mutation、request snapshot capture、精确 commit predicate evaluation 与 no-throw publication
使用该 lane。耗时较长的 planning、operation callback、policy call 与 route wait 只使用 request-owned
`GraphModel` 和可选 `RealtimeProxyGraph` snapshot，并在该 lane 外执行。因此，operation 阻塞时
mutation 可以推进 `GraphRevision`；旧 request 随后会在精确 revision predicate 处失败，而不会
覆盖新 Graph。

每个 runtime 还拥有一条有界串行 compute-request lane。同步、返回图像与异步 compute 会通过
coordinator publication；execution inspection/replacement 使用普通 one-shot submission。
该 lane 精确计费 64 个 queued、running 或 parked 单元。每个 supersession key 会保留一个持久
continuation ticket 与一个 latest pending mailbox；重复 publication 只替换该 pending value。
现有 lane worker 是唯一的 logical active-request runner，每次 ticket turn 最多让一个 generation
经过既有 Kernel/ComputeService path 执行；不会创建额外 background runner 或 generation thread。
不同 Graph runtime 彼此独立，位于不同 live Graph instance 的相同 label 也不会互相 supersede。

Required-session lifetime admission 仍会覆盖同步/异步 compute、required graph save、node-YAML
replacement、ROI projection、timing inspection 与 all-cache clearing，直至完成 public
result/status translation。Embedded close 期间，Host 先发布 close marker，并让更早准入的同步调用
完成 public translation。随后 Kernel 停止 compute-request admission，Host 再等待 async
placeholder 与 status publication。这样不需要 request FIFO 出现空位，就能唤醒被满队列阻塞的
producer。已接受 request 会在 graph-state admission 保持开放时排空，以便执行最终 commit；随后
Kernel 排空 graph-state、把 runtime 标记为 stopped 并移除它。进程拥有的 worker 与 policy
binding 比 Graph 存活更久；复制的 route binding 没有需要停止的物理 owner。Node replacement
与 ROI projection 仍在同一 graph-state work item 内完成 required lookup 与 mutation。
Execution information 会在 compute-request lane 内复制 route/statistics，不会暴露物理 owner
或 queue capability。

每条产品 compute path 现在都使用 staged output。每条产品 compute path 现在都使用 staged output。Kernel 在同一 identity/revision 捕获完整 Graph
与可选 RT proxy，关闭 snapshot 磁盘写入，并且只针对这些 snapshot 执行 sequential、policy-selected、dirty HP 与 RT work。本地 output validation 后，ComputeService 会把匹配 Run
推进到 `CommitPending`。私有产品 commit policy 会验证精确 Run/staged/live identity、权威
revision 与 current supersession key/generation，执行符合条件的延迟 HP cache persistence，并在
同一 graph-state work item 中交换完整可见状态。
只有该 transaction 成功后才会发布 Run success。

这是截至 issue #76 的当前基线。私有 request source 可以 cooperative cancel 一个 HP Run，或当前
realtime request 的两个 child Run；immutable deadline 会在有界 observation point 提议
`DeadlineExceeded`，Run-owned terminal arbiter 则会排列 cancellation、failure 与 visible commit。
每个 Graph 的 latest-wins publication 会让精确 key 的最新 generation 成为权威、请求取消旧的
active owner、合并一个 pending owner，并在 cancellation 迟到时仍拒绝 stale commit。这是
process-owned `RunLifecycleRegistry` 现在会在 capture 或 planning 前开始 candidate、保留 Graph
lifetime lease，并原子安装一个 standalone Run 或包含两个 realtime child 的完整 bundle。
Empty/no-op 与 connected-preflight 路径使用相同的 admission/finalization 边界。connected
preflight 会在不进入 provider 的情况下冻结 provider/device/callable 与完整 service root；
provider 只在 bundle installation 与 reserved start 后进入，其 output 会在已安装 Run 内驱动
dirty planning。Graph close 与 process shutdown 会通过该 registry 发起 cancellation，并在
unregistration 前等待精确 physical/resource settlement。worker-local queue/callable/lease owner
会在 settlement notification 前 retire，持久 finalization authority 不能被静默丢弃。这仍是
cooperative 而非 preemptive execution；不声称支持 provider preemption 或 public Host/CLI/IPC
cancellation。Commit policy 在概念上仍与 `ComputeIntent` 分离，因为 HP/RT intent 语义既不定义
可见性，也不定义 cancellation authority。

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
该请求会创建一个 `RunGroup`，其中包含一个 full quality HP child Run 和一个 interactive quality
RT child Run。当两个
物理 execution domain 都可用时，`IntentUpdateCoordinator` 会并发启动两个 sibling，先等待
RT，并通过 sibling commit gate 保证 HP 只在经过 revision 验证的 RT proxy publication 成功后，
才尝试自身独立的 Graph commit。
两个 child 都共享固定 service，并使用各自复制的私有 route。没有 parallel
domain 时，同一批 callback 会按 RT 后 HP 的顺序 inline 执行。每个 child 只有在自身 visible
commit 后才发布终态；group 会在两个 child observation lease 都 settle 后才解析稳定 aggregate。
有效 RT publication 后的 Graph mutation 可能让 HP sibling 变为 stale；RT 保持可见，而 HP 以
`ComputeError` 失败。更新的 realtime generation 会取消两个旧 child，并拒绝旧 pending gate。
如果旧 RT proxy 先完成 commit，它会保持可见，但旧 HP sibling 仍会在 current-generation
predicate 失败。

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

Policy 与 execution configuration/replacement 失败发生在 candidate 发布之前。
无效的超八请求、与固定 pool 冲突的正数请求，或未知 type 会映射为 `InvalidParameter`；Host
ledger 耗尽时，`GraphErrc::ComputeError` 会通过 embedded Host 与 IPC status 边界保持不变。
内建 CPU Run 聚合溢出或 full-vector capacity 耗尽同样会在任何 initial ready entry 发布前返回
`ComputeError`。Public `maximum_parallelism` 显式为零是非法值，会在图执行前由 Host 或
IPC decoding 拒绝。

## 边界与原理

- 同一份 request plan 同时提供顺序与并行执行语义；execution strategy 只改变机制，不改变
  topology 或 dirty 含义。
- 一个非 realtime HP request 拥有一个 `ComputeRun`；realtime 拥有一个 `RunGroup`，其中包含从
  planning 前 descriptor capture 到各自独立 terminal publication 与 aggregate settlement 的不同
  HP 与 RT child Run。HP Run 拥有
  full-plan temporary result 或 dirty HP staging。每个内建 CPU callback 都保留匹配 child
  lease 与复合 task identity，但 Run 不拥有 Graph state、worker 或 dependency transition
  的语义。
- `GraphStateExecutor` 保护可见 graph 一致性与精确 commit，私有 compute-request lane 保护
  same-Graph request 和 route replacement 顺序。私有 execution task runtime 只接收 ready
  compute work，因而 graph-state command 绝不会成为 execution task。
- HP cache 与 RT proxy state 使用不同 staged commit path，因此 preview state 不会被隐式提升为
  authoritative HP output。
- Execution entry version 只拒绝陈旧的 queued submission，并不是 Run identity。HP/RT
  failure 会在稳定 lease 下通过 `(RunId, RunLocalTaskId)` 路由。当前 Run 会记录显式 QoS，service
  会将其用于 ordering、公平性与 headroom admission，同时记录精确 Graph identity/revision
  provenance。Deadline 仍是 ordering input，并且会在 Run 到达 cooperative observation point 时
  触发 expiry。内建 ready publication、queue/dequeue、operation、dependency、phase 与 commit
  边界都会观察私有 Run cancellation；已经进入的 non-preemptible provider 可以完成，但其 staged
  output 不能 commit。Current supersession generation 是独立 commit predicate，因此迟到的
  cancellation 无法恢复 stale output。Graph-close 与 process-shutdown lifecycle cancellation
  已是当前私有契约；public cancellation control 仍是未来行为。

这些拆分使 planning、physical dispatch 与 visible commit 可以独立测试。
[ADR 0001](../../adr/zh/0001-graph-state-access-is-not-scheduler-dispatch.zh.md)约束当前
graph-state/dispatch 区分。已接受的
[ADR 0007](../../adr/zh/0007-compute-runs-and-process-execution-have-separate-owners.zh.md)
同时定义当前固定的多 Graph HP/RT service、独立 child Run、内建 policy ordering、
lease/completion isolation、权威 revision/generation-safe staging、RT-first 独立 commit gate、
cooperative Run cancellation、确定性 `RunGroup` settlement、latest-wins supersession、
admitted-Run registry、Graph lifetime lease 与 close/shutdown lifecycle 所有权。
[进程执行域目标](../../roadmap/zh/Kernel-Evolution.zh.md#进程执行域)保留长期所有权方向，但不改变
这些当前事实。

## 实现与验证入口

- `src/lib/runtime/kernel_compute.cpp`
- `src/lib/host/embedded_host.cpp`
- `src/lib/benchmark/benchmark_service.*`
- `src/lib/ipc/request_router.cpp`
- `src/lib/compute/compute_service.*`
- `src/lib/compute/run_lifecycle_registry.*`
- `src/lib/compute/execution_lifecycle_telemetry.*`
- `src/lib/compute/compute_supersession.*`
- `src/lib/compute/compute_request_coordinator.*`
- `src/lib/compute/compute_run.*`
- `src/lib/compute/run_group.*`
- `src/lib/compute/compute_dispatch_plan_builder.*`
- `src/lib/compute/compute_task_dispatcher.*`
- `src/lib/compute/intent_update_coordinator.*`
- `src/lib/compute/dirty_update_executor.*`
- `src/lib/runtime/graph_event_service.*`
- `tests/integration/test_compute_service_split.cpp`
- `tests/unit/test_policy_registry.cpp`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_host_adapter.cpp`
- `tests/integration/test_opencv_operation_concurrency.cpp`
- `tests/unit/test_ipc_protocol.cpp`
- `tests/unit/test_compute_run.cpp`
- `tests/unit/test_event_stream_boundaries.cpp`
