# 调度器架构

内核具有派发具体 ready work 的 scheduler 接口。本文定义当前 worker、queue、lifecycle、
plugin、intent selection、ready-task 和 trace 行为。

## 当前接口：IScheduler

`IScheduler` 是正式调度器接口，并公开继承 `SchedulerTaskRuntime`。调度器附着到借用的 public
`SchedulerHostContext`，启动 worker 资源、调度计算并干净关闭。Context 只暴露 device capability、
task worker/epoch context 与 trace publication；它不暴露 `GraphRuntime`、graph state、cache ownership
或 native device handle。

核心生命周期：

```text
create -> attach(host_context) -> start -> submit ready callbacks -> shutdown -> detach
```

`GraphRuntime` 实现并拥有 host context，也拥有已注册 scheduler 的生命周期顺序。Scheduler 必须先 attach
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
随后原样重新抛出 `std::bad_alloc`、`std::system_error` 或 local implementation exception。失败后重复调用
`shutdown()` 仍然安全，同一 scheduler object 可以再次 `start()` 并执行后续 batch。
即使暂存 thread 已经存在，在完整 worker vector、queue array、mutex 所有权、counter 与 exception
state 安装到成员存储前，`is_running()` 仍保持 false。Observer 因此不会把局部安装的 worker set
视为 running scheduler。

`GraphRuntime::start()` 是外层生命周期事务。它会预留 rollback ledger，并在调用每个已停止
scheduler 的 `start()` 前先记录该 scheduler。若某次 start 在局部状态已经发布后失败，本次失败的
scheduler 以及此前由该调用启动的 scheduler 都会按逆序 shutdown。Rollback 的次生错误会被压制，
从而保留第一个 start error；只有全部 scheduler 成功启动后，`GraphRuntime::running()` 才会变为 true。
`GraphRuntime::set_scheduler()` 与 `replace_scheduler()` 共用同一个 owner transaction。它们会在
candidate lifecycle 工作前预留可能需要的新 map node；candidate attach，并在 runtime running 时
start 的整个期间，旧 owner 仍保持已发布且存活。Candidate 准备完成后，通过不分配内存的
`unique_ptr` swap 发布。若 candidate attach/start 失败，shutdown 与 detach 会在独立异常边界中
依次尝试，次生 cleanup error 被压制，精确的准备阶段异常继续传播。发布成功后，被替换的旧 owner
按 shutdown、detach、destroy 顺序清理；即使某一步失败，两个 lifecycle stage 仍都会尝试。旧
owner cleanup error 会作为提交后的诊断状态被压制：publication 无法真实回滚，因此已经成功的
replacement 仍报告成功。被替换 owner 仍会销毁，其 reservation 恰好释放一次。
`GraphRuntime::stop()` 会在同一个 lifecycle mutex 下发布 stopped state，并把每个 scheduler 的
`is_running()` 查询与 `shutdown()` 调用视为两个独立的 best-effort lifecycle step，只在 sweep 完成
后重新抛出第一个 failure。若状态查询抛出异常，runtime 会记录该错误，但因为 scheduler 状态未知，
仍会尝试关闭同一个 scheduler；无论其中任一步骤是否失败，后续 scheduler 都会继续 sweep。其
`noexcept` destructor 会压制显式 plugin lifecycle failure，而 scheduler owner 仍保留自己的最终
cleanup fence。

Host 侧 plugin scheduler owner 会在每次进入 `attach()` 或 `start()` 尝试之前重新启用这些最终
cleanup fence。重复 attach 或 start 可能先发布借用的 host pointer、worker 或其他局部 plugin
resource，然后再抛出异常；此时 owner 析构仍会在单次 plugin destroy 调用前，通过独立异常边界执行
detach 或 shutdown。此前一次生命周期已经完成，不能压制本次失败重试所需的 cleanup。

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
观察为完整提交或完整不存在。Executor 的 virtual destructor 为 protected：scheduler code 可以运行
借用对象，却不能通过 base pointer 删除 dispatcher-owned storage。

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

Active epoch 与 completion-count publication 使用同一个 counter mutex，increment/decrement 也经过该
mutex。Initial batch 会先 stage queue state，再原子发布新的非零 epoch 与 count，因此旧 callback 不能先
通过 epoch check，再修改新 batch。Epoch 从 `UINT64_MAX` 回绕到 `1`；零保留给不可取消 compatibility
work。Count 为零时 decrement 是 no-op，非正 increment 是 no-op；正 increment 超过 `INT_MAX` 时抛出
`std::overflow_error` 且状态不变。任何与 active epoch 不同的非零 worker TLS epoch 都属于 stale，
即使跨越回绕，其 increment/decrement 也会被忽略。
Exception consumption 也会使用同一个 counter mutex，并且只有 waiter 捕获的 epoch 仍为 active 时才清零
剩余 count，因此迟到的 waiter 无法把新 batch 的 count 置零。

对于 CPU work stealing，本地 queue enqueue 与对应 ready-predicate increment 会先持有 global
predicate mutex，再持有目标 local mutex；本地 dequeue 会在释放该 local mutex 前递减 predicate。
Exception cleanup 使用相同的 global-to-local lock order，在排空全部 queue 期间一直保留 global
mutex，并且只在访问完所有 local queue 后把 ready predicate 归零。这样 queue visibility 与数值
wait predicate 构成同一个 publication unit：cleanup 后不会出现迟到 increment，也不会有低于 reset
值的 decrement 泄漏到下一 batch。

这个顺序属于 scheduler reuse 契约：被观察到的 exception flag 始终具有匹配的非空 pointer，等待 caller
会收到原始 exception identity，而下一批次会显式 reset publication state 与 first-publisher latch。
`set_exception` 收到空输入时，会在 claim publication 以及修改 queue、count、epoch 或 exception state
之前直接返回。GPU pipeline CPU worker 对 RT queue 与 HP-CPU fallback queue 使用同一个 shared mutex 和同一个
`rt_cv_` predicate handshake；每个 publisher 都在通知前持有该 mutex 修改任一 ready predicate。
不存在无人等待的独立 `hp_cpu_cv_`，因此 HP submission 不会丢失在 predicate evaluation 与
condition-variable sleep 之间。Shutdown 是独立的生命周期 transition；GPU pipeline 会先在 CPU
idle-queue、GPU idle-queue 和 completion-wait mutex 保护下发布 stop state，再通知并 join worker。

`GraphRuntime` 保存以 `ComputeIntent` 为 key 的 scheduler map；当前 compute 会选择
`GlobalHighPrecision` 或 `RealTimeUpdate` entry。该 key 选择一个 per-graph scheduler object，
本身并不定义 QoS、deadline、fairness、cancellation、resource reservation 或 process-wide
priority class。当前 contract 不会从 `ComputeIntent` 推导这些概念。

Scheduler 不会拉取 plan。`ComputeTaskDispatcher` 识别 readiness，再通过
`SchedulerTaskRuntime` 推送具体 callback 或借用 task handle。选中的 scheduler 可以根据 queue、
priority hint、epoch 与 available device 对这些 ready submission 排序和路由，但不会收到 graph
topology、compute plan，也不拥有 dirty propagation。

对于 `RealTimeUpdate`，`IntentUpdateCoordinator` 会通过两个独立 asynchronous call 先启动 RT
dirty sibling，再启动 HP dirty sibling；当两个选中的 scheduler runtime 都在运行时，两个 sibling
可以并发计算。RT 写入会 stage 到
`RealtimeProxyWriteBuffer`，并提交到 `RealtimeProxyGraph`；HP 写入会 stage 到
`HighPrecisionDirtyWriteBuffer`，并且只在 RT proxy commit gate 打开后提交到 `GraphModel`。
每个 scheduler 只处理自己 sibling 的 ready callback、scheduler-local batch epoch 与 queue
policy；它不会创建 sibling，也不会让 RT output 成为正式 HP cache source。

## 当前 Dispatch 状态

并行计算规划和 plan execution 属于 `ComputeService` 协作者：`FullTaskGraphExpander`、
`NodeCacheTaskGraphPruner`、`DirtyRegionPlanner`、`DirtySnapshotTaskGraphPruner`、
`IntentUpdateCoordinator` 和 `ComputeTaskDispatcher`。裁剪完成后，`ComputeTaskDispatcher`
会把 node/cache-pruned task graph 或 dirty 裁剪后的 update work set materialize 为具体 task，
并通过相关 `ComputeIntent` 配置的 `IScheduler` 实例，经由 `SchedulerTaskRuntime` 提交 ready work。

`GraphRuntime` 拥有图状态、`GraphStateExecutor`、scheduler 注册、事件和平台资源。它不暴露通用
worker queue、task graph 或 completion-counter API。Graph-state operation 和会修改可见
`GraphModel` 的 compute request 都使用 `GraphStateExecutor`，包括 scheduler-backed parallel
compute。Scheduler 实现只通过 `SchedulerHostContext` 与 runtime 交互；`IScheduler` 与
`SchedulerTaskRuntime` 都不会直接拥有 runtime model。

同一 executor 也是 scheduler owner 的访问与 teardown boundary。对于一个 session，runtime start、
compute、scheduler name/statistics copy 与 scheduler replacement 不能重叠。Embedded close 会先
发布 Host lifecycle marker，只等待该 marker 之前的同步 admission，然后在等待 async submission
placeholder 之前停止 lane admission。该 stop 会唤醒阻塞在有界 FIFO 上的 producer；已经 admission
的 callback 会继续排空并 join 唯一的 lane worker，随后 runtime stop 才能调用任意 scheduler
lifecycle method。`get_scheduler()` 在内部可以返回 raw pointer，但 caller 必须在 graph-state
callback 有效期间完成全部使用；active compute 释放该 owner 之前，replacement 不能发布新 owner
并销毁旧 owner。

## 内置调度器

| 类型 | 含义 |
| --- | --- |
| `cpu_work_stealing` | 多线程 CPU 调度器。 |
| `serial_debug` | 确定性单线程调试调度器。 |
| `gpu_pipeline` | 带可选 GPU HP 队列和设备可用性报告的 CPU 调度器。 |
| `heterogeneous` | `gpu_pipeline` 的别名。 |

`GpuPipelineScheduler::Config` 目前只暴露实际生效的队列控制项：
`cpu_workers`、`gpu_workers` 和 `prefer_gpu_for_hp`。Scheduler submission 不携带外围
`ComputeIntent`。High-priority work 会进入以 RT work 命名的 CPU queue；normal-priority work 只有在
`prefer_gpu_for_hp=true`、已配置 GPU worker 且 `SchedulerHostContext` 报告 `GPU_METAL` 可用时，
才可能进入 GPU queue。HP 与 RT dirty source batch 当前都使用 high priority，二者的 downstream
group 都使用 normal priority，因此不能把 queue name 理解为 intent contract。

### 每图物理资源所有权

每个 `GraphRuntime` 分别拥有一个 HP scheduler object 与一个 RT scheduler object。Graph load 会创建
二者，runtime start 会启动二者。配置的 worker 请求只有零到八有效。在构造 scheduler 或 plugin
前，零会一次性解析为：

```text
min(max(1, hardware_concurrency()), 8)
```

显式的一到八保持精确；更大的请求会在 worker 构造前被拒绝，且不会改变未来 Graph 的默认值。
内置 CPU scheduler 拥有的 worker 不超过解析后的授权。注册 plugin 会按完整授权计费，可以少创建
worker，但不得超过授权。`gpu_pipeline` 与 `heterogeneous` 按解析后的 CPU 授权再加一个已配置的
潜在 GPU worker 计费，即使准入时设备不可用也一样；因此当前单实例绝对计费上限为九。
`serial_debug` 是唯一的零 slot 例外，会在 calling thread 上同步执行。

这些 scheduler 仍按 graph 与 intent 拥有物理资源；当前没有 shared worker pool 或 cross-graph
fairness mechanism。当前 containment layer 改用一个进程生命周期的 `SchedulerWorkerBudget`，在所有
embedded `Host` 与 `Kernel` 之间共享固定 32-slot 上限。Graph load 会先规划两个 intent scheduler，
并在构造任意一方前原子预留二者的合计计费。返回的 pair 为每个 intent 各含一个 move-only
reservation；每个 reservation 在构造期间位于 concrete scheduler 之外，随后移入自己的
`ReservationOwnedScheduler`。`GraphRuntime` 仍负责 scheduler shutdown 与 detach；reservation owner
只保证 concrete scheduler 的销毁发生在 slot 释放之前。因此 attach/start/YAML publication 失败、
成功的 graph close 与 Host 销毁都会把取得的所有 slot 恰好归还一次。Close 失败则保留 runtime 与
两个 reservation，以便重试。

Scheduler replacement 是 strong transaction：旧 scheduler 与 reservation 保持存活时，先规划并
预留 candidate，再 attach/start candidate 并发布，最后才退役旧 owner。因此 replacement 需要
transient headroom。容量不足会在不构造 candidate、也不扰动旧 compute 行为的情况下返回
`GraphErrc::ComputeError`；无效请求与未知类型仍为 `InvalidParameter`。
发布后，旧 owner 的 shutdown 与 detach 仍会进行 best-effort sweep；它们的失败不会把已提交的
replacement 转成对外失败，销毁过程会把被替换的 reservation 恰好归还一次。这个 32-slot ledger 只约束
内置 planning 或受信任 plugin grant 所代表的 worker；它不约束 graph-state executor、daemon
thread、frontend helper、operation 内部 thread，也不是整个进程的 OS thread 总上限。
`GraphStateExecutor` 具有独立的结构性上限：每个已加载 Graph 有一个 worker，最多有 64 个等待
callback。

## 插件发现与图调度器选择

`graph_cli` 会在加载配置后、加载 graph 前扫描 `scheduler_dirs`。该扫描只会让 scheduler factory
发现 plugin 提供的 scheduler 类型，不会改变某个 graph 已经选择的 scheduler 实例。

新加载的 graph 会从 `Kernel::SchedulerConfig` 接收 scheduler 实例；该配置由
`scheduler_hp_type`、`scheduler_rt_type` 和 `scheduler_worker_count` 填充。因此，plugin
scheduler 只有在这些配置键命名了已扫描的 plugin 类型时才会生效，或者在用户随后对当前 graph
运行 `scheduler set <hp|rt> <type>` 后生效。`scheduler plugins` 报告的是发现状态；
`scheduler get all` 报告的是当前 graph 实际挂载的 HP/RT scheduler 实例。

每个 scheduler DSO 必须先由 `ps_scheduler_plugin_get_abi_version() noexcept` 返回数字 ABI 版本 2。
缺少或不匹配 handshake 时，会在调用 implementation version、discovery metadata 或 instance
creation 前失败；ABI v1 没有兼容路径。Implementation version 随后只查询一次并缓存为 diagnostic
metadata，不作为兼容性 gate。创建时，`num_workers` 是已经解析好的一到八 hard grant。合规的
in-process plugin 可以少拥有 worker thread，但绝不能超过授权；Host 会保守地持有完整授权，直到
concrete instance 销毁。该计数器不是 sandbox，无法阻止恶意或不合规 DSO 创建未申报 thread。

数字 handshake 只拦截预期 Photospider interface generation。已接受的 boundary 仍是临时 C++ ABI，
因为 `IScheduler*` 及其 vtable、标准库 string/container 与 callback、allocator/runtime ownership、
RTTI 和 exception 都会跨越 DSO。当前 plugin 必须使用匹配 SDK 与兼容 C++ 工具链；handshake 不承诺
纯 C 使用、跨工具链 compatibility 或长期 ABI stability。

成功 discovery 还要求 type count 大于零、每个范围内索引都返回非空指针且非空的稳定 name，并且至少有
一个有效 name 不与已有 scheduler type 冲突。无效 identity metadata 会拒绝整个 candidate，不发布任何
type、metadata 或 retained handle。若 candidate 只存在部分冲突且仍有至少一个不冲突 type，则继续原子
提交。

每个进入 scheduler DSO 的 discovery、create、lifecycle 与 runtime call 都会持有显式 lease，并把
plugin-origin exception 转成 host-owned value：新的 `std::bad_alloc`、按 code/message 复制的
`GraphError`、映射为 `InvalidParameter` 的 `std::invalid_argument`，以及映射为 `ComputeError` 的其他
standard/unknown failure。返回的 device enumerator 会在 planning 前校验。Host task failure 保持精确且对
plugin 可见：owner 会预分配 append-only identity slot，relay 不分配内存地记录原始 `exception_ptr` 并使用
裸 `throw;`，host 只恢复匹配的 registered identity。Plugin code 执行时不持有 registry guard，wait 会清除
batch registry，exception object 只在 guard 释放后销毁，因此其 destructor 可以安全重入 scheduler API。

## Daemon 的 Host-Only Scheduler 边界

`photospiderd` 只通过 copied public `Host` value 路由 scheduler discovery 与 control。
Process-global method 包括 `scheduler.types`、`scheduler.description`、`scheduler.scan`、
`scheduler.load`、`scheduler.loaded_plugins` 与 `scheduler.configure_defaults`；它们都不解析
graph session。前五个 method 会 inspection 或 mutation process-owned scheduler factory/loader
state；configuration 只改变之后加载的 graph session 所使用的 default。现有
session 保留其 scheduler object，直到显式 replacement。

`scheduler.info` 与 `scheduler.replace` 会先验证 opaque daemon session id、
`ComputeIntent`，以及适用时的 replacement type，再让一个 session admission 在恰好一次
匹配 Host call 期间保持存活。Embedded Host 会在与 compute 和 graph close 相同的 per-graph
`GraphStateExecutor` 边界内执行 scheduler name/statistics copy 与 replacement。因此，运行中的
compute 不能与该 graph 的 scheduler inspection 或 replacement 重叠；active compute 释放旧
scheduler 前，replacement 也不能销毁它。

每个 direct request 与每个 first-page access 都使用 daemon 的公共 Host mutex。Scheduler
mutation 绝不 retry。`scheduler.types` 与 `scheduler.loaded_plugins` 在唯一一次 Host call 前
预留 bounded collection quota，验证并排序完整 copied list，同时保留 duplicate，再将其
freeze 以供 stable cursor paging。Continuation 只读取这个 frozen process-global value，不调用
Host，也不查找 session。这使 socket IO 保持在 Host lock 之外，同时把 scheduler access
与所有其他 Host-routed family 串行化。

成功的 scheduler plugin library 在 client 断开与 graph session 之间仍由 process loader 持有。
Wire value 只暴露 type name、description、diagnostic plugin label、configuration value，以及 copied
scheduler name/statistics。它们绝不暴露 scheduler pointer、factory、registry、loader、callback、
dynamic-library handle 或 mutable ownership token。完整 wire schema 与边界由
`../../codebase-structure/zh/IPC-Protocol-v1.zh.md` 持续维护。

`scheduler.trace` 保持为独立的 bounded non-destructive observation route。Installed typed IPC
Client 暴露全部九个 scheduler route，会用精确 cursor/offset 验证聚合两个 frozen collection
view，并对每次 direct mutation 或 trace/status observation RPC 只尝试一次。全部九个 route 仍属于
精确排序的 55-method `daemon.version.methods` inventory。Installed
`create_ipc_host(socket_path)` 会通过新的 short-lived typed connection 映射对应的九个 Host
virtual，保持相同的 one-attempt mutation/observation policy，并且绝不回退到 embedded scheduler
state。

## 调度器 Dispatch 边界

`IScheduler` 不暴露 compute-planning helper，因此 scheduler 实现不能拥有 graph/task planning。

当前 dispatch 模型是：

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

当前 scheduler instance 拥有 queue selection、batching、worker lifecycle、epoch filtering 与
resource-specific dispatch。Scheduler runtime 还会通过
`SchedulerTaskRuntime::available_devices()` 报告可用设备。
`ComputeTaskDispatcher` 会使用这个设备列表和
`OpRegistry::select_best_implementation()`，选择与已经 materialized 的 task shape 匹配的
operation callback。因此，scheduler queue routing 不拥有 operation implementation
selection。Dirty propagation、dirty work-set activation、node/tile 展开、monolithic dirty
escalation 和逻辑 compute-task derivation 属于 scheduler dispatch 之前的阶段。

Dirty control lane 不是 dirty-feature-specific scheduler queue。Dirty node 通过串行 control
path 更新图级 dirty lifecycle 和 ROI state；dispatcher 再从该状态 materialize dirty work
generation，并且只把具体 ready task callback 提交给 scheduler。Scheduler 实现根据通用
submission metadata 做决策，例如 scheduler-local epoch 与 optional scheduler-specific priority
hint。Dirty generation 仍属于 task/snapshot provenance；当前 source-first dirty dispatch 不会把
它作为 scheduler epoch 传入。Scheduler 可以通过 epoch drop stale queued work，可以使用
FIFO/LIFO 或 work-stealing queue，可以路由 CPU/GPU resource，也可以保留旧 work 继续运行；它
不会接收 task graph，也不需要专门的 dirty-source queue。

compute-service 路径会先推导 planned work，再进入 scheduler dispatch，并且不暴露
planned-task dispatch 之外的计算路径。空 planned dispatch 只有在目标已经拥有可复用 HP 输出时才是合法的；否则它是 planning contract error。

## Epoch 与取消

调度器队列使用 epoch 取消陈旧排队工作。Epoch `0` 被视为不可取消兼容工作。只有携带真实
epoch 的 submission 才能作为 stale work 丢弃。Epoch filtering 不会取消已经运行的 callback，
也不是通用 `ComputeRun` cancellation contract。ADR 0007 固定目标 Run identity、稳定 lease
与 terminal cancellation 竞态，但当前 epoch 尚未实现它们。

## 可观测性

`SchedulerHostContext::log_event` 通过一个 host adapter，把 public `SchedulerTraceAction` label 映射到
private `GraphRuntime::SchedulerEvent` ring。Built-in 与 plugin scheduler 都经由 context 发布，不重复
实现该 mapping。Private ring 在线程安全、固定容量的 storage 中记录 assignment、node execution、
tile execution、dirty path、stale generation 与 exception rethrow action。每个 graph
会预分配 65,536 个 production slot。有效 publication sequence 为
`1..UINT64_MAX-1`；`UINT64_MAX` 是 terminal exhaustion sentinel，绝不会分配给事件。Ring
满时只淘汰恰好一条最旧 trace。最后一个有效 sequence 被消耗后，后续 publication attempt
都会被丢弃，并使用饱和算术计数。

`Host::scheduler_trace(session, after_sequence, limit)` 是非破坏性的 sequence-page read。Zero
从最旧 retained event 开始，有效 limit 为 1 至 4,096。返回的 `SchedulerTracePage` 只包含
sequence 大于 cursor 的 event，并带有 `next_sequence`、`has_more`，以及 caller cursor 之后
unavailable retained history 与 exhausted publication attempt 的精确饱和 `dropped_count`。Page
内容与 metadata 观察同一个 ring-lock point。重复 cursor 不会移除 trace event 或改变顺序；
exhaustion 前，后续 publication 可以在之后的 read 中出现。

Page 在非空时前进到最后 returned sequence；exhaustion 前的空 page 保留 input cursor；观察到
最后一个有效 sequence，或 exhausted storage 不存在 later retained event 时，返回
`UINT64_MAX`。Sentinel cursor 只有在 exhaustion 后才有效。Invalid limit、future cursor 或过早
sentinel cursor 会在复制 page 前以 `GraphErrc::InvalidParameter` 失败。内部测试可以清除
retained trace slot，但 production code 没有 unbounded trace getter 或 public clear control。

Scheduler-worker 容量验证与 display statistics、trace metadata 相互独立。长期 integration test 会
观察通过 public Host compute path 到达的、同步控制的真实 operation callback；它们不会从
`get_stats()`、per-instance worker id 或整个进程的 OS thread snapshot 推导进程 worker 总数。

## 边界与原理

- `IScheduler` 是当前正式公共 scheduler interface。
- 具体 ready parallel work 通过 scheduler-owned task runtime 路由；scheduler 不拉取 plan。
- Graph-state 命令和可见 graph compute request 位于单 worker、64 个等待任务的
  `GraphStateExecutor` FIFO lane 内。
- Scheduler runtime 为 ready-task-only：它们接收带 scheduler-local epoch 和 optional
  scheduler-specific hint 的具体 callback，而不是 task graph 或 dirty work-set state。
- 插件 scheduler lifecycle 遵循 `Plugin-ABI.zh.md`。
- Scheduler plugin handshake 只拦截 interface generation；已接受的 scheduler object 仍属于临时
  C++ ABI。
- Scheduler attachment 只使用 `SchedulerHostContext`；该 context 不暴露 graph/runtime ownership
  或 native backend handle。
- 单实例 worker grant 与共享 32-slot admission ledger 只约束当前 per-graph ownership；它们不会
  创建 shared executor，也不宣称限制整个进程的 OS thread 总数。
- 当前 interface 同时包含 policy 和物理 worker 所有权。本文描述这项可执行契约，不会把 shared
  executor 表述为当前行为。

Ready-task-only 边界让 scheduler ordering、worker lifecycle 与 failure publication 可以在不拥有
graph topology 或 cache 的情况下测试。进程 ledger 约束当前 per-graph 物理所有权，但不会把
reservation 重新解释为正在运行的 thread 或 shared pool。
[ADR 0001](../../adr/zh/0001-graph-state-access-is-not-scheduler-dispatch.zh.md)约束当前 dispatch
拆分。[ADR 0003](../../adr/zh/0003-process-owned-execution-resources.zh.md)记录高层替代方向；
[ADR 0007](../../adr/zh/0007-compute-runs-and-process-execution-have-separate-owners.zh.md)
固定其详细的 Run、ready-task、completion、resource 与 lifecycle 所有权；精确的
[进程执行域目标](../../roadmap/zh/Kernel-Evolution.zh.md#进程执行域)汇总已接受目标，但不会改变
本文当前契约。

## 实现与验证入口

- `include/photospider/scheduler/scheduler.hpp`
- `include/photospider/scheduler/scheduler_task_runtime.hpp`
- `include/photospider/scheduler/scheduler_plugin_api.hpp`
- `src/lib/runtime/graph_runtime.*`
- `src/lib/scheduler/cpu_work_stealing_scheduler.*`
- `src/lib/scheduler/gpu_pipeline_scheduler.*`
- `src/lib/scheduler/scheduler_factory.*`
- `src/lib/scheduler/scheduler_worker_budget.*`
- `src/lib/scheduler/scheduler_reservation_owner.*`
- `src/lib/scheduler/scheduler_plugin_loader.*`
- `tests/integration/test_scheduler.cpp`
- `tests/integration/test_gpu_pipeline_scheduler.cpp`
- `tests/integration/test_scheduler_worker_budget.cpp`
- `tests/integration/test_scheduler_plugin_loader.cpp`
- `tests/unit/test_scheduler_factory_plan.cpp`
- `tests/unit/test_scheduler_reservation_owner.cpp`
- `tests/unit/test_scheduler_exception_publication.cpp`
