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

### 事务化启动与 runtime 发布

`CpuWorkStealingScheduler::start()` 与 `GpuPipelineScheduler::start()` 提供强异常保证。
Queue array、mutex 所有权与 worker vector 会先暂存，再发布生命周期。若资源分配或任意
CPU/GPU thread 构造抛出异常，scheduler 会发布 `running=false`，通知全部 worker/completion
wait，join 本次已经创建的每个 thread，并清空暂存 array、queue、counter 与 exception state。
随后原样重新抛出 `std::bad_alloc`、`std::system_error` 或 plugin exception。失败后重复调用
`shutdown()` 仍然安全，同一 scheduler object 可以再次 `start()` 并执行后续 batch。
即使暂存 thread 已经存在，在完整 worker vector、queue array、mutex 所有权、counter 与 exception
state 安装到成员存储前，`is_running()` 仍保持 false。Observer 因此不会把局部安装的 worker set
视为 running scheduler。

`GraphRuntime::start()` 是外层生命周期事务。它会预留 rollback ledger，并在调用每个已停止
scheduler 的 `start()` 前先记录该 scheduler。若某次 start 在局部状态已经发布后失败，本次失败的
scheduler 以及此前由该调用启动的 scheduler 都会按逆序 shutdown。Rollback 的次生错误会被压制，
从而保留第一个 start error；只有全部 scheduler 成功启动后，`GraphRuntime::running()` 才会变为 true。
`GraphRuntime::stop()` 会在同一个 lifecycle mutex 下发布 stopped state；即使前一个 plugin 抛出
异常，也会继续尝试每个 scheduler shutdown，并只在 sweep 完成后重新抛出第一个 failure。其
`noexcept` destructor 会压制显式 plugin lifecycle failure，而 scheduler owner 仍保留自己的最终
cleanup fence。

确定性的 allocation/thread-creation hook 只存在于 `BUILD_TESTING=ON` object 中。
`BUILD_TESTING=OFF` 产品不包含 hook storage、导出的 test seam 或强制 GPU route。

### 事务化 batch enqueue 与借用 handle

每次多任务 enqueue 都是 queue transaction。CPU high-priority/global-ready 与 local work-stealing
route，以及 GPU RT、HP-CPU 与 GPU route，会在持有相关 queue lock 时记录原始 deque 长度，
再追加完整 batch。任意 insertion 抛出异常时，已经追加的 entry 会从尾部以不分配内存的方式移除。
只有整个 batch 提交成功后，才发布 epoch、ready/completion/stat counter、exception
claim/pointer/epoch/cleanup/visible state 与 condition-variable notification。Insertion 失败时，
这些 exception 与 epoch field 全部保持精确的调用前值。Worker 不会观察到 batch prefix，原始异常
identity 会继续传播。

`TaskHandle` 是 executor pointer 与 task id 组成的借用 pair。它的 `TaskExecutor` 必须让所有成功
提交的 callback 存活到 `wait_for_completion()` 返回。失败 batch 不提交任何 handle，执行零个
callback，因此 request-local dirty executor 可以立即展开栈，而不会在 queue 中留下指向已销毁
stack storage 的 entry。Rollback 后，同一 scheduler object 可以接收下一 batch。Exception
publication 会先经过相同的 queue transaction gate，再选择自己的 epoch，所以并发 batch 只会被
观察为完整提交或完整不存在。

### 批次异常发布与复用

CPU work-stealing 和 GPU-pipeline runtime 每个批次只发布一个原始 worker exception。独立的
first-publisher latch 只在新批次开始时 reset。胜出的 publisher 会先验证执行中 task epoch 与 active
batch 一致，写入精确的 `first_exception_` 与 exception epoch，拒绝该 batch 后续 ready submission，
并清空所有 queued callback。只有 queue cleanup 完成后，才以 release store 发布 consumer 可见的
`has_exception_` flag。Publisher 会在 claim、pointer、cleanup、epoch 与 visible-flag publication
全部完成前持续持有同一个 initial-submission queue gate。因此新 initial batch 无法在旧 pointer
store 与旧 flag store 之间 reset exception state，从而消除跨 epoch publication。

Dequeued callback 在离开 queue cleanup 可见范围前携带 scheduler batch epoch 并增加 in-flight count。
来自 stale epoch 的 completion、completion-count growth 与 exception publication 都会被忽略。
Completion waiter 在 completion count 与 in-flight count 都归零前不会返回成功；在 queue cleanup 完成
且所有旧 callback settle 前也不会重新抛出失败。随后 waiter 才在 exception mutex 下读取并清除精确
pointer/flag。这个 settle-before-return 策略保证紧接着提交新 batch 是安全的：不存在仍能清空新 queue、
递减新 count 或把迟到 exception 发布到新 batch 的旧 publisher。

对于 CPU work stealing，本地 queue enqueue 与对应 ready-predicate increment 会先持有 global
predicate mutex，再持有目标 local mutex；本地 dequeue 会在释放该 local mutex 前递减 predicate。
Exception cleanup 使用相同的 global-to-local lock order，在排空全部 queue 期间一直保留 global
mutex，并且只在访问完所有 local queue 后把 ready predicate 归零。这样 queue visibility 与数值
wait predicate 构成同一个 publication unit：cleanup 后不会出现迟到 increment，也不会有低于 reset
值的 decrement 泄漏到下一 batch。

这个顺序属于 scheduler reuse 契约：被观察到的 exception flag 始终具有匹配的非空 pointer，等待 caller
会收到原始 exception identity，而下一批次会显式 reset publication state 与 first-publisher latch。
GPU pipeline CPU worker 对 RT queue 与 HP-CPU fallback queue 使用同一个 shared mutex 和同一个
`rt_cv_` predicate handshake；每个 publisher 都在通知前持有该 mutex 修改任一 ready predicate。
不存在无人等待的独立 `hp_cpu_cv_`，因此 HP submission 不会丢失在 predicate evaluation 与
condition-variable sleep 之间。Shutdown 是独立的生命周期 transition；GPU pipeline 会先在 CPU
idle-queue、GPU idle-queue 和 completion-wait mutex 保护下发布 stop state，再通知并 join worker。

按 `ComputeIntent` 路由计算：

| 意图 | 预期调度器角色 |
| --- | --- |
| `GlobalHighPrecision` | 面向吞吐的 HP 计算。 |
| `RealTimeUpdate` | 低延迟交互式更新。 |

`GraphRuntime` 保存以 `ComputeIntent` 为 key 的调度器映射。

长期 scheduler 职责是 planning 之后的资源 dispatch。Scheduler 应从 intent-aware task pool
中拉取 planned 或 annotated task，选择队列顺序、批处理、worker 策略、取消和具体执行资源，
然后分发工作。它们不应拥有图级 dirty-region propagation 或 compute-task derivation。

对于 `RealTimeUpdate`，scheduler-backed 路径现在会先启动 RT dirty sibling，再启动 HP dirty
sibling，并且当两个 scheduler runtime 都在运行时允许两个 sibling 并发计算。RT 写入会 stage 到
`RealtimeProxyWriteBuffer`，并提交到 `RealtimeProxyGraph`；HP 写入会 stage 到
`HighPrecisionDirtyWriteBuffer`，并且只在 RT proxy commit gate 打开后提交到 `GraphModel`。
Scheduler 仍然只负责 ready task callback、epoch、取消和队列策略；它们不会让 RT 输出成为正式
HP 缓存来源。

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
| `gpu_pipeline` | 带可选 GPU HP 队列和设备可用性报告的 CPU 调度器。 |
| `heterogeneous` | `gpu_pipeline` 的别名。 |

`GpuPipelineScheduler::Config` 目前只暴露实际生效的队列控制项：
`cpu_workers`、`gpu_workers` 和 `prefer_gpu_for_hp`。当前 scheduler 总是把 RT
ready work 路由到高优先级 CPU 队列。普通优先级 HP ready work 只有在
`prefer_gpu_for_hp=true`、已配置 GPU worker 且 runtime 挂载了 Metal device 时，才可能进入
GPU 队列。`force_cpu_for_rt`、`rt_preempt_threshold_ms` 以及 scheduler-local implementation
priority table 这类旧字段，不再是当前代码路径的 active configuration。

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

队列选择、批处理、worker reservation、取消和资源专用 dispatch 属于 scheduler。Scheduler
runtime 还会通过 `SchedulerTaskRuntime::available_devices()` 报告可用设备。
`ComputeTaskDispatcher` 会使用这个设备列表和
`OpRegistry::select_best_implementation()`，选择与已经 materialized 的 task shape 匹配的
operation callback。因此，scheduler queue routing 不拥有 operation implementation
selection。Dirty propagation、dirty work-set activation、node/tile 展开、monolithic dirty
escalation 和逻辑 compute-task derivation 属于 scheduler dispatch 之前的阶段。

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
