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
compute-request lane、事件、当前 scheduler 和平台运行时资源，但不拥有计算依赖规划。

**`GraphModel`**
内存中的图状态：节点、拓扑邻接、参数、输出、缓存元数据、计时数据和图级运行时元数据。

## 图状态与持久化

**图状态操作（Graph-state operation）**
读取或修改可见图状态、但不会成为计算任务的操作，例如图文档加载、cache 命令、inspection
或 ROI projection。

**`GraphStateExecutor`**
当前图状态操作和可见计算 capture/commit transaction 使用的每图独占访问机制。它拥有
一个 worker 和最多容纳 64 个等待 callback 的 FIFO 队列，不包括至多一个正在执行的
callback。满队列 submission 会阻塞；close 会停止 admission、排空已有 work 并 join worker。
即使 failure recovery 在并发 closer 被唤醒前重新开放 lane，这些 closer 仍会等待自己
加入的持久 completion generation。它与 scheduler dispatch 分离，其 worker 不占用 scheduler
worker slot。

**图文档（Graph document）**
用于创建或更新图状态的持久化表示。YAML 是当前具体格式；“图文档”描述其行为，而不会把
序列化库当作图状态。

**每图独占访问（Per-graph exclusive access）**
当前 visible Graph capture、mutation、commit validation 与 publication 由唯一 graph-state lane
串行化。长时间 compute 会在该 lane 之外操作 request-owned snapshot；另一条 compute-request
lane 会串行化同一 Graph 的 compute 与 scheduler-owner access。

**`GraphInstanceId`**
一个 live Graph instance 的私有强类型、非零、process-lifetime identity。Compute snapshot 会复制
该值；以相同 user-visible session label 重新打开 Graph 时会创建不同 identity，从而避免 commit
期间发生 label-reuse ABA。

**`GraphRevision`**
一个 Graph instance 内 compute-correctness state 的私有 checked nonzero revision。范围内的
structural、document、cache、dirty 与 lifecycle mutation 会推进该值；compute snapshot 与成功的
compute publication 会保留该值。当前 commit compatibility 规则要求 identity 与 revision 精确
相等。Topology generation 仍是独立的 planning cache key。

## 计算规划与执行

**`ComputeIntent`**
请求的语义质量/更新意图。`GlobalHighPrecision` 与 `RealTimeUpdate` 描述 HP/RT 行为，
会选择 planning/operation 语义，也会选择 per-graph scheduler-map route。该值不会作为 scheduler
task metadata 传入，不定义 thread pool、task priority、QoS、deadline、fairness、cancellation
mode 或 commit policy。

**`ComputeService`**
内部计算 facade。它通过更窄的协作者协调请求验证、规划、缓存策略、脏工作选择、operation
解析、派发、指标和 staged output commit。

**`ComputeRun`**
当前用于一个非 realtime HP domain 或一个 realtime HP/RT child domain 的私有、
request-owned 执行记录。其不可变 descriptor 包含不透明且不复用的 Run id、session label、
强类型 Graph instance identity、权威 `GraphRevision`、target、单 domain intent、匹配的
full/interactive quality 与显式 QoS。它拥有单调 phase、exact-once terminal state，并通过共享 control state
拥有相应路径所需的 full submission plan/temporary result 或 dirty HP staging。内建 CPU
full、dirty 与 preflight task 会保留不可伪造的 `ComputeRunLease`，通过固定的多 Graph
`ExecutionService` 执行 owned callback，并且只通过匹配的
`(RunId, RunLocalTaskId)` 发布失败。Realtime request 当前会创建成对的 child Run，
但不创建 `RunGroup`。最终 lifecycle registry、cancellation、supersession 与 request-owned
`RunGroup` 仍是后续工作。

**`ComputeRunQos`**
Run 捕获的私有不可变调度输入：显式 `Interactive` 或 `Throughput` service class、可选的单调时钟
absolute deadline、正 weight 与可选的正 maximum-parallelism descriptor。当前 service 会将
class、deadline 与 weight 用于 policy ordering 和 headroom admission。Maximum parallelism 会
继续记录，但尚不是 execution cap。Deadline 用于排列 interactive work，不会让 Run 过期或取消。
当前 Kernel request 使用 throughput，且这些值都不会从 intent 或 output quality 推断。

**`FullTaskGraph`**
一个 graph generation、compute intent 和 task-shape 配置下完整的 node/tile task 形态。
请求目标、cache 状态和 dirty 状态不会创建该形态。

**`ComputePlan` / `ComputeTaskGraph`**
把 full task graph 裁剪到目标和依赖锥后形成的请求级静态计划。只要仍有由它派生的
scheduler-visible task 可能执行，它就保持不可变。

**`DirtyRegionSnapshot`**
记录 dirty source、受影响 region、tile 和 edge mapping 的图级 ROI 与生命周期状态。
它不是计算任务图或 scheduler queue。

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
submission；只有旧式 dirty scheduler route 仍可推送借用 task handle。

**`ReadyTaskSubmission`**
一个计算依赖已经满足的 move-only service submission。它拥有不可变 Run/task identity、匹配 lease、
executable、priority hint 与受信任的 Host resource declaration。Legacy scheduler 仍可能接收
borrowed handle 或 owned callback，但两条 route 都不会收到 `GraphModel`、task graph 或 dirty
propagation ownership。

## 调度、缓存与数据

**`IScheduler`**
当前旧式 scheduler 接口。一个 serial、GPU 或 plugin scheduler instance 会为一条
per-graph intent route 拥有 worker lifecycle、ready queue、batch/epoch state 与
completion/exception publication。Threaded implementation 同时拥有 policy 与 physical
resource；`serial_debug` 同步执行。内建 CPU Graph binding 改用固定的、由 Host 组合的
`ExecutionService`，不拥有 `IScheduler`。该旧式接口对这些 route 仍是当前行为，但不是
已接受的最终 policy-only scheduler generation。

**`SchedulerPolicy`**
当前 service 使用的私有无状态比较 seam。其 interactive 与 throughput implementation 会排列不可变
ready descriptor；由 service 拥有的 store 保留 Graph/Run fairness state 与物理 entry。Policy 不拥有
worker、ready entry、Run、Graph、budget、resource token、executor、completion route 或 lifecycle
authority。它不是未来用于替换 plugin 的 ABI。

**Scheduler worker request**
配置中、planning 前的值。零表示 automatic，一到八保持精确；更大的值无效。Automatic resolution
为 `min(max(1, detected hardware concurrency), 8)`。Request 尚不是 reservation，也不是正在运行的
thread 数量。

**Resolved worker grant**
Scheduler 构造前产生的一到八非零值。它是内置 CPU worker 上限，也是一个受信任 plugin instance
的 ABI v2 `num_workers` hard ceiling。Plugin 可以少拥有 worker thread，但不得更多。它与最终
进程 slot 计费不同，因为内置 GPU scheduler 还要计入潜在 device worker，而内置
`serial_debug` 计费为零。

**Scheduler worker slot**
一个潜在 scheduler-owned 物理 worker 的保守准入单位。设备不可用或合规 plugin 少创建 worker
时，slot 仍可能保持预留。它不是 ready callback、operation 内部 thread、graph-state executor、
daemon worker 或观测到的 OS thread。

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
move-only、不可伪造 child authority。Graph load 会在构造任一 scheduler 前原子取得 HP/RT legacy
CPU-slot reservation pair；replacement 会在旧 owner 保持存活时取得一个 candidate reservation。
`ReservationOwnedScheduler` 会先销毁 concrete scheduler，再精确释放。Run root 会持续保持计费，
直到全部 queued/executing child grant 都结束。对于内建 Throughput Run，不具权威的 class-quota
charge 具有相同 root lifetime；Interactive 与 legacy root 不会扣减该 quota。

**`SchedulerTaskRuntime`**
scheduler-owned、push-only 的 ready-task dispatch 机制。它接受 initial ready batch 和后来释放的
ready batch；不会从 plan 拉取、派生 task、检查 graph topology 或提交 graph state。

**`SchedulerTaskPriority`**
当前独立的 `Normal`/`High` ready hint，与 `ComputeIntent` 正交。HP 与 RT dirty source batch 都使用
`High`，二者的 downstream group 都使用 `Normal`。在 service policy store 中，它不是绝对 priority：
在跨 class 仲裁已经选定的 service class 内，aging 可以让较旧的 normal-hint entry 穿过持续的
high-hint stream 被选中。

**Scheduler epoch**
Scheduler-local 的非零 batch identity，用于拒绝 stale queued work 和忽略 stale completion
publication；零表示 compatibility work。它不是 dirty generation、graph revision、Run identity、
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
cache 或 scheduling 语义的所有者。

## 必须保持区分的术语

- `ComputeIntent` 不是 scheduler priority 或 commit policy。
- Dirty generation 不是 scheduler epoch。
- 图状态操作不是计算任务。
- `DirtyRegionSnapshot` 不是 `ComputeTaskGraph`。
- ready task 不是任务图。
- HP cache 不是 RT proxy state。
- `ImageBuffer` 不是[目标通用数据模型](../../roadmap/zh/Kernel-Evolution.zh.md#通用数据与-region)。
- Worker request 不是 resolved grant，grant 也不一定等于最终 scheduler slot 计费。
- 已预留 scheduler worker slot 不能证明当前正好有一个 thread 在运行。
- `ResourceVector` 不是 worker pool、观测到的 allocation 总量，也不是猜测未声明
  device/I/O/plugin 维度的许可。
- Root reservation 不是正在执行的 callback；child grant 不能移出 ledger 创建的 ownership path。
- 旧式每图 `IScheduler` 既不是当前由 Host 组合的内建 CPU `ExecutionService`，也不是
  [目标 policy-only scheduler generation](../../roadmap/zh/Kernel-Evolution.zh.md#进程执行域)；
  当前 service 边界与剩余 lifecycle 约束由
  [ADR 0007](../../adr/zh/0007-compute-runs-and-process-execution-have-separate-owners.zh.md)固定。

## 实现与验证入口

- `include/photospider/host/host.hpp`
- `include/photospider/core/compute_intent.hpp`
- `include/photospider/core/image_buffer.hpp`
- `include/photospider/scheduler/scheduler.hpp`
- `src/lib/runtime/graph_runtime.hpp`
- `src/lib/graph/graph_model.hpp`
- `src/lib/graph/graph_state_executor.hpp`
- `src/lib/compute/task_graph_planning.hpp`
- `src/lib/compute/dirty_region_snapshot.hpp`
- `src/lib/compute/execution_service.hpp`
- `src/lib/runtime/resource_ledger.hpp`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_resource_admission.cpp`
- `tests/unit/test_resource_ledger.cpp`
