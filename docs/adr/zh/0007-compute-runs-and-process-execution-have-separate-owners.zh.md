# ADR 0007：Compute Run 与进程执行具有不同所有者

## 状态

已接受并实现到 Issue #76。Issue #70 至 #76 已成为 CPU 执行/资源、policy 与私有 route 切片的当前软件
行为：embedded composition root 会注入一个固定的 `ExecutionService`；内建 CPU 的 HP、RT、
connected-parameter preflight 和 dirty source/downstream 工作会跨越 move-only
`ReadyTaskSubmission` 边界。来自多个 Graph 的独立 Run 可以重叠，同时 completion、failure、trace
与 Host state 都按 Run 隔离。Realtime 请求会创建一个 request-owned `RunGroup`，其中包含不同的
HP 与 RT child Run。`GraphRuntime` 只存储复制的 HP/RT route id 与 nonzero generation；任何 Graph
都不拥有物理 worker、queue、policy context 或 plugin DSO lifetime。Service 独占一个 Host 权威
`ResourceLedger`，原子准入完整 Run vector，并让 initial/dependent work 通过按 entry/byte 有界的
ready store。恰好一个 Interactive 与一个 Throughput policy binding 会在 immutable Host-authored
frontier 上执行 work/byte 计费、Graph/Run 公平性、deadline 偏好、aging、headroom 与有界
Throughput 进展，且不拥有 worker 或资源权威。Product compute 会捕获 request-owned Graph/proxy
snapshot，在 graph-state lane 之外执行，并且只在强类型 Graph identity 与 authoritative revision
精确校验后发布。私有 compute-request lane 会串行化同一 Graph 的 request，但不拥有 executor 或
policy lifetime；RT publication 不受之后 stale HP result 的回滚影响。Issue #73 增加私有
cooperative Run cancellation：显式请求与 monotonic deadline 会与 failure 和 Run-owned commit
contender 竞争；匹配的 queued work 会被清除，已经进入的 work 会排空，dependent re-entry 会被
拒绝，且已接受的 cancellation 不能发布 staged state。Sibling gate 仍为 `Pending` 时，RT
cancellation 会拒绝 HP commit 并请求 HP cancellation。Installed Host、CLI 与 IPC protocol v2
surface 仍不可取消，IPC job 继续报告 `cancellable: false`。Issue #74 增加 checked per-Graph
supersession generation、精确 key 的 latest pending coalescing、每个 admitted key 的一个 persistent
continuation ticket、稳定 supersession cancellation、current-generation commit authority 与确定性
`RunGroup` aggregate settlement。现有有界 compute-request lane worker 是唯一 logical active
runner；不会增加每个 Graph 的 background runner 或每个 generation 的 thread。Issue #75 移除拥有
worker 的 scheduler SDK/ABI，并增加纯 C policy ABI v1、原子 generation-scoped binding
replacement、Host 编写的 frontier 与 fallback、generation-local sticky fault、reserved start 与
封闭的私有 execution route。Issue #76 增加 process-owned `RunLifecycleRegistry`、Graph lifetime
anchor/lease、单调 Graph close、显式 process execution shutdown、精确 lifecycle/resource
settlement 与 source-private 有界 telemetry。

本决策会细化 ADR 0003，并在详细所有权与生命周期契约上取代它。ADR 0003 仍作为“把物理执行
资源移出每个 Graph”的历史高层决策保留。ADR 0001 继续完全有效。

## 背景

当前实现在每个 `GraphRuntime` 中为各 intent 保留一个复制的 execution-route binding。封闭的
`cpu`、`serial_debug` 与 `gpu_pipeline` route id 会命名私有 Host 实现；`GraphRuntime`
不拥有其中任何 worker、ready queue、generation、completion adapter、异常发布或 ordering
policy。Policy binding 是 process/service state，而不是 Graph state。

显式注入的 service 直接拥有一个固定 CPU worker pool 和一个私有 Metal worker lane。它会在首次
使用前冻结一个解析后的 `[1,8]` CPU 数量，接受多个 active Run，使用 policy-aware、按
entry/byte 有界的 ready store，
并按 Run 隔离 completion、first exception、in-flight drainage、trace Host 与 settlement。其私有
ledger 拥有不可变 composition limit，并为完整 Run vector 提供 authority。固定 service thread 与
私有 route machinery 属于基础设施，不再是 Run-lifetime 计费。

`ComputeRun` 的共享 control 拥有计划或 dirty staging、临时结果与稳定 lease。每个内建 CPU
ready task 都保留不可伪造 Run lease 与 `(RunId, RunLocalTaskId)`；failure publication 由匹配
identity 守卫。Full HP 使用 Run-owned `TaskSubmissionPlan` callback。Dirty HP、RT 与
connected-parameter preflight 会具化 heap-owned context 和 move-only
`ReadyTaskSubmission`，因此没有 stack `TaskExecutor*` 跨越 service 边界。三条私有 route 都使用
同一条 ready-store、policy、reserved-start、Run-lease 与 completion path。

Route-aware planning 会在准入前冻结选中的 implementation 与 `Device`。`cpu` 和
`serial_debug` 只暴露 CPU；Host 报告 Metal 时，`gpu_pipeline` 依次暴露 Metal、CPU，否则只暴露
CPU。CPU 与 Metal submission 进入彼此独立的固定 lane，但共用 Run grant 与 maximum-parallelism
ceiling。Graph、policy binding 或 operation provider 都不拥有 lane 或第二套 device-capacity
authority。

ADR 0003 已选择 request-owned `ComputeRun`、process-owned `ExecutionService`、
host-owned `ResourceLedger` 和仅负责比较的 policy seam 这一方向，但没有裁定：

- 一个 HP/RT 请求究竟拥有一个 Run，还是多个 domain Run；
- 配对 RT/HP 的 outcome、sibling cancellation、commit ordering 与 caller completion 如何聚合；
- 哪些输入不可变，哪些请求局部对象需要稳定存储；
- Run 状态机、终态以及取消/提交竞争；
- ready submission 和 completion routing 的精确边界；
- 目标 `GraphRuntime` 的正向与负向所有权；
- 哪个 owner 提供一个原子的 admitted-Run/Graph-close/process-shutdown 生命周期 fence；
- 谁可以铸造和释放资源 reservation 与 grant；
- Graph close 与进程 shutdown 各自的 drain 范围；以及
- issue #66–#76 所依赖的契约。

如果继续让这些选择保持隐含，各个迁移切片就可能构建出互不兼容的生命周期、队列、token 权威和
提交规则。

## 决策

### Run 身份与请求关系

每个可以独立执行的单 domain 计划都有一个 `ComputeRun`。非实时 HP 请求拥有一个 Run。
协调 HP 与 RT sibling 的请求拥有一个 request/run-group 身份，并且每个 domain 拥有一个
child Run。各个 child Run 具有独立的身份、计划、dispatcher、staged output、resource
reservation 和终态；grouping 永远不会创建 HP 到 RT 或 RT 到 HP 的任务依赖。

Request-owned `RunGroup` 是协调记录，不是 mixed-domain Run。其稳定 control block 拥有 group
identity、每个 child 的一个 observation `RunLease`、sibling gate、cancellation fan-out、
exact-once aggregate arbiter 与 caller promise；不拥有 child plan、dispatcher、staged output 或
resource reservation。它的成功 caller-visible payload 是 RT child output，但只有两个 child 都
成功时 group 才成功。Group 会在两个 child 终态都已知后，按以下确定性顺序选择一个聚合 outcome：

1. 任一 child 失败都会使 group `Failed`；
2. Host 分类的 resource-exhaustion failure 优先于其他 failure；在选中的 failure class 内由 RT
   failure 打破平局；
3. 如果没有 child 失败，任一 child 取消都会使 group `Cancelled`；只有当 fan-out 确实赢得至少
   一个仍开放的 child terminal arbiter 时，group-origin 的 lifecycle/explicit cancellation
   reason 才优先于 child-only reason。Group 的 monotonic request arbiter 会为 traceability
   保留第一个被接受的 group-origin reason，但两个 child 都已终态后的 request acceptance 不能
   替换其 aggregate。在真正赢得 child 的 group request 中，第一个被接受的 reason 保持稳定；
   其他情况下 RT child reason 优先于 HP child reason；
4. 只有两个 child 都成功时，group 才会携带 RT output 成为 `Succeeded`。

显式 request cancellation、Graph close 与 process shutdown 会把 cancellation 广播给两个 child。
RT 在提交前失败或取消，会永久拒绝 sibling commit gate，并请求取消 HP。HP failure 或
cancellation 不会回滚已发布的 RT proxy、不会覆盖 RT child 的终态，也不会请求取消 RT；RT child
仍会排空到自己的终态，而在没有更高优先级 outcome 时，group 会报告按上述规则选择的
HP-derived failure 或 cancellation。

两个 child 终态都已知后，group 可以冻结聚合 outcome；但 caller future 只有在两个 child 都达到
物理 quiescence、graph finalization 已提交或丢弃全部 staged output、reservation/grant 已恰好释放
一次、两个 admission attempt 都已完成处理，并且每个已安装的 admitted-Run registry entry 都已
注销后才能完成。Caller-facing future 只存储复制后的 aggregate outcome/output value，绝不持有
`RunLease`；group 会把释放 child observation lease 作为最后一项 settlement action，之后才把
future 标为 ready。丢弃 caller future 不会取消任何 child。
非 realtime 的单 Run future 对其唯一 Run 使用相同规则。

`RunId` 是不透明且稳定的，在拥有它的 `ExecutionService` 生命周期内不会复用。任务通过
`(RunId, RunLocalTaskId)` 标识；local task id 在其 Run 之外没有意义。Request、parent 和
run-group id 可以记录来源，但不能替代 Run 的唯一身份。

在异步规划或执行之前，Run 会捕获一个不可变 descriptor，至少包含：

- Graph 身份与 `GraphRevision`；
- 请求 target 和输入 snapshot；
- `ComputeIntent` 与 quality；
- QoS class、使用单调时钟的 deadline、weight 和 maximum parallelism；以及
- 可选 supersession key 与 generation。

`ComputeIntent`、QoS、资源策略和提交策略保持独立，彼此之间不得相互推断。

### 单调状态与唯一终态

当前私有 Run 截至 Issue #76 已采用的阶段推进顺序如下：

```text
Created -> Admitted -> Queued -> Running -> CommitPending -> Terminal
```

安全路径可以跳过部分非终态，包括 cache hit、admission failure 和提前取消路径。Run 永远不会
倒退，也不会离开 `Terminal`。

只能发布以下一种终态：

- `Succeeded`：只在经过验证的可见提交或经过验证的 no-op 成功之后；
- `Failed`：携带精确的 host-owned failure 或 exception category，包括 admission failure；
  或
- `Cancelled`：携带稳定原因。当前私有 source 覆盖显式请求、过期 monotonic deadline 与 Issue
  #74 supersession，以及 #76 Graph-close/process-shutdown source wiring；

Operation completion 本身不等于成功。Dispatcher 必须完成依赖聚合，并且 graph-state 提交
事务必须验证 Run predicate。

Issue #73 使 cancellation intent 与 terminal contention 成为当前行为。取消、失败和提交竞争者
共用一个 Run-owned terminal arbiter。第一个被接受的 claim 获胜；后续 claim 只能完成清理与
telemetry。提交验证、可见发布和 success claim 组成同一个 graph-state 串行事务，并共用同一个
terminal gate：

- 如果先接受取消或失败，就拒绝提交；
- 如果先接受并发布一次有效提交，后续取消就是 no-op；
- 迟到的 task 或 device completion 永远不会改变终态。

终态发布与物理 quiescence 是两个不同条件。已取消或失败的 Run 仍可能等待不可抢占工作排空。
Run 回收必须等到 quiescence 与资源释放，而不能只看终态是否可见。

### 稳定 Run 存储与 lease

Run control block 稳定拥有：

- 不可变 descriptor 与单 domain 计划；
- dispatcher dependency counter 与 dependent map；
- Run-local task namespace 与 completion endpoint；
- temporary result 与 staged output；
- first-failure/exception、cancellation 和 terminal-arbiter 状态；
- commit policy 与捕获的 revision；
- reservation、grant 与 Run-scoped accounting；以及
- 终态与清理 telemetry。

`RunLease` 是指向该 control block 的强、不可伪造生命周期 lease。它既不是 Graph 所有权，
也不是资源 token。每个已接受的 ready-store entry、正在执行的 callback、completion record、
dispatcher continuation 与 commit continuation 都会拥有或转移一个 lease。

Enqueue rollback 会释放候选 lease。Dequeue 会把 ready-store lease 转移给执行阶段。
Completion routing 会保留 lease，直到匹配的 dispatcher 接受结果。

只有满足以下条件时，Run 才能以非抛出方式析构：

1. 已发布一个终态；
2. ready、running、completion、dispatcher 和 commit 工作全部 quiescent；
3. 所有 `RunLease` 都已释放；
4. 所有 reservation 与 grant 都已恰好释放一次；
5. 不再有 continuation 可以发布 Run 或 Graph 状态。

析构不会隐式取消、发布 outcome，也不会等待本应仍持有 lease 的工作。丢弃 caller future 或
observer 不会取消已 admission 的 Run；取消必须显式发生，或由生命周期策略触发。

### Dispatcher 拥有 ready release

ADR 0001 继续治理派发边界。只有 `ComputeTaskDispatcher` 拥有：

- 任务依赖与 counter；
- ready detection 与 dependent release；
- source-first dirty release；
- temporary-result indexing；
- completion aggregation；以及
- 新 ready submission 的创建。

`ExecutionService` 只接受计算依赖已经满足的 `ReadyTaskSubmission`。Submission 包含不可变
执行 metadata、`(RunId, RunLocalTaskId)`、owned 或其他形式的稳定 executable handle、
资源 estimate/requirement，以及绑定到匹配 completion endpoint 的 `RunLease`。

当前 Issue #70 与 #71 的值实现了不可变 metadata、复合 identity、owned executable、匹配
`ComputeRunLease`、多 Run 路由，以及隔离的 completion/failure settlement。当前 service 会拒绝
borrowed handle、匿名 raw callback、混合 initial Run id，以及不属于匹配 active Run 的
submission。`TaskSubmissionPlan` 与 heap-owned dirty context 仍负责发现 initial readiness、
释放 dependency、拥有 result index 并减少 logical completion count。每个 submission 都携带统一
trusted-host demand；完整 Run admission 与 checked aggregation 会在 publication 前发生，
initial/dependent submission 都需要来自同一个有界 store 的匹配 child grant。两条路径进入同一
policy route，并且 Run 的 fairness row 会跨越临时为空的阶段，一直保留到 Run 最终退休。

Service 永远不会接收或派生 `GraphModel`、`ComputePlan`、`ComputeTaskGraph`、dirty
snapshot/state、dependency map、cache authority 或可见 commit authority。
`TaskCompletion` 通过 Run lease 返回到匹配 dispatcher；dispatcher 必须在改变依赖状态前验证
Run 与 local-task namespace。新释放的 dependent work 会重新进入进程 admission、有界 ready
store 和全局 policy；永久 worker-local 路径不得绕过公平性、取消或 Run 隔离。

### 当前所有权边界

| 所有者 | 拥有 | 不拥有 |
| --- | --- | --- |
| Request / `RunGroup` | group identity、child observation lease、sibling gate、cancellation fan-out、aggregate outcome/error selection、caller promise | child plan/dispatcher/terminal arbiter、cross-domain dependency、Graph state、进程 worker、resource reservation |
| Request / `ComputeRun` | Run 身份、不可变输入、请求计划与 dispatcher 状态、staged/temporary output、exception/cancellation/terminal 状态、Run reservation、commit policy、Run telemetry | Graph state、进程 worker、ready-store policy、资源铸造权威 |
| `GraphRuntime` | `GraphModel`、graph-scoped state、graph-state lane、单调 `GraphRevision`、revision capture、串行 commit validation/publication、graph event、稳定 graph-instance identity、复制的 HP/RT route id 与 generation、graph-lifetime anchor、platform/session metadata | Run、admitted-Run index、CPU/device/I/O/plugin worker、进程 ready store、admission、`ResourceLedger`、`PolicyRegistry`、policy binding、物理 execution route |
| `ExecutionService::RunLifecycleRegistry` | 一个 process admission fence、service accepting/stopping state、按 Graph 建索引的 open/closing admission row、pending admission candidate、按 Graph 建索引且已 admission 的 `RunLease` entry，以及 process-wide Run enumeration | Run plan、dispatcher、terminal arbitration、staged output、Graph state、resource minting、execution policy |
| 进程 `ExecutionService` | lifecycle registry、固定 CPU pool、一个私有 Metal lane 与后续 resource executor、私有 serial-debug/GPU route、policy-aware 有界 ready storage、policy-binding state、Run/resource admission、policy 结果验证、reserved start、执行异常 fence、completion routing | 任务规划/依赖、Graph/document persistence、cache authority、dirty propagation、可见 commit、Graph state |
| `ResourceLedger` | checked composition limit、事务型 reservation、经过验证的 child grant、exact-once release accounting | 排序策略、任务依赖、Graph state、向第三方委托 token |
| 进程 `PolicyRegistry` | immutable built-in/DSO policy type record、经过验证的纯 C callback table、registry visibility、DSO lease | service binding/context、ready work、worker、resource、Graph/Run state、completion 或 lifecycle authority |
| Policy binding | 在 service-owned binding state 中对 Host 编写的 immutable candidate descriptor 排序 | worker、物理 ready store、Run、Graph state、budget、reservation/grant/token、native device handle、executor、completion 或 lifecycle authority |

产品 composition root 现在会构造并注入当前固定 lane `ExecutionService`；它不是静态
singleton。测试会创建和销毁彼此隔离的 domain。Composition-root execution configuration 会解析并
冻结 worker 数量；policy plugin 不能请求、授予或调整该 pool。Graph load、route replacement、
Run submission 与 dirty 阶段都不会调整 pool 大小。Graph-indexed `RunLifecycleRegistry` 现在是
唯一 admitted-Run index 与 admission/close/shutdown fence；原先只覆盖 physical execution 的 weak
active-Run map 已删除。

Composition root 会先构造 service，再构造被注入的 Kernel/Host，并让 service 存活到这些 owner
停止 Run admission 且排空所有 Run。Planning、persistence、cache、dirty propagation 与
可见提交继续位于这个深模块之外。

### Admitted-Run registry 与 lifecycle fence

`ExecutionService` 把一个私有 `RunLifecycleRegistry` 作为自身 admission 子系统的一部分。这是
权威选择，而不是使用 Host-adapter registry 或 composition-global singleton：所有参与其中的
Host/Kernel 都使用同一个注入 service，测试仍可构造隔离 service。Graph 打开时，
`GraphRuntime` 会向 registry 注册一个稳定、不复用的 `GraphInstanceId` 与 graph-lifetime
anchor。面向用户的 session name 不是 registry key，不能产生 close/reopen ABA。

Run admission 与 Graph close 使用以下两阶段协议：

1. `ComputeService` 创建处于 `Created` 的 child Run，但不让它表现为已 admission。
2. 在 registry 的单一 lifecycle fence 下，`begin_graph_admission` 检查 service 为
   `Accepting` 且 graph row 为 `Open`，记录 pending admission candidate，并从已注册 anchor 获取
   graph-lifetime lease。正在关闭的 Graph 必须等待该 candidate 提交或回滚。
3. 该 lease 保持 target 存活时，graph-state lane 捕获权威 `GraphRevision`；随后 planning 可以在
   lane 外构建 immutable descriptor 与 resource request。
4. 可信 service code 获取完整 ledger reservation，或者什么也不获取。`commit_admission` 在同一
   lifecycle fence 下重新检查 service 与 graph state，原子消费 candidate，在 Graph 与 process
   两个 index 中安装一个 registry-owned `RunLease`，把 graph-lifetime lease 与 reservation
   转移给 Run，并推进 `Created -> Admitted`。
5. Registry 安装成功就是 Run admission 的线性化点。任何 recheck 或 resource failure 都会恰好
   一次回滚 reservation、pending candidate 与 graph lease；未 admission 的 Run 会向 caller 发布
   精确 failure/cancellation，但不会出现在 admitted index 中。

Graph close 会在同一个 fence 下把 graph row 从 `Open` 改为 `Closing`。如果 registration 先
线性化，close 会在 graph index 中找到该 Run；如果 close 先线性化，`commit_admission` 会拒绝
candidate，close 则等待其回滚。因此不会迟到 admission，也不会漏掉已 admission Run 或 in-flight
candidate。Process shutdown 使用同一个 fence 把 service 改为 `Stopping`，并把每个 graph row
改为 `Closing`，因此 global 与 graph-local admission 不会发生分歧。

Visible commit 会先进入 graph-state lane，只有在最终 open-row/registered-Run validation 与
publication 时才取得 lifecycle fence。Graph close 绝不会在持有 lifecycle fence 时等待
graph-state lane：它先标记 `Closing`、释放 fence，再执行 drain。因此 commit publication 与
closing transition 具有唯一顺序——先完成验证的 commit 可以在 close 前完成，先完成标记的 closing
会拒绝 commit——且 lock order 不会形成 registry/lane cycle。

每个 registry entry 只拥有一个 `RunLease` 与不可变 identity/index metadata，不拥有也不检查
plan、dispatcher、staged output、terminal arbiter 或 reservation。Run 保留自己的
graph-lifetime lease 与 resource owner。只有在 Run 已终态、所有
ready/running/completion/dispatcher/commit path 都已 quiescent、graph commit 或 discard
finalization 已完成，并且所有 reservation/grant 与 Run-owned graph-lifetime lease 都已恰好释放
一次后，可信 finalization path 才能注销 entry。Unregistration 只释放 registry 的
`RunLease`；只有此后 Graph close 才能观察到 index 与 candidate count 均为空，caller future
也才能发布已 settle completion。

### Host 权威的资源核算

`ExecutionService` 独占拥有一个内部 `ResourceLedger`，并用 composition-root limit 初始化。
只有可信 host code 可以铸造其 move-only、不可伪造 reservation 与 execution grant。
内建或第三方 policy、operation plugin 或 policy plugin 可以请求或建议资源，但不能构造、
复制、扩大或直接释放 token。

当前 Issue #70 与 #71 service 与 ledger 会核算：

- CPU 执行容量；
- ready-store entry 与 byte；
- retained/in-flight Host memory 与 scratch。

以下维度仍是后续目标行为；当前 ledger 不会猜测、预留，也不会用虚构的非零值表示它们：

- 每 device 的 queue、in-flight、memory 与 scratch 容量；
- compute-I/O operation 与 byte；以及
- plugin-process、invocation、IPC/shared-memory 与隔离容量。

Admission 会事务性验证一个 checked resource vector，只返回完整 Run reservation 或什么也不
返回。可信 service code 会在该 reservation 内部分配 ready-entry/byte 与 CPU/memory/scratch
child grant。Service 从 general admission ceiling 中减去配置的 interactive headroom 后，只把
active Throughput root reservation 计入该 ceiling。Interactive Run 不会扣减这项 class quota，但
每个 reservation 与 grant 仍必须由 ledger 授权，ledger 也仍是唯一物理容量 authority。
Throughput quota check、ledger reservation 与 class
charge 构成一个串行 transaction。不具权威的 class charge 只有在精确物理 root release 时才会
扣回，包括 live child grant 推迟该 release 的情况。

当前 success、callback failure、construction rollback、worker failure、Graph close 与 Host
destruction 都会让每个 reservation/grant 恰好释放一次。只要 child grant 仍存活，Run
reservation 就不能释放。Checked overflow 或 capacity exhaustion 永远不会导致 overcommit、部分
reservation 或静默 clamp。同步文档化 allocation exhaustion 继续表现为 `std::bad_alloc`；异步
failure 由精确 Run failure channel 捕获，且不能提交部分输出。Issue #73 cancellation 与 Issue
#76 lifecycle finalization 会保持同一个不变量。

原 worker-only counter 已完整删除，不会被包装、重命名、alias，也不会作为第二 authority 保留。
Execution worker-count resolution 只属于 composition-root configuration；所有 Run admission 都使用
`ExecutionService` ledger。

### Policy generation 与私有 execution route

`ExecutionService` 拥有恰好一个 Interactive 与一个 Throughput policy binding，以及全部物理
ready entry 与 fairness row。每次 start 按
`work_units + ceil(complete_ready_grant_bytes / 4096)` 计费；每个 Graph 在已选 class 的
accumulator 中按原始 cost 计费，每个 class 不可变的 Run row 按 `ceil(cost / weight)` 计费。
Service 会选择 class、为每个 live Run 最多暴露一个 lane head、应用 age/deadline/projected-service
frontier，并在 Throughput 仍可 start 时最多允许三个连续 Interactive start。Initial work 与新释放
的 dependent work 进入同一路径，Run row 会跨越临时为空的阶段，一直保留到最终退休。QoS
class、deadline 与 weight 是显式 descriptor 输入，不从 intent、quality 或 maximum parallelism
推断。

Built-in 与 DSO policy 使用同一条 Host 编写的 frontier 与 decision validation path。DSO policy
通过自包含的 C11 policy ABI v1 只接收 immutable scalar candidate snapshot，并且只能选择一个
original candidate 或 abstain。它不拥有 worker、ready entry、token、grant、reservation、budget、
executor、Run、Graph、completion route 或 lifecycle authority。Host 对 obsolete valid decision
最多重试两次，之后采用同 class built-in choice。首个无效 DSO decision 对精确 binding generation
保持 sticky；成功 replacement 会创建新的 nonzero generation，并清除该 fault。

选中的 candidate 不是 execution authority。Reserved start 会重新检查精确 ready entry 与 route、
使用 no-throw ownership 暂存 grant，并在任何 executor callback 开始前提交 entry removal、
ready-to-execution grant exchange、fairness/burst accounting、in-flight state 与 callback transfer。
封闭的 `cpu`、`serial_debug` 与 `gpu_pipeline` route 会保持其 worker、queue、device 与
completion adapter 私有。拥有 worker 的 scheduler ABI、SDK target、`IScheduler` hierarchy 与
per-Graph 物理 owner 已通过一次完整破坏性迁移被移除，没有保留 adapter、forwarding API 或旧
worker-count grant。

每条 planning path 都会在 Run 发布前冻结选中的 operation callable 与 device。如果 device 不在
已配置 route/Host inventory 中，service 会在安装 active Run state 前拒绝它。CPU fallback 使用
固定 pool，Metal 使用单一 GPU lane。Completion、exception、cancellation、reuse、shutdown 与
drainage 会恰好一次退役共用的 ledger 和 Run state。

### Revision、staged commit、取消与 supersession

Issue #72 使最小 revision 子集成为当前行为。`GraphRuntime` 会针对与计算正确性相关的 Graph
mutation 推进 checked nonzero `GraphRevision`，同时每个 live Graph 都有强类型、不可复用的
instance identity。Run 在 planning 前捕获二者，product work 使用 request-owned Graph/proxy
snapshot。Graph-state lane 只在 capture 和经过验证的可见 publication 期间持有，而不会覆盖长时间
planning/execution；私有 compute-request lane 会串行化同一 Graph 的 request，但不拥有 executor
或 policy state。Issue #73 使私有 cancellation 与 commit-terminal arbitration 成为该当前
边界的一部分。

当前 commit predicate 与串行化 terminal transaction 要求：

- `CommitPending`、预期 domain 与 graph label，以及精确 staged Graph/proxy owner；
- staged identity/revision 与 immutable Run descriptor 相等；
- live Graph identity/revision 与该 descriptor 相等；以及
- 请求 domain 具有有效 staged output；以及
- 重新观察显式/deadline cancellation 后，在 terminal arbiter 仍开放时接受 Run-owned commit
  contender。

Commit contender 会在 graph-state work item 内、符合条件的 persistence 或 visible publication
之前被接受。先接受 cancellation 或 failure 会阻止该 claim，且不会发布任何 Graph、proxy 或
deferred cache state。成功 publication 会保留 authoritative revision，并在串行 work item 返回前
把 contender 解析为 Run success；predicate 或 persistence failure 会把它解析为精确 Run failure。
后续 cancellation 是 no-op。验证失败会丢弃 staged output，且不能修改可见 Graph/proxy state 或
写入 deferred cache artifact。

Issue #74 已在该 predicate 上增加：精确 supersession key/generation 与 live Graph coordinator
相等。Issue #76 增加当前最终检查：registry Graph row 为 `Open`、Run 已注册且 Graph lifetime
lease 仍有效。

不能因为 topology 相等或输出相似，就推断 revision compatibility。任何未来 compatible-revision
优化都需要新的显式决策。

当前 #74 supersession path 会让较新 generation 成为 current，并请求取消匹配的较旧 Run。它不会
修改旧 Run 的计划或复用其身份。不可抢占工作与外部副作用可以完成，但 stale、cancelled、failed
或 overdue output 不能提交。

对于 paired realtime request，request-owned `RunGroup` gate 是一个单调三态 latch：
`Pending`、`RtCommitted` 或 `Denied`。经过验证的 RT publication 会执行 `Pending ->
RtCommitted`，而 publication 前的 RT failure、cancellation 或 supersession 会执行 `Pending ->
Denied`；cancellation 还会请求 HP cancellation。即使迟到 RT work 完成，`Denied` 也永远不会重新
开放。HP child 会应用独立的 revision、generation、cancellation、terminal 与 staged-output
predicate，因此之后出现 stale 或 cancelled HP result 不会回滚 RT。Graph-close 与
process-shutdown denial source 现在会 fan-out 到两个 child。

### Close 与 shutdown 范围

当前 Graph close（#76）：

1. 在 lifecycle-registry fence 下把 graph row 改为 `Closing`，停止该 Graph 的新/pending Run
   admission 与普通外部 graph-state admission，并且只为已经 admission 的 Run settlement
   保留 lifecycle admission；
2. 等待 fence 前 candidate 先于 close 注册或回滚，再枚举完整的 graph-indexed admitted-Run set；
3. 拒绝 visible commit，并只为这些 Run 请求 cancellation 或 drain；
4. 允许这些 Run 的 completion、cancellation 与 commit continuation 进入 graph-state
   finalization path，但只能观察 closing、丢弃 staged output、发布 terminal state 和释放
   ownership；
5. 等待 terminal outcome、物理 quiescence、graph finalization、exact resource release、
   admitted-Run unregistration 与 graph-lifetime lease release；
6. 移除空 registry row，在 graph-state finalization 仍可用时停止并排空私有 per-Graph
   compute-request lane，再停止并排空 graph-state lane、销毁 Graph state，但不停止任何
   process-owned execution route；
7. 让 `ExecutionService` 和无关 Graph Run 继续运行。

实现会把 registry fence 与本地双 lane 顺序组合起来，而不会恢复旧的 single-lane 模型。Graph
close 会注销每个 Run、移除空 row、停止/排空 compute-request lane，再停止/排空 graph-state lane
并销毁 Graph state。Graph destruction 不会停止 process-owned route，marker 后也不存在 reopen。

当前进程执行域 shutdown（#76）：

1. 在同一 registry fence 下把 service 改为 `Stopping`，把每个 graph row 改为 `Closing`，
   停止全局新 Run admission 与普通外部 graph-state work admission；
2. 等待所有 pending candidate 先于 shutdown 注册或回滚，再枚举完整的 process admitted-Run
   set；
3. 为每个已经 admission 的 Run 选择 cancellation 或 drain，同时只为这些 Run 保留有界
   ready submission、execution、completion routing 与 graph-state finalization；
4. settle 并注销每个 Run，恰好一次释放其 graph/resource lease；
5. 在 registry 为空且 Run quiescent 后停止其余 ready/execution admission；
6. join 所有物理 executor；
7. 销毁 `ExecutionService`。

Worker 与 operation exception 会在执行边界被 fence，并通过匹配 Run lease 路由。它们不能逃逸
worker thread、使不同 Run 失败或跳过资源释放。终态发布或 Graph close 后到达的 completion
只能执行清理。

## 交付边界

本决策冻结依赖契约。Issue #66 到 #76 已作为当前切片实现；#76 完成以下顺序。
Non-goal 列记录各切片交付时的历史边界，不描述仓库当前状态：

| Issue | 使用本决策的部分 | 交付时的历史非目标 |
| --- | --- | --- |
| #66（当前） | HP `ComputeRun` descriptor、state、storage 与唯一终态 | 进程 worker 迁移 |
| #67（已完成的基础） | 稳定 Run lease 与 `(RunId, RunLocalTaskId)` full-HP completion 隔离 | 共享 CPU service |
| #68（已完成的基础） | 显式注入的单 Run CPU-only `ExecutionService`，只接受 ready 输入 | 多 Graph 迁移与最终 ledger |
| #69（已完成） | 多 Graph 与 HP/RT 共享 CPU domain；删除 per-Graph 内建 CPU worker；dirty/preflight 使用 owned submission | 完整 admission/policy 模型与 `RunGroup` |
| #70（当前） | Production resource admission、有界 ready store 与 `ResourceLedger` | 公平性算法、lifecycle registry 与新的 device/I/O/plugin 维度 |
| #71（当前） | Interactive 与 throughput 内建 policy | Plugin ABI 迁移、revision 偏好、取消与 supersession |
| #72（当前） | `GraphRevision` capture 与 staged commit predicate | Cooperative cancellation |
| #73（当前） | Queued/running/commit cancellation，汇合 #70 与 #72 | Latest-wins policy |
| #74（当前） | #71 与 #73 之后的 latest-wins supersession 与 realtime `RunGroup` | Scheduler ABI 替换 |
| #75（当前） | #71 之后的纯 C policy generation、Host frontier/fallback、reserved start 与私有 execution route | 永久 old/new adapter |
| #76 | #69/#73/#74/#75 之后的 Graph close、进程 shutdown、telemetry 与最终不变量 | 新执行域能力 |

依赖图无环。#72 已获准在 #67 之后与 #68–#71 并行；#75 已在 #71 之后与 #73–#74 路线一同交付。
后续 release gate 仍需独立审核与远端集成；这不会改变当前 runtime ownership 事实。

## 结果

- 请求局部状态可以安全超过 caller stack 生命周期，而不需要把任务图正确性转交给执行 service。
- 不同 Run 可以复用 local task id，不会产生 completion 或 exception 串扰。
- 内建 CPU worker 数量不再由 Graph 数量决定，同时 Graph close 仍保持 graph-scoped。
- 一个可信 ledger 成为每种物理资源维度的最终权威；policy 与 plugin code 无法铸造容量。
- 取消可能在不可抢占工作被回收之前发布终态，因此测试和 telemetry 必须区分 terminal 与
  quiescent。
- 精确 revision equality 很保守，可能丢弃可复用工作；任何放宽都需要新的显式决策与证据。
- 已完成的 scheduler-to-policy ABI 迁移有意采用破坏性方式。
- Device、I/O、general data、plugin isolation 和 server control plane 细节可以继续演进，
  而无需改变 ready-task 或资源权威边界。

## 可测试性与故障不变量

随着行为落地，长期测试必须覆盖：

- Run id 与 local-task 隔离；
- 单调阶段与 exact-once 终态竞争；
- caller observer 析构与 lease-backed 生命周期；
- 配对 RT/HP 的聚合 outcome 优先级、非对称 sibling propagation、RT-first commit-gate
  竞态、两个 child 都成功后的迟到 group cancellation，以及 caller-future settlement；
- ready-only submission 与 dispatcher-owned dependent release；
- Graph-close/admission 和 process-shutdown/admission 线性化，包括 pending-candidate rollback
  与 admitted-Run registry unregistration；
- 有界进程 admission 与 fail-closed arithmetic；
- 所有成功、失败、取消、rollback、close、shutdown 和 worker-failure 路径的精确释放；
- revision-safe staged commit 与不可抢占取消；
- policy 下的 interactive/batch 进展；
- graph-local close 与 process-global shutdown；以及
- 无法污染另一个 Run 的异常路由。

CTest 与 CI 继续只用于长期软件行为。不得注册或保留 issue-specific replay、provenance、
migration-residue、phase-completion 或 result orchestration。

## 被拒绝的替代方案

### 为 HP 与 RT 创建一个 mixed-domain Run

拒绝，因为 HP 与 RT 拥有独立计划、staged output、commit role 与终态清理。Group 可以协调它们，
而无需创建 cross-domain task dependency。

### 把 dependency counter 移入进程 ready store

拒绝，因为它违反 ADR 0001，并会让执行策略依赖 Graph 与 task-graph 语义。

### 通过更长的 wait 延长 borrowed TaskExecutor 生命周期

拒绝，因为进程队列、device completion、compute I/O 与跨 Run 公平可能超过 caller stack
及该 wait 的生命周期。

### 把旧的 worker-only counter 复用为 ResourceLedger

拒绝，因为这个已删除的过渡计数器资源模型错误、采用隐藏的进程静态所有权，并且不拥有 Run、
queue、memory、scratch、device、I/O 或 plugin-process 权威。

### 允许 policy callback 铸造 grant

拒绝，因为不可信或有缺陷的策略可能 overcommit 资源并规避 exact-release accounting。

### 在 adapter 后保留 worker-owning scheduler ABI

拒绝，因为物理 worker 与 token 会继续位于唯一 host-authoritative 执行域之外。

### 让 GraphRuntime 或每个 Host adapter 拥有 admitted-Run registry

拒绝，因为由 Graph 拥有会乘法增加 process lifecycle state，并可能停止无关工作；per-adapter
Host registry 则不能为所有被注入 owner 提供统一的 process admission/shutdown fence。因此，
registry 是唯一私有 `ExecutionService` admission subcomponent，不是 public 或 static
composition object。

## 与其他决策及文档的关系

- [ADR 0001](0001-graph-state-access-is-not-scheduler-dispatch.zh.md) 继续治理
  graph-state/ready-dispatch 分离。
- [ADR 0003](0003-process-owned-execution-resources.zh.md) 继续作为历史高层决策，只在目标所有权
  与生命周期细节上由本 ADR 取代。
- [ADR 0006](0006-kernel-documentation-separates-facts-decisions-targets-and-status.zh.md)
  要求当前事实、本 accepted target 决策、roadmap 方向与 Issue/Project 状态保持分离。
- [内核演进目标](../../roadmap/zh/Kernel-Evolution.zh.md) 记录持久目标与交付依赖顺序。
- 当前行为（包括 issue #69 的固定多 Graph HP/RT CPU pool、issue #70 的 admission/ledger 边界、
  issue #71 的 policy-aware ready store、issue #72 的强类型 Graph revision 与 staged
  publication 边界、issue #73 的私有 cooperative cancellation 与 Run-owned commit
  arbitration、issue #74 的 latest-wins generation、有界 ticket-backed coalescing、
  current-generation commit predicate 与 realtime `RunGroup`，issue #75 的纯 C policy ABI、
  Host 编写的 frontier/fallback、reserved start 与私有 execution route，以及 issue #76 的
  lifecycle registry、精确 Graph close/process shutdown 与 source-private telemetry）继续以
  [计算边界](../../kernel-architecture/zh/Compute-Boundaries.zh.md)、
  [计算流程](../../kernel-architecture/zh/Compute-Flow.zh.md) 和
  [策略与执行架构](../../kernel-architecture/zh/Policy-and-Execution-Architecture.zh.md) 为权威。
  Issue #76 lifecycle/telemetry 已是当前实现行为；独立交付审核与远端集成仍是 release gate，
  不属于 runtime ownership 语义。
