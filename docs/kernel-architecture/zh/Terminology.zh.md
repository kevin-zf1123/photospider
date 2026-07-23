# 内核术语

本词汇表定义当前内核实现使用的语言。只属于已接受目标决策的术语，包括
[ADR 0007](../../adr/zh/0007-compute-runs-and-process-execution-have-separate-owners.zh.md)
或[内核演进目标](../../roadmap/zh/Kernel-Evolution.zh.md)中的术语，都不得被描述为当前运行时对象。

## 产品与运行时所有权

**`ps::Host`**
后端之外代码唯一受支持的接口。嵌入式前端、CLI 和 IPC adapter 通过该 seam 交换复制后的
公共值，不会获得 `Kernel`、`GraphRuntime` 或 `GraphModel` 引用。

**Embedded Host adapter**
`ps::Host` 的进程内实现。它拥有一个 `Kernel` 和内部交互状态，同时共享进程级 operation
plugin owner。

**IPC Host adapter**
已安装的 client-side `ps::Host` 实现。它把 Host 调用转换为版本化本地 IPC 协议，并拥有
client polling 和映射图像生命周期，不拥有 daemon session 或后端运行时对象。

**`Kernel`**
内部多图 facade，也是 graph、compute、cache、traversal、inspection 和 persistence service
的组合所有者。

**`GraphRuntime`**
每图资源容器。它拥有一个 `GraphModel`、一个有界 graph-state lane、一条有界私有
compute-request lane、一个私有 supersession coordinator、复制的 HP/RT execution-route binding、
event、execution trace 与平台运行时资源，但不拥有计算依赖规划、policy context 或物理 route worker。

**`GraphModel`**
内存中的图状态：节点、拓扑邻接、参数、输出、缓存元数据、计时数据和图级运行时元数据。

## 图状态与持久化

**图状态操作（Graph-state operation）**
读取或修改可见图状态、但不会成为计算任务的操作，例如图文档加载、cache 命令、inspection
或 ROI projection。

**`GraphStateExecutor`**
当前图状态操作和可见计算 capture/commit transaction 使用的每图独占访问机制。它拥有
一个 worker。Graph-state 模式最多容纳 64 个等待 callback，不包括至多一个 active callback；
私有 compute-request 实例则对 queued、running 或 parked 的 one-shot/ticket admission 合计精确
计费 64 个单位，active worker 不是未计费的第 65 个单位。Reserved continuation 从 reservation
到 retirement 始终拥有同一个 FIFO node，wake 与 worker-tail handoff 都不会 self-submit 或等待容量。
达到容量上限的 submission 会阻塞；close 会停止外部 admission/wake、排空已有 one-shot 与
reserved-ticket work，并 join worker。
即使 failure recovery 在并发 closer 被唤醒前重新开放 lane，这些 closer 仍会等待自己
加入的持久 completion generation。它与 ready dispatch 分离；其 worker 是不计费的基础设施，
不是 Run execution grant。

**图文档（Graph document）**
用于创建或更新图状态的持久化表示。YAML 是当前具体格式；“图文档”描述其行为，而不会把
序列化库当作图状态。

**每图独占访问（Per-graph exclusive access）**
当前 visible Graph capture、mutation、commit validation 与 publication 由唯一 graph-state lane
串行化。长时间 compute 会在该 lane 之外操作 request-owned snapshot；另一条 compute-request
lane 会串行化同一 Graph 的 compute 与 execution-route access。

**`GraphInstanceId`**
一个 live Graph instance 的私有强类型、非零、process-lifetime identity。Compute snapshot 会复制
该值；以相同 user-visible session label 重新打开 Graph 时会创建不同 identity，从而避免 commit
期间发生 label-reuse ABA。

**`GraphRevision`**
一个 Graph instance 内 compute-correctness state 的私有 checked nonzero revision。范围内的
structural、document、cache、dirty 与 lifecycle mutation 会推进该值；compute snapshot 与成功的
compute publication 会保留该值。当前 commit compatibility 规则要求 identity 与 revision 精确
相等。Topology generation 仍是独立的 planning cache key。
产品 compute 还要求 Run 捕获的 supersession key 与 generation 精确保持 current。

**`SupersessionKey` / `SupersessionGeneration`**
一个 live Graph 内部的私有 latest-wins identity。Key 由 target node 与 canonical request intent
组成：缺失 intent 与显式 HP 属于同一 lineage，realtime 则保持独立。Checked nonzero graph-wide
allocator 为每个 prepared candidate 分配严格递增且永不 wrap/reuse 的 generation。Allocation 只是
准备；graph-state publication 才是 current-generation linearization point。每个 admitted key
至多拥有一个 reserved compute-lane ticket、一个 active/draining candidate 与一个 latest pending
mailbox value。

## 计算规划与执行

**`ComputeIntent`**
请求的语义质量/更新意图。`GlobalHighPrecision` 与 `RealTimeUpdate` 描述 HP/RT 行为，
会选择 planning/operation 语义，也会选择复制的 per-Graph 私有 execution-route binding。该值不会
作为 ordering authority 传给 policy，也不定义 thread pool、task priority、QoS、deadline、fairness、cancellation
mode 或 commit policy。

**`ComputeService`**
内部计算 facade。它通过更窄的协作者协调请求验证、规划、缓存策略、脏工作选择、operation
解析、派发、指标和 staged output commit。

**`ComputeRun`**
当前用于一个非 realtime HP domain 或一个 realtime HP/RT child domain 的私有、
request-owned 执行记录。其不可变 descriptor 包含不透明且不复用的 Run id、session label、
强类型 Graph instance identity、权威 `GraphRevision`、target、单 domain intent、匹配的
full/interactive quality、显式 QoS 与不可变 request supersession identity。其 shared control
拥有单调 phase、唯一 exact
terminal/commit arbiter、稳定的第一个 cancellation reason，以及相应路径所需的 full submission
plan/temporary result 或 dirty HP staging。Run 会 mint 一个私有弱生命周期 cancellation source；
普通 `ComputeRunLease` 只能观察 explicit cancellation 或过期 deadline，并保留 cleanup/
commit-contender lifetime。Full、dirty 与 preflight task 会通过 Host-owned `ExecutionService`
与封闭的私有 route 执行 owned callback，并且只通过匹配的 `(RunId, RunLocalTaskId)` 发布失败。

**`RunGroup`**
当前用于一对 realtime HP/RT child 的私有 request owner。它捕获一个 realtime supersession
identity，拥有彼此独立的 HP Full 与 RT Interactive child Run 及其 observation lease，共享
request-wide cancellation 与单调 RT-first sibling gate，并按 resource failure、其他 failure、
group/child cancellation、success 的确定顺序聚合结果。RT 在 proxy commit 前 cancellation/failure
会拒绝 HP；child-only HP cancellation 不会取消 RT；已经提交的 RT proxy 不会被之后的 HP 或
generation failure 回滚。它不拥有 child plan/dispatcher、worker、Graph state、resource mint、
lifecycle registry 或 public cancellation control。

**`ComputeRunQos`**
Run 捕获的私有不可变调度输入：显式 `Interactive` 或 `Throughput` service class、可选的单调时钟
absolute deadline、正 weight 与可选的正 maximum-parallelism descriptor。当前 service 会将
class、deadline 与 weight 用于 policy ordering 和 headroom admission。Maximum parallelism 会同时
限制 Run root 对 CPU/retained/scratch 的 callback-concurrency 估算，以及该 Run 同时 in-flight 的
callback 数量；它不会调整固定 worker pool 的大小。Ready-entry 与 ready-byte 准入仍覆盖每个逻辑
task。Deadline 会排列 interactive work；当现有 cooperative boundary 观察到 injected monotonic
clock 已达到或超过该值时，还会通过 Run terminal arbiter 提出 `DeadlineExceeded`。这会在没有
timer thread 或 wall clock 的情况下协作式使 Run 过期。当前 Kernel request 使用 throughput，
且这些值都不会从 intent 或 output quality 推断。

**`FullTaskGraph`**
一个 graph generation、compute intent 和 task-shape 配置下完整的 node/tile task 形态。
请求目标、cache 状态和 dirty 状态不会创建该形态。

**`ComputePlan` / `ComputeTaskGraph`**
把 full task graph 裁剪到目标和依赖锥后形成的请求级静态计划。只要仍有由它派生的
execution-visible task 可能执行，它就保持不可变。

**`DirtyRegionSnapshot`**
记录 dirty source、受影响 region、tile 和 edge mapping 的图级 ROI 与生命周期状态。
它不是计算任务图、policy snapshot、ready store 或 execution route。

**`DirtyUpdateWorkSet`**
由 dirty snapshot 从既有请求 plan 中选出的活动任务子集。选择可以激活或裁剪已规划任务，
但不会创建新的 node 或 tile task 形态。

**`DirtyControlLane`**
应用 node-originated dirty lifecycle update 并刷新图级 dirty 状态的串行路径，不拥有计算任务。

**`ComputeTaskDispatcher`**
拥有 dependency counter、ready release、temporary-result indexing 语义与 completion aggregation
的执行协调器。对于当前 full HP 工作，request `ComputeRun` 拥有 `TaskSubmissionPlan` storage 和
temporary result slot，而 dispatcher 拥有其中的 dependency transition 与 final result commit。
Dirty executor 复用它的 source-first submission helper，并通过 dirty write buffer 拥有自己的
staged commit。内建 CPU full、dirty 与 preflight 路径会推送由 lease 支撑的 owned
submission，并经过公共 `ExecutionService` 边界。

**`ReadyTaskSubmission`**
一个计算依赖已经满足的 move-only service submission。它拥有不可变 Run/task identity、匹配 lease、
executable、priority hint 与受信任的 Host resource declaration。Host-owned ready store、policy
frontier、reserved-start transaction 与私有 route 只能观察各自的有界投影；都不会收到
`GraphModel`、task graph 或 dirty propagation ownership。

## Policy、Execution、缓存与数据

**Policy**
内建或纯 C ABI v1 selector；它接收一份不可变、有界 candidate snapshot，并返回一个 candidate id
或 abstain。Policy 不拥有 worker、queue、ready entry、resource grant、Run、Graph、executor、
completion route、logger 或 lifecycle authority。借用的 snapshot 只在 callback 期间有效。

**`PolicyClass` / policy binding**
`Interactive` 与 `Throughput` 是两个进程 service class。每个 class 有一个进程级 binding，拥有
built-in 或 DSO context、非零 generation、可选的 immutable first fault，以及 metadata/context/
invocation 所需的 DSO lease。同名 replacement 也会创建新 context 和 generation；即使两个 class
使用同一 type，它们仍保持独立。

**`PolicyRegistry`**
进程内 built-in 与已加载纯 C policy type registry。一个 DSO 经过验证后作为单个全有或全无事务
发布；unload 会移除可见性，但 active metadata、binding、context 与 invocation 会保留独立 DSO
lease。它不是 execution-plugin registry。

**Host-authored frontier**
任何 policy callback 之前由可信 Host 选出的 candidate 子集。Host state 决定 service class、
startability、cancellation/route compatibility、八次 dispatch aging frontier、最早有限
Interactive deadline、最小 Graph/Run projected service score、saturation escape 与稳定 enqueue
顺序。Policy 只能从不可变 original snapshot 选择，不能扩大 frontier 或生成 work。

**Execution worker request**
固定 Host CPU pool 的进程配置值。零表示有界 automatic resolution，一到八保持精确。配置完成后，
零或相同正值会保留 pool，冲突正值无效。它不是 Run grant、policy input 或当前执行 callback 数量。

**私有 execution route**
封闭 Host implementation id `cpu`、`serial_debug`、`gpu_pipeline` 之一。`GraphRuntime` 只存储复制的
HP/RT id 与非零 generation。Host-owned `ExecutionService` 拥有物理 worker/queue 与 route-specific
in-flight state；route 不能作为 plugin 扫描或加载。

**`ResourceVector`**
一个完整且经过检查的 request 或 snapshot，具有彼此独立的 CPU-slot、retained-memory-byte、
scratch-byte、ready-entry 与 ready-byte 维度。零表示该维度声明的数量为零；它绝不表示 ledger
可以自行虚构未知数量。

**`ResourceLedger`**
由一个 `ExecutionService` 独占的私有 Host 权威 mint。它原子准入完整 vector，只签发有界 child
grant，并在 parent 与 child ownership 结束后精确释放容量。私有 release observer 可以在这个精确
root-release 点更新不具权威的 companion accounting，但不能铸造或扩大容量。默认上限属于 Host
composition，而不是静态 process singleton。Ledger 不拥有 worker、ready ordering、dependency、
lifecycle registry、device/I/O/plugin 估算或 fairness authority。

**有界 ready store**
由 `ExecutionService` 拥有的 policy-aware store，其聚合 entry 与 accounted-byte 计数不得超过
不可变 ledger 上限。每次 dispatch 按
`work_units + ceil(complete_ready_grant_bytes / 4096)` 计费，在每个已选 class 中独立累加原始
Graph service，并在每个 Run 的不可变 class 内累加按 weight 归一化的 Run service，同时遵守
interactive deadline ordering。它先通过固定的 Interactive/Throughput 三比一仲裁选择 service
class；在已选 class 内，ready entry 会在八次成功 dispatch 后先于普通 policy comparison aging，
且 aging 绝不会改变已选 class。Initial 与 dependency-released submission 会跨越同一边界，Run
row 也会跨越临时为空的阶段继续存在。从 store 移除 entry 时，只会在取得 execution authority 或
清除该 entry 后释放其 ready grant。

**Resource reservation 与 grant**
Reservation 是一个原子准入 root vector 的 move-only RAII owner；grant 是在该 vector 内签发的
move-only、不可伪造 child authority。Run root 会持续保持计费，直到全部 queued/executing child
grant 都结束。Ready-entry/byte grant 覆盖 queued submission；reserved start 会把精确选中的 ready
grant 交换为 CPU/retained/scratch execution grant。Throughput Run 的非权威 class-quota charge
具有相同 root lifetime；Interactive root 不会扣减该 quota。

**Reserved start**
Policy selection 与 executor entry 之间仅由 Host 执行的事务。私有 `SelectionPin` 标识精确 ready
entry/version；`StartTransaction` 会 stage resource grant，重新检查 current Run/cancellation/route/
fairness state，再以 allocation-free、no-throw 方式提交 removal、service-accounting update 与 callback
transfer。被拒绝或 pre-commit 抛异常的路径不会改变 ready/fairness state，并恰好一次释放 staged grant。

**`ExecutionTaskRuntime`**
Reserved start 后使用的私有 push-only task/completion adapter。它接受 initial 与后来释放的 ready
work，发布 route worker/epoch attribution，并 settle completion 或首个 exception。它不会从 plan
拉取或派生 task，不检查 Graph topology，不排列 candidate，不铸造 resource，也不提交 Graph state。
它不是已安装扩展 ABI。

**`ExecutionTaskPriority`**
当前独立的 `Normal`/`High` ready hint，与 `ComputeIntent` 正交。HP 与 RT dirty source batch 都使用
`High`，二者的 downstream group 都使用 `Normal`。在 service policy store 中，它不是绝对 priority：
在跨 class 仲裁已经选定的 service class 内，aging 可以让较旧的 normal-hint entry 穿过持续的
high-hint stream 被选中。

**Execution epoch**
私有非零 route/runtime batch identity，用于 Host 准入后的 attribution 与 stale completion isolation。
它不是 policy generation、binding generation、dirty generation、Graph revision、Run identity、
deadline 或 cooperative cancellation token。

**HP cache**
`cached_output_high_precision`，节点上唯一具有权威性的可复用内存图像结果。

**`RealtimeProxyGraph`**
runtime-owned 的临时 RT 输出状态。它不是权威 HP cache，也不会作为可复用图缓存持久化。

**`ImageBuffer`**
当前图像 payload 契约：二维 extent、通道数、单一 scalar type、device、row stride、共享数据
所有权和可选 backend context。它不是通用 Tensor、Deep Image 或 vector-scene 模型。

**`PixelRect` / `PixelSize`**
公共 Host 与 operation 契约以及私有 Graph、ROI propagation、dirty-region、cache identity、
planning 与 task state 使用的不依赖外部 library 的整数 geometry value。只能在 OpenCV
adapter 或 provider 实际调用 matrix/library 时局部构造 OpenCV geometry；这些内核契约不会
存储或传递该类型。

**Operation provider**
operation callback、propagation contract 与 metadata 的实现来源。依赖中立 core operation
始终在进程 seed 时组合。仓库 OpenCV CPU provider 是独立的可选 build module，拥有自身
algorithm、进程初始化与异常翻译。它与 v2 DSO provider 都向相同的 provider-neutral registry
slot 发布，因此 DSO 可以替换 active operation，并在卸载后恢复其 predecessor。公共 operation
契约使用 Photospider 值类型。

**Adapter**
位于外部库、transport 或产品边缘的窄转换。Adapter 转换表示，但不会成为 graph、planning、
cache、policy 或物理 execution 语义的所有者。

## 必须保持区分的术语

- `ComputeIntent` 不是 `PolicyClass`、execution priority、QoS 或 commit policy。
- Dirty generation 不是 execution epoch、policy binding generation 或 route generation。
- 图状态操作不是计算任务。
- `DirtyRegionSnapshot` 不是 `ComputeTaskGraph`。
- ready task 不是任务图。
- HP cache 不是 RT proxy state。
- `ImageBuffer` 不是[目标通用数据模型](../../roadmap/zh/Kernel-Evolution.zh.md#通用数据与-region)。
- Execution worker request 不是 Run reservation 或 child grant。
- Policy candidate id 或 decision 不是 execution authority；只有已提交的 reserved-start transaction
  才能进入私有 route。
- Policy binding generation 不是 route generation、snapshot generation、Run id 或 supersession
  generation。
- `ResourceVector` 不是 worker pool、观测到的 allocation 总量，也不是猜测未声明
  device/I/O/plugin 维度的许可。
- Root reservation 不是正在执行的 callback；child grant 不能移出 ledger 创建的 ownership path。
- Policy 只拥有 ordering；私有 execution route 只拥有物理 entry；Run 拥有 request correctness 与
  settlement；Host 拥有 validation 与 resource authority。

## 实现与验证入口

- `include/photospider/host/host.hpp`
- `include/photospider/core/compute_intent.hpp`
- `include/photospider/core/image_buffer.hpp`
- `include/photospider/policy/policy_plugin_api.h`
- `src/lib/runtime/graph_runtime.hpp`
- `src/lib/graph/graph_model.hpp`
- `src/lib/graph/graph_state_executor.hpp`
- `src/lib/compute/task_graph_planning.hpp`
- `src/lib/compute/dirty_region_snapshot.hpp`
- `src/lib/compute/execution_service.hpp`
- `src/lib/execution/execution_task_runtime.hpp`
- `src/lib/policy/policy_registry.hpp`
- `src/lib/compute/compute_request_coordinator.hpp`
- `src/lib/compute/compute_supersession.hpp`
- `src/lib/compute/run_group.hpp`
- `src/lib/runtime/resource_ledger.hpp`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_resource_admission.cpp`
- `tests/unit/test_resource_ledger.cpp`
- `tests/unit/test_policy_registry.cpp`
- `tests/unit/test_compute_supersession.cpp`
