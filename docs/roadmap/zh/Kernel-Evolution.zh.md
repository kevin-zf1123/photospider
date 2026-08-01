# 内核演进目标

## 状态与范围

本文记录已接受的合并后架构方向。它是目标，不是当前软件行为说明，也不是实施任务清单。
当前事实仍由 `docs/kernel-architecture/` 说明；架构决策记录在 `docs/adr/`；实施状态只由链接的
GitHub Project 和 Issue 跟踪。

[ADR 0006](../../adr/zh/0006-kernel-documentation-separates-facts-decisions-targets-and-status.zh.md)
定义上述分离与提升流程。每个交付切片都要引用其当前状态基线、governing ADR、精确目标章节、
实时 Project/Issue 状态和实际验证结果。完成交付项本身不会让目标变成当前行为；只有实现与长期
测试支持该行为时，才会修改对应维护中架构文档。

当前分支定位为本地、单用户、embedded 或 Unix-socket sidecar 基线。在 Photospider 被描述为
通用数据流内核、低延迟交互引擎或多 session server runtime 之前，应完成本文目标。

## 开发领域

| 领域 | GitHub Project | 父 Issue | 目标结果 |
| --- | --- | --- | --- |
| 依赖中立内核 | [kernel-dependency-decoupling](https://github.com/users/kevin-zf1123/projects/2) | [#51](https://github.com/kevin-zf1123/photospider/issues/51) | 内核 geometry、value、buffer、graph document 和 cache 行为不再使用 OpenCV 或 YAML 作为语义语言。 |
| Run 与进程执行域 | [compute-run-execution-domain](https://github.com/users/kevin-zf1123/projects/3) | [#64](https://github.com/kevin-zf1123/photospider/issues/64) | Request-owned `ComputeRun`、process-owned CPU execution、资源账本、graph revision、取消和 supersession。 |
| 通用数据与异构执行 | [generic-data-heterogeneous-execution](https://github.com/users/kevin-zf1123/projects/4) | [#77](https://github.com/kevin-zf1123/photospider/issues/77) | `Value`、`DataDescriptor`、`BufferHandle`、`Region`、device queue、fence、transfer 和有界 compute I/O。 |
| 执行画像与安全服务 | [execution-profiles-server-isolation](https://github.com/users/kevin-zf1123/projects/5) | [#91](https://github.com/kevin-zf1123/photospider/issues/91) | 交互/吞吐画像、独立 server control plane、受限 worker 和隔离插件执行。 |

当前重构的合并门禁继续由
[codebase-refactor](https://github.com/users/kevin-zf1123/projects/1) 跟踪，并由
[Issue #42](https://github.com/kevin-zf1123/photospider/issues/42) 聚合。

### 当前 containment 基线

[Issue #43](https://github.com/kevin-zf1123/photospider/issues/43) 建立了最初的
scheduler-worker containment，
[Issue #44](https://github.com/kevin-zf1123/photospider/issues/44) 建立了有界 graph-state lane。
Issue #69 至 #75 此后以一个由 Host 组合的 execution domain、原子 resource vector、policy-aware
有界 ready storage、revision-safe staged publication、纯 C policy generation 与 Host 私有
execution route，替换了 worker-only containment 与拥有 worker 的 scheduler SDK。Visible
compute 现在会捕获完整的 request-owned state，并在 graph-state 之外执行；第二条有界串行 lane
会保持同一 Graph 的 request ordering，但不拥有物理 executor。
当前有界契约：

- 为每个 embedded Host 提供一个共享 CPU service 与一个 CPU 维度默认为 32 的 ledger，并使用
  完整且经过 checked arithmetic 的 CPU、retained-memory、scratch、ready-entry 与 ready-byte
  vector admission 每个 Run；
- 让 initial 与 dependent work 通过同一个受 entry/byte 约束的 ready store，并且只在 reserved
  start 时才把 ready authority 交换为 execution grant；
- 保持恰好一个 Interactive 与一个 Throughput policy binding，应用 Host 编写的
  class/frontier/fairness 规则，并针对 immutable original snapshot 与当前 Host state 验证每个
  built-in 或 DSO decision；
- 通过自包含的 C11 ABI v1 暴露 policy plugin，同时让 worker、queue、resource、Run/Graph state
  与 completion route 均不进入该 ABI；
- 让首个无效 plugin decision 对其精确 binding generation 保持 sticky，并通过同一条受信任的
  built-in selection path fallback；
- 把 `cpu`、`serial_debug` 与 `gpu_pipeline` 保持为封闭的私有 execution-route id；Graph
  只存储复制的 route id 与 generation，绝不存储物理 worker、queue、plugin context 或 policy
  binding；
- 按 work 与 ready-byte quantum 对 start 计费，维护分层 Graph/Run 公平性，让 ready work aging，
  保留 Interactive headroom，并在有界 Interactive burst 后保证 Throughput 进展；
- 用每 Graph 一个 worker、64 个等待任务的 FIFO 取代 graph-state async-per-submit，并通过阻塞
  backpressure 避免丢弃已经 admission 的 work，同时为每个 Graph 提供另一条具有相同上限的
  lane 来串行化 request；
- 为每个 live Graph 分配不可复用的强类型 identity 和 checked nonzero revision，并且只在二者
  精确相等后发布 product snapshot；
- 让 embedded close 先发布 Host marker、排空 marker 之前的同步 admission，再在等待 async
  placeholder 之前停止 compute-request admission，使满 FIFO producer 无法令 close 死锁；随后在
  graph-state 仍可用时排空 compute-request work 并排空 graph-state，而不会拆除 process-owned
  execution route。

默认 32 个 CPU slot 覆盖已 admission 的 Run execution grant。固定 `ExecutionService` thread 与
其私有 route machinery 属于基础设施。Ledger 不计算各自具有独立“每 Graph 一个
worker”上限的 graph-state executor 或 compute-request executor，也不声称覆盖 operation 内部
thread、daemon/frontend worker、全部 OS thread，或尚未声明的 device/I/O/plugin-process
resource。
Issue #70 已完全移除旧的
worker-only counter：`ExecutionService` 现在拥有唯一 Host 权威 ledger，在发布每个内建 CPU
Run 前以一个 checked full-vector reservation 完成 admission，并要求 initial 与 dependent work
存放在有界 ready queue 时持有 entry/byte grant。Issue #71 增加当前私有 interactive 与 throughput
策略、显式 QoS ordering、work/byte 计费、Graph/Run 公平性、aging、headroom 与有界 throughput
进展。Issue #72 增加强类型 Graph identity/revision、request-owned product snapshot、
exact-revision commit，以及 RT-first 独立 child publication。Issue #73 增加私有 cooperative
cancellation、monotonic deadline observation、精确 queued/running drainage 与
cancellation/commit arbitration。Issue #74 增加 request-level realtime `RunGroup`、checked per-Graph
latest-wins generation、有界 ticket-backed coalescing 与 current-generation commit authority。
Issue #75 移除 scheduler SDK，并增加纯 C policy ABI v1、原子 policy binding replacement、Host
编写的 frontier 与 fallback、generation-local sticky fault、reserved start 与私有 execution route。
Registry-owned close/shutdown cancellation、精确 settlement 与 lifecycle telemetry 已是 Issue
#76 当前行为。
[ADR 0007](../../adr/zh/0007-compute-runs-and-process-execution-have-separate-owners.zh.md)
是当前 Run 详细生命周期、owner 边界、resource mint、close/shutdown 作用域与交付依赖的权威来源。

## 架构原则

1. `ps::Host` 继续作为后端之外唯一产品 seam。
2. 图状态操作绝不作为 compute work 进入 ready store、policy selection 或 execution route。
3. Compute planning 拥有 topology、dependency、ROI、dirty selection 和 ready detection；
   scheduling 只看到具体 ready work 的不可变 metadata。
4. 语义 intent、资源 policy 和提交可见性保持分离。
5. 物理 CPU、GPU、I/O 和外部进程资源具有唯一显式的进程所有者和 Host 权威预算。
6. 外部库和文档格式通过 adapter 进入；其类型不定义 kernel geometry、value、planning 或 cache 语义。
7. Data descriptor、ownership、device synchronization 和 region 必须显式；不得依靠 opaque context
   恢复正确性所需事实。
8. 本地 sidecar、server control plane、worker runtime 和不可信插件执行是不同安全域。

## 目标所有权结构

```mermaid
flowchart TD
  HOST["Host / Kernel"] --> CAPTURE["图状态 lane：捕获 revision"]
  CAPTURE --> REV["immutable GraphRevision"]
  REV --> SERVICE["ComputeService"]
  SERVICE --> RUN["ComputeRun"]
  SERVICE --> PLAN["ComputeTaskPlanner"]
  PLAN --> GRAPH["ComputePlan / ComputeTaskGraph"]
  GRAPH --> DISPATCH["ComputeTaskDispatcher"]
  RUN --> DISPATCH
  DISPATCH -->|"ReadyTaskSubmission"| EXEC

  subgraph EXEC["Process-owned ExecutionService"]
    ADMIT["AdmissionController"] --> LEDGER["ResourceLedger"]
    LEDGER --> READY["Host-owned ReadyTaskStore"]
    READY --> POLICY["Policy binding / pure-C policy ABI"]
    POLICY --> ROUTER["Resource router"]
    ROUTER --> CPU["CPU executor"]
    ROUTER --> DEVICE["DeviceExecutorRegistry"]
    ROUTER --> IO["Compute I/O executor"]
    ROUTER --> PINVOKE["PluginInvocationExecutor"]
  end

  PINVOKE --> PSUP["PluginRuntimeSupervisor"]
  CPU --> COMPLETE["TaskCompletion"]
  DEVICE --> COMPLETE
  IO --> COMPLETE
  PINVOKE --> COMPLETE
  COMPLETE --> RUN
  COMPLETE --> DISPATCH
  RUN --> COMMIT["ComputeCommitPolicy"]
  COMMIT --> VALIDATE["图状态 lane：校验并发布"]
  VALIDATE --> VISIBLE["GraphModel / RealtimeProxyGraph"]
```

`Process-owned` 表示产品组合根中只有一个显式所有者，并不表示静态 singleton。Embedded test、
桌面产品和 worker process 必须能够确定性地构造、注入和销毁执行域。

图状态 lane 现在会先捕获 immutable revision，之后再校验 commit predicate。长时间 planning 与
execution 发生在 `GraphModel` 独占变更边界之外，因此一个 `ComputeRun` 不会阻止 frontend
产生更新 revision。Issue #72 使最小 identity/revision staging 行为成为当前行为，Issue #73 使
私有 Run cancellation 与 commit arbitration 成为当前行为，Issue #74 则使 request-level grouping
与 supersession generation 成为当前行为；Issue #75 使 policy generation、reserved start 与私有
execution route 成为当前行为；Issue #76 使 lifecycle registry、close/shutdown 与 telemetry 成为
当前行为。图中仍包含后续 device、I/O 与隔离 plugin 目标切片。

## Run 与进程执行域契约

[ADR 0007](../../adr/zh/0007-compute-runs-and-process-execution-have-separate-owners.zh.md)
细化 ADR 0003 的高层方向。本节汇总其持久目标约束；当摘要省略细节时，以 ADR 为权威。

### `ComputeRun`

当前截至 Issue #75 的基线会为每次非 realtime HP service call 创建且只创建一个私有 Run。Realtime
call 则会创建一个 request-owned `RunGroup`，其中包含彼此分离的 HP `Full` 与 RT
`Interactive` 子 Run。
每个 Run 都会捕获 process-lifetime opaque id、session label、强类型 Graph instance identity、
authoritative revision、target、intent、quality 与显式 QoS；拥有单调 phase 和 exact-once terminal state；并通过共享
control 拥有对应的 full submission plan/temporary result 或 standalone dirty staging。
Full HP work 会保留稳定且不可伪造的 lease，拥有 runner，并且只通过匹配的
`(RunId, RunLocalTaskId)` 路由 task failure。包括 dirty 与 preflight work 在内的内建 HP/RT
CPU ready work，现在都会作为具有 heap-owned callback context 的 move-only submission 跨越
注入的 multi-Run `ExecutionService` 边界。Service 会在发布前为每个 Run 获得一个 checked
full-vector reservation。Initial 与 dependent ready work 必须持有 bounded-store grant，worker
再将其交换为 execution grant。Product compute 使用完整的 request-owned Graph/proxy snapshot，
并且只在 Issue #72 exact-revision predicate 成功后发布。显式 QoS class、deadline 与 weight 会进入
当前内建 policy route；intent 与 quality 不会推断它们。Issue #73 会在有界 planning、queue、
callback、dependency、phase 与 commit 边界观察每个 immutable deadline 与私有 request source。
Issue #74 增加每个 request 的不可变 supersession key/generation、request-wide realtime
cancellation 与 aggregate settlement、每个精确 key 的一个 pending mailbox 与 persistent ticket，
以及 current-generation commit validation。Issue #75 增加 Host 编写的 policy frontier、
generation-scoped policy binding 与 fault state、reserved-start admission 和私有 execution route。
Issue #76 增加当前 lifecycle registry wiring、Graph close、process shutdown 与 telemetry。
Public cancellation control 仍属于后续切片。

本节其余内容说明已实现 ownership contract 及其剩余目标扩展。

`ComputeRun` 是计算身份和生命周期单元，与 `GraphRuntime`、policy selection snapshot 和
`ComputeIntent` 不同。

非 realtime HP request 拥有一个 Run。协调独立规划的 HP 与 RT sibling 的 request 拥有一个
request/run-group identity，并为每个 domain 拥有一个 child Run。Group identity 协调 caller-visible
completion，但不会创建跨 domain task dependency。

Request-owned `RunGroup` 只有在两个 child 都成功时才成功，并返回 RT child output。其 control
block 拥有 child observation lease、sibling gate、aggregate arbiter 与 caller promise，不拥有任何
child plan、dispatcher、staged output 或 reservation。其确定性聚合顺序为 failure、cancellation、
success；resource exhaustion 优先于其他 failure，同一 failure class 中 RT 优先于 HP，
group-origin cancellation 优先于 child-only reason；monotonic group arbiter 最先接受的
group-origin reason 保持稳定，之后再按 RT/HP child 打破平局。Group/lifecycle cancellation 会
到达两个 child。RT 在提交前 failure 或 cancellation
会永久拒绝 HP commit，并请求取消 HP；HP failure 或 cancellation 不会回滚已发布的 RT proxy，
也不会请求取消 RT，但会阻止 group success。Caller completion 会等待两个 child Run 都达到
terminal、quiescent、finalized，两个 admission attempt 都完成处理，graph/resource 完成 exact
release，并且每个已安装的 registry entry 都注销。Ready caller future 只包含复制后的 aggregate
value，不持有 child `RunLease`。

一个 Run 拥有或捕获：

- 一个不透明、不复用的 `RunId`，以及可选的 request/parent/run-group identity；
- immutable graph identity、`GraphRevision`、target 与 request input snapshot；
- single-domain `ComputeIntent`、quality、QoS、monotonic deadline、weight 和 maximum
  parallelism；
- supersession key 与 generation；
- monotonic cancellation state 和唯一 terminal outcome；
- request plan、dispatcher dependency state、staged output 和 exception state 的稳定存储，
  并由 Run lease 让这些存储保持存活；
- resource reservation 和 commit policy。

Task 使用 `(RunId, RunLocalTaskId)` 寻址。Local task id 在所属 Run 之外没有意义。

目标 phase progression 为：

```text
Created -> Admitted -> Queued -> Running -> CommitPending -> Terminal
```

安全路径可以跳过非 terminal phase，但绝不倒退。只发布一个 `Succeeded`、`Failed` 或
`Cancelled` outcome。Completion 本身不等于成功：dependency aggregation 与串行化 graph-state
commit predicate 都必须成功。Cancellation、Run 内部 failure 与 Graph/RT result-commit
contender 共享一个 Run terminal arbiter。当不可抢占 work 仍需排空时，terminal publication
可以先于物理 quiescence。

`ComputeRun` 为 request-local state 提供稳定生命周期，但不拥有 dependency transition 的语义；
dependency counter、ready detection 和 dependent release 仍由 `ComputeTaskDispatcher` 负责。

`ComputeIntent` 描述 HP/RT 业务语义；QoS 和 deadline 描述资源策略；
`ComputeCommitPolicy` 决定完成结果能否可见。三者不能互相推导。

### Run lease、ready task 与 completion

每个已接受的 ready-store entry、执行中的 callback、completion record、dispatcher continuation
与 commit continuation 都拥有或转移一个不可伪造的 `RunLease`。该 lease 让 plan、dispatcher、
temporary/staged output、exception state 与 completion endpoint 保持存活，但不会转移 Graph 或
resource authority。

只有 dispatcher-ready `ReadyTaskSubmission` value 能进入执行域。它们携带 immutable metadata、
`(RunId, RunLocalTaskId)`、稳定 executable state、resource requirement 与一个 Run lease；绝不会
携带 `GraphModel`、plan/task graph、dirty state、dependency map、cache authority 或 visible commit
authority。

Completion 通过 lease 返回匹配的 Run dispatcher。新就绪的 dependent 会重新进入进程 admission、
有界 ready store 与 global policy。不同 Run 可以复用 local task id，而不会发生 completion、
dependency 或 exception 串扰。

Run destruction 不抛异常，且只会在发布一个 terminal outcome、达到 quiescence、释放全部 lease，
以及精确释放全部 reservation/grant 后发生。Caller observer 被丢弃不会隐式取消已经 admission 的 work。

### 截至 Issue #76 的当前 `GraphRuntime`

截至 Issue #76，当前 `GraphRuntime` 拥有 `GraphModel`、graph-scoped runtime state、彼此分离的
graph-state 与 compute-request lane、monotonic `GraphRevision`、revision capture、串行化 commit
validation/publication、graph event、稳定 graph-instance identity、platform/session metadata，
以及一个绑定该精确 Graph identity 的 `GraphLifetimeAnchor`。该 anchor 提供 lifecycle admission
使用的 close coordinator 与 lease root。

它不拥有 Run、admitted-Run index、CPU/device/I/O/plugin worker、process ready store、
process admission、`ResourceLedger`、`PolicyRegistry`、policy binding 或物理 execution route。
Process-owned `ExecutionService` 拥有私有 `RunLifecycleRegistry`；该 registry 拥有
admitted-Run index 与 admission/Graph-close/process-shutdown lifetime fence。
`GraphRuntime` 只存储复制的 HP 与 RT route id 及其 nonzero generation。Run 可以持有
registry-validated Graph lifetime lease，但不会反转任一所有权。

graph-state lane 只在捕获 immutable revision 和执行经过验证的 visible commit 时持有，不覆盖
长时间 planning/execution。私有 compute-request lane 当前会串行化同一 Graph 的 request，但不
拥有 executor 或 policy lifetime。Issue #76 registry/lifetime fence 已是当前行为；这里未来工作
仅限 public cancellation/control surface 与另行批准的后续 execution-domain capability，而不是
Graph anchor 或 lifecycle registry。

## 进程执行域

`ExecutionService` 是深模块：调用方提交 ready work 并接收 completion；admission、queueing、
policy validation、reservation、executor 和 completion routing 保持在内部。

产品 composition root 根据进程配置构造一个显式 service，并将其注入。它不是 static singleton。
Root 会先于参与其中的 Kernel/Host 构造该 service，并保留它，直到这些 owner 停止 Run admission
并排空自身 Run。Graph close 不会停止该 service；只有 process execution-domain shutdown 才会停止。

当前截至 Issue #75 的基线已实现共享 CPU/resource、policy 与 staged-commit 边界：
`EmbeddedHostState` 会在 Kernel 前以显式 limit 创建一个固定 pool 的 CPU service，Kernel 再把
它注入 request-local `ComputeService`。
内建 CPU 的 full HP、full RT、standalone dirty HP/RT 与 preflight dispatch 都会转移不可变、
由 lease 支撑且具有 owned callback context 的 `ReadyTaskSubmission`。Service 会并发执行多个
Run，同时隔离每个 Run 的 completion、first failure、trace routing 与 Host context。封闭的私有
route 提供 serial-debug、shared-CPU 与 GPU-pipeline execution，但不暴露其 worker、queue 或
completion adapter。Service 独占一个 Host 权威 ledger，在发布前原子 admission 每个完整 Run vector，要求 initial
与 dependent ready work 以 child grant 进入同一个有界 store，并恰好一次释放每个
reservation/grant。私有 interactive 与 throughput 策略会应用显式 QoS、work/byte 计费、
Graph/Run 公平性、deadline 偏好、aging、interactive headroom 与有界 throughput 进展。精确
Graph identity/revision validation 与 staged product publication 已是当前行为。私有 cooperative
Run cancellation 会关闭匹配 ready admission、清除匹配 queued entry、拒绝 dependent re-entry、
等待 in-flight callback，并与 commit 仲裁。每个 Graph 的 supersession 现在会为每个精确 key 合并
一个 pending owner，并要求 commit 匹配 current checked generation。Host 编写的 policy frontier、
纯 C plugin selection、generation-local sticky fallback、allocation-free start commit 与
ready-to-execution grant exchange 也已成为当前行为。Issue #76 已让 lifecycle registry、Graph
lifetime lease、单调 Graph close、显式 process shutdown 与 source-private telemetry 成为当前
行为；public cancellation control 仍属未来工作。

Service 拥有物理 CPU worker 与后续 resource executor、有界 ready storage、Run/resource
admission、policy-result validation、execution exception fence 与 completion routing。它不拥有
planning、dependency semantic、Graph/document persistence、cache authority、dirty propagation、
visible commit 或 Graph state。

其私有 `RunLifecycleRegistry` 拥有唯一 process admission fence、service accepting/stopping
state、graph-indexed open/closing row、pending admission candidate、graph-indexed admitted
`RunLease` entry 与 process-wide Run enumeration。它不由 Graph 拥有、不属于单个 Host adapter，
也不是 static。Admission 首先在该 fence 下记录 pending candidate 并获取 graph-lifetime lease，
随后捕获 immutable revision、执行 planning 并获取完整 resource reservation。第二次 fenced
recheck 会原子地在两个 index 中安装 Run，这就是成功 admission 的线性化点。

Graph close 与 process shutdown 会通过同一个 fence 改变 lifecycle state。
Registration-before-close 会进入 index 并被排空；close-before-registration 会拒绝 candidate，并
等待 exact rollback。Registry entry 只持有 `RunLease` 与 identity metadata，绝不拥有 plan、
dispatcher、terminal arbiter、staged output、Graph state 或 resource token。只有 terminal
publication、物理 quiescence、commit/discard finalization 与 exact graph/resource release 都完成
后，entry 才会注销。

Visible commit 会先进入 graph-state lane，再取得 lifecycle fence，以完成最终
open-row/registered-Run validation 与 publication。Close 会标记 closing、释放 fence，之后才等待
lane，因此 commit-first publication 可以完成，close-first validation 会拒绝 commit，并且不会形成
registry/lane lock cycle。

`ExecutionService` 独占一个由 composition-root limit 初始化、Host/device 权威的
`ResourceLedger`。只有
受信任 Host code 能铸造其 move-only、不可伪造的 reservation 与 grant。Policy 或 plugin 可以请求
或建议 resource，但不能构造、复制、扩大或直接释放 token。

当前 ledger 会验证 CPU slot、ready-store entry/byte、retained/in-flight Host memory 与 Host
scratch 的事务性 vector。它还为每个已配置非 CPU `DeviceId` 拥有隔离且 immutable 的
memory/scratch limit。Native allocation plan 会原子提交两个 dimension，在 `allocatedSize`
校准后归还未使用 byte，并把 actual ownership 拆分给 persistent native Value owner 与
asynchronous completion scratch。Device queue depth/in-flight command limit、compute-I/O
operation/byte，以及 plugin-process/invocation/IPC 仍是未来维度，当前不会用虚假的零值 authority
表示。当前 success、failure、rejection、rollback、replacement、worker-exception、stale
completion、eviction、cancellation 与 close/shutdown path 都会恰好一次释放每份 authority。
Capacity exhaustion 与 checked overflow 会在无 partial reservation、overcommit、跨 device
借用或 silent clamping 的情况下失败。

每个 policy binding 都是比较 seam，而不是物理 executor 或 resource authority。当前 Interactive
与 Throughput binding 会排列 Host 编写的 immutable candidate descriptor；由 service 拥有的 store
保留每个物理 entry 与 Graph/Run fairness row。Policy 不拥有 worker、ready store、Run、Graph
state、budget、reservation、grant/token、native device handle、executor、completion route 或
lifecycle authority。

Issue #71 使用两个真实内建 policy 与一条共享路径证明该 seam：

- dispatch cost 为 `work_units + ceil(complete_ready_grant_bytes / 4096)`；
- Graph service 在每个已选 class 各自独立的 accumulator 中使用原始 cost，Run service 在每个
  Run 的不可变 class 中使用 `ceil(cost / weight)`；
- interactive 排序偏好存在且更早的单调时钟 deadline，throughput 排序采用加权且确定的规则；
- ready entry 在八次成功 dispatch 后 aging；
- throughput 持续 ready 时，至多三次连续 interactive dispatch 后必须保证 throughput 进展；
- 配置的 interactive headroom 只限制 active Throughput root reservation；Interactive Run 不会扣减
  该 class quota，而 ledger 保留最终物理权威；Throughput charge 会跟随精确 root lifetime，直到
  deferred child release 完成；
- initial 与 dependent work 使用同一 policy route，Run row 会跨越临时为空的阶段继续存在。

Latest-generation preference 与 exact-key coalescing 自 #74 起已是当前行为。Revision-safe commit
自 #72 起已是当前行为，cooperative cancellation 自 #73 起已是当前行为；纯 C policy ABI v1、
generation-scoped binding、Host 编写的 frontier、经过验证的 fallback、reserved start 与私有
execution route 自 #75 起已是当前行为。
Larger quantum 与 device-utilization awareness 仍是后续 profile/device 目标；issue #71 不声称已实现它们。

拥有 worker 的 scheduler ABI、SDK target、`IScheduler` hierarchy 与 per-Graph 物理 owner 已经被
完全移除。纯 C policy ABI 是破坏性替代；没有留下 compatibility adapter 或 forwarding layer。

旧的 worker-only budget 已被完全移除，没有包装、重命名或别名。Execution worker-count
resolution 现在只属于 composition-root configuration；Run 的全部 CPU admission authority 都来自
唯一 service-owned ledger。

### Revision、cancellation 与可见 commit

Issue #72 使最小 revision 子集成为当前行为。Run 在 planning 前捕获一个强类型 Graph instance
identity 与 immutable `GraphRevision`。Product work 使用 request-owned Graph/proxy snapshot。其
串行化 predicate 要求 `CommitPending`、预期 domain/label、精确 staged owner、staged
identity/revision 与 descriptor 相等、live identity/revision 相等，以及有效 staged domain output。
成功 publication 会保留 revision，并先于 Run success；validation 失败会丢弃 staged output，且
不能修改 visible Graph/proxy state 或写入 deferred cache artifact。

Issue #73 让 cancellation 成为该当前 predicate 的组成部分。一个私有 request source 与 immutable
monotonic deadline 会通过与 Run 内部 failure 和 Graph/RT result-commit contender 相同的 Run
arbiter 竞争。内建 ready entry 按精确 Run identity 清除，dependent re-entry 被拒绝，queued
plan/callback completion unit 恰好一次 retire，已经进入的 non-preemptible work 则会排空，
但不能允许 staged publication。被接受的 commit contender、精确 predicate、符合条件的
persistence、visible swap 与 terminal resolution 会共享同一个串行 graph-state work item。

Issue #74 已扩展该 predicate，要求 supersession generation 仍为 current。Supersession 会选择一个
更新 generation，
并请求取消此前匹配的 Run，但不会复用其 identity 或修改其 plan。不可抢占 work 与外部 side effect
可以完成，但 stale、cancelled、failed 或 overdue output 不能 commit。

Issue #76 还增加当前 `Open` registry Graph row、已注册 Run 与有效 Graph lifetime lease 检查。

任何未来 compatible-revision 优化都要求另一个显式决策；不能从 topology 相等推断 compatibility。

配对 RT/HP work 使用单调的 `Pending` / `RtCommitted` / `Denied` sibling gate。Issue #72 当前
只在有效 RT proxy publication 之后开放该 gate，随后应用独立的 HP revision predicate；之后出现
stale HP result 不会回滚 RT。Issue #73 规定 RT cancellation 在 `Pending` 状态胜出时拒绝 gate 并
请求 HP cancellation；`RtCommitted` 之后的 HP cancellation 不能回滚 RT。Graph-close 与
process-shutdown denial reason 现在会 fan-out 到两个 child Run。

### Graph close 与 process shutdown 作用域

Graph close 会在 lifecycle fence 下把对应 row 标为 closing、拒绝 new/pending admission、等待
此前 candidate 注册或回滚，并枚举完整 graph Run index。它会拒绝 visible commit，取消或排空
这些 Run，并保留它们的 finalization path。只有 terminal publication、物理 quiescence、
commit/discard finalization、exact graph/resource release 与 admitted-Run unregistration 都完成
后，它才移除空 row，在 graph-state finalization 仍可用时停止/排空 compute-request lane，再
停止/排空 graph-state lane 并销毁 Graph state。不相关 Graph 的 Run 与 shared service 会继续运行；
marker 完成后任一 lane 都不会 reopen。

Process execution-domain shutdown 会在同一个 fence 下把 service 标为 stopping，并把全部 graph
row 标为 closing，处理 pending candidate，再枚举完整 process Run index。只为选择 cancel 或 drain
的已 admission Run 保留有界 ready submission、execution、completion routing 与 graph-state
finalization。每个 Run settle、恰好一次释放 graph/resource lease 并注销后，shutdown 才停止其余
work admission、join 全部物理 executor、retire policy binding、在 15 个 lifecycle/resource
counter 全部为零时发布 `ServiceStopped`，最后销毁 service。Worker/operation exception 会被 fence，
并通过匹配 Run lease 路由；late completion 只执行 cleanup。

### 交付依赖契约

| Issue | 必需结果 | 依赖 |
| --- | --- | --- |
| [#66](https://github.com/kevin-zf1123/photospider/issues/66) | 当前 HP `ComputeRun` descriptor、state、storage 与唯一 terminal outcome | #63、#65 |
| [#67](https://github.com/kevin-zf1123/photospider/issues/67) | 当前稳定 Run lease 与 `(RunId, RunLocalTaskId)` full-HP completion isolation | #66 |
| [#68](https://github.com/kevin-zf1123/photospider/issues/68) | Injected CPU-only service 基础、一个 Run、ready-only input | #67 |
| [#69](https://github.com/kevin-zf1123/photospider/issues/69) | Shared multi-Graph/HP/RT CPU domain，且无 per-Graph CPU worker | #68 |
| [#70](https://github.com/kevin-zf1123/photospider/issues/70) | 当前 production admission、有界 ready store 与 ledger | #69 |
| [#71](https://github.com/kevin-zf1123/photospider/issues/71) | 当前 interactive 与 throughput 内建 policy | #70 |
| [#72](https://github.com/kevin-zf1123/photospider/issues/72) | 当前 revision capture 与 staged commit predicate | #67 |
| [#73](https://github.com/kevin-zf1123/photospider/issues/73) | 当前 queued/running/commit cancellation | #70、#72 |
| [#74](https://github.com/kevin-zf1123/photospider/issues/74) | 当前 latest-wins supersession 与 realtime `RunGroup` | #71、#73 |
| [#75](https://github.com/kevin-zf1123/photospider/issues/75) | 当前纯 C policy generation、Host frontier/fallback、reserved start 与私有 execution route | #71 |
| [#76](https://github.com/kevin-zf1123/photospider/issues/76) | Graph close、process shutdown、telemetry 与最终不变量 | #69、#73、#74、#75 |

该图无环。#72 已获准在 #67 后与 #68–#71 并行推进；#75 已在 #71 之后与 #73–#74 路线一同
交付。该表固定所有权依赖，不固定实现算法。

## 依赖中立内核

[ADR 0002](../../adr/zh/0002-external-libraries-are-kernel-adapters.zh.md)
约束本目标。维护中的当前基线由[内核术语](../../kernel-architecture/zh/Terminology.zh.md)、
[内核数据模型](../../kernel-architecture/zh/Data-Model.zh.md)、
[脏区传播与工作选择](../../kernel-architecture/zh/Dirty-Region-Propagation.zh.md)和
[图生命周期与变更语义](../../kernel-architecture/zh/Graph-Lifecycle.zh.md)记录。迁移期间，这些
当前状态文档仍是权威来源。

内核只拥有表达和执行自身语义所需的小型原语：

- checked rectangle、extent、clip、union/intersection、scale、halo、grid、tile alignment 和
  transform bound；
- stride-aware buffer view、copy、fill、crop-to-view、pad、最小 conversion 和 validation；
- format-neutral parameter value 和 typed graph definition；
- 注入式 graph document reader/writer、image/artifact codec 与 cache metadata codec。

OpenCV 继续作为可选 operation provider、image codec 和公共 image adapter。它不得定义 Graph、
ROI、dirty propagation、planning、cache 或 runtime interface。当前仓库自有 CPU provider 已遵循
[ADR 0004](../../adr/zh/0004-opencv-cpu-operations-are-reentrant-provider-work.zh.md) 的 provider
并发方向：使用可重入 `cv::Mat` callback，在发布前把 OpenCV 内部 CPU threading 固定为一，把
外层并行交给 Host 已准入的 execution start，并让真实共享 backend 同步保持 provider-local。仓库自有
operation algorithm、对应 OpenCV 初始化与异常翻译现已位于可独立开关的 provider module 中；
provider-disabled profile 会证明 stdlib-only v2 provider 能提供并执行缺失 operation。Issue #63
让 image processing、codec、public adapter、provider/plugin 默认值与 embedded product 都由
capability 选择。Dependency-disabled profile 不发现 OpenCV，并使用标准库或显式 unavailable
adapter 构建真实 kernel aggregate 与 Host product。

YAML 继续作为受支持的 document adapter；`YAML::Node` 不再作为 runtime parameter、output、
cache metadata 或 graph-state value model。Graph load/save 是具有显式 transaction 与 error
contract 的注入行为；
[ADR 0005](../../adr/zh/0005-graph-document-ingestion-is-a-classified-transaction.zh.md) 固定了
load 边界必须保留的分类摄取事务。

Issue #62 让 runtime/cache value 纵向切片成为当前行为：共享 YAML conversion 归 adapter
所有，cache metadata 经过注入的格式中立 codec，inspection 使用中立的递归 formatter。Issue #63
完成 dependency-disabled product/static/install consumer 纵向切片。其 clean smoke build 会禁用
两个 capability discovery，验证真实 `photospider_kernel` 与 `photospider` target，确认安装不泄漏
依赖，并运行外部 Host consumer。

## 通用数据与 Region

当前 baseline：`ImageBuffer`、`DataType`、`Device`、`PixelRect`、`ParameterMap`、
operation ABI v2 以及既有 cache/execution ownership 仍是已经实现的 compatibility contract。
V-2 实现了有界、dependency-neutral 的 CPU DenseTensor `Value`/`ImageView` 子集与一条内建
operation。V-3 现已新增 checked BufferHandle ownership、由 lease 控制的 construction、
process-local allocation/revision identity、受界限约束的 signed layout，以及 CPU image Value
在正式 HP cache 中的 identity authority。V-4 现已新增 public bounded Region contract、
logical dirty/cache validity，以及由精确 core dense path 执行的 ImageRect/TensorSlice。V-5 会
路由 CPU implementation metadata 与 checked resource demand。V-6 现已新增
dependency-neutral ReadyFence/Value readiness contract，以及一条由确定性 fake device executor
证明的显式 source-private CPU Value-copy task。V-7 现已在进程 execution domain 中新增固定的
source-private `DeviceExecutorRegistry`，并让仓库 Metal Perlin operation 经过其自有
device/queue、invocation-scoped allocator 与持久 pipeline cache。V-8 现已新增经过检查的
CPU/Metal binding fact、纯显式 access planning、保留 revision 的双向 transfer、进程级
residency、精确 stale-completion arbitration、pending-Value continuation 与 asynchronous
Perlin readback。V-9 现已新增仅面向 fixed registry 中可执行设备的隔离 memory/scratch
account，在 allocation 前准入 native plan，与 allocator 上报的 actual byte 对账，并把精确
lease 绑定到 persistent Value 与 asynchronous completion。V-10 批准 typed compute-I/O
completion 并让 persistence authority 保持分离；V-11 通过 `ComputeIoExecutor` 运行首条有界
cache/codec mechanism。V-12 现在会验证已安装通用模型中的 1/3/4/8/16 通道 FP32/FP64
image、rank-one 至 rank-five FP32/FP64 latent Value、padded 与 signed/zero stride、精确
Region merge、显式 CPU/external-device transfer 和有界 compute-I/O retention。V-13 现在会
安装一条 packed FP4 E2M1、block-scale quantized DenseTensor 垂直路径，包含 version-1 Blocked
addressing、checked packed access、block-aligned TensorSlice copy、保留表示的 transfer、精确
memory-cache retention 与 fail-closed image disk persistence。V-14 现在会安装一条
dependency-neutral provider-defined Value 垂直路径，包含保留 byte 的 Schema/Facet/Layout
envelope、checked multi-buffer binding、一个注入式 typed registry、纯 C definition-suite ABI v3、
纯 property/DataSpec/Region evaluation、canonical descriptor/content/layout digest、
artifact-envelope round-trip，以及 generation-safe replacement/unload。精确行为记录在
[内核数据模型](../../kernel-architecture/zh/Data-Model.zh.md)、
[ImageBuffer 内存契约](../../kernel-architecture/zh/ImageBuffer-Memory-Contract.zh.md)、
[插件 ABI](../../kernel-architecture/zh/Plugin-ABI.zh.md)与
[内核缓存模型](../../kernel-architecture/zh/Cache-Model.zh.md)；execution ownership 记录在
[策略与执行架构](../../kernel-architecture/zh/Policy-and-Execution-Architecture.zh.md)与
[计算边界](../../kernel-architecture/zh/Compute-Boundaries.zh.md)。下述完整模型是已接受目标；
只有这里明确指出的 V-2 至 V-14 子集是当前 runtime 事实。

[ADR 0008](../../adr/zh/0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.zh.md)
是完整目标契约的权威来源。其核心分离关系是：

```text
Value
├── DataDescriptor
│   ├── exactly one versioned RepresentationSchema
│   └── zero or more orthogonal versioned Facets
└── one or more authoritative StorageBindings
    ├── StorageLayout
    ├── BufferHandle[]
    ├── ReadyFence
    └── AccessProvider lease
```

`DataDescriptor` 是逻辑对象。Allocation、stride、packing、device、byte range、mapping 与
readiness 是物理 binding fact。`Value` 在唯一的 exclusive `ValueBuilder::seal` 后保持逻辑和
结构不可变；checked view 会保留完整 Value。Seal 会撤销每个普通 builder/caller
`WriteLease` 和每条 consumer 写路径。Pending producer 只能保留 seal 时原子转交的唯一私有写
capability，并且只能用于其预先验证的 binding envelope。稳定 core 通过永久
Schema/Facet/Layout identity、canonical versioned payload、纯 nonblocking query、显式
operation 与带 lease 的不可变进程级 provider generation 实现扩展。

首个 representation 是同构 rank-N DenseTensor。普通图像是
`DenseTensor + ImageFacet`；channel、color、alpha 与 time 含义都是显式的，绝不从名称推断。
每个 site 的 variable sample 使用 `VariableSampleField`；OpenEXR Deep 逻辑 value 是
`VariableSampleField + ImageFacet + DeepSampleFacet`。StructuredValue v1 是自包含的，
不含 runtime child Value。

已实现的 V-2 至 V-14 子集刻意保持更窄的范围：

- `DenseTensorDescriptor` 包含 positive concrete shape、彼此独立的 unsigned/signed integer
  或 floating element semantics、8/16/32/64-bit native scalar storage 或显式 four-bit FP4
  E2M1 encoding，以及可选 V-13 block-scale quantization；后者包含 rank-matched positive block
  shape，以及每个完整 row-major logical block 对应的一项 finite positive scale；
- `ImageFacet` 显式映射彼此不同的 x/y axis 与可选 channel axis；
- public `BufferHandle` 是同一个 opaque process-local `AllocationIdentity` 上受检、非空的
  range；subrange 会保留 allocation lifetime。CPU allocation 可以签发 host read lease，
  source-private native binding 则保留 external owner，并且只暴露经过检查的 binding fact；
  两条路径都不暴露 public raw pointer 或 native pointer；
- move-only `ValueBuilder` 控制唯一 move-only `WriteLease`，要求 byte offset 为零的 positive
  exact-envelope Strided producer，或 version-1、nibble-aligned、exact-envelope、non-overlapping
  Blocked producer，在 lease 存活时拒绝 seal，并发布全新 process-local `ValueRevisionId`；
- final、copyable `Value` 共享 immutable descriptor/layout/handle state；DenseTensor Value
  通过 sealed handle 构造，并且只保留一个带 tag 的 Strided 或 Blocked layout；Strided alias 可以使用受界限
  约束的 byte offset 与正、零或负 signed stride，V-13 Blocked alias 则使用 checked bit offset
  与 block bit stride；
- retaining checked `DenseTensorView`/`ImageView` 持有 `ReadLease` 并暴露只读 whole-byte
  address；`PackedDenseTensorView` 则暴露 checked FP4 code 与 scale-dequantized value，不伪造
  element byte pointer；
- installed `ReadyFence` 是 Pending、Ready、Failed 或 ProducerCancelled 的 copyable
  nonblocking observer；其 move-only completer 只发布一个 terminal state，丢弃 unresolved
  completer 会发布 cancellation；wait 通过共享的 non-inline executor 入队，该 executor 在
  pending、queued 及 callback 完成前保持存活，并在 callback 进入时释放 queued
  self-retention；
- 同步 Value 初始即为 Ready；source-private CPU 与 native pending producer 保留唯一 mutable
  completion capability，并在每次 terminal state 前撤销它；pending/failed/cancelled Value
  保留 immutable metadata，但拒绝 BufferHandle 与 checked-view payload access；
- 经过检查的 `DeviceId`、`MemoryDomain`、`StorageBinding`、producer identity 与纯
  `AccessPlan`，会显式表示 direct、map、import、transfer 或 unsupported access。
  `ValueTransferTask` 在全新的 destination binding 上保留逻辑 revision，并且只在 source ready
  后以显式 queued work 执行 CPU copy、CPU-to-Metal upload 或 Metal-to-CPU readback；
- 唯一的进程级 `ResidencyManager` 按精确 revision/binding 索引合格 replica。它会原子校验完整
  Graph/Run/generation/task/producer/binding completion identity、发布 readiness 并插入
  residency，因此晚到、重复或 identity 不匹配的 completion 不能释放 dependency work，也不能
  重新获得 stale commit right。Kernel 会在 coordinator submission 前预跟踪每条 lineage，
  而且只有 accepted current publication 才会在 currentness 可观察前推进该行，从而阻止之后
  才启动的较旧 Run 让 freshness 倒退；
- source-private `DeviceExecutorRegistry` composition 会在 `ExecutionService` 下拥有固定的非 CPU
  executor；在仓库 plugin 已启用的 Apple profile 中，Metal executor 拥有一个可复用 native
  device/queue 与经过校验的 pipeline cache，通过 invocation allocator 把 callback-scoped
  texture/buffer 保留到 callback 返回，并在 reserved start 后进入一条选中的 Perlin operation，
  且不会通过 Graph、policy、metadata 或 public Host state 暴露 native handle。Perlin 会发布
  pending native Value，并编码 asynchronous texture-to-shared-buffer readback，不等待 command
  buffer，也不调用同步 `getBytes`；
- service composition 会校验全部逐 `DeviceId` 候选 limit，然后只为 frozen registry 中匹配的
  executor 创建 device memory/scratch account。空 registry 或 non-Apple 默认 registry 不暴露
  Metal account，而缺少候选 budget 的 registered executor 仍无法准入 native allocation；
- `image_process:invert_dense` 把精确 descriptor-only inference 与 stride-aware
  unsigned-8 execution 分开，已有 sealed input Value 时直接复用，并发布精确 sealed result
  revision 与独立 ImageBuffer compatibility snapshot；
- 私有正式 HP CPU image cache entry 把有效 sealed `NodeOutput::image_value` 作为
  allocation/revision authority。普通 copy 保留 identity；dirty mutation、replacement 与
  disk decode 创建新 identity；disk save 读取 Value byte；runtime token 永不成为持久
  cache/task key。V-13 正式 memory-cache copy 还会保留 packed Value 与精确 TensorSlice
  validity，而 image-only disk cache 会在 executor admission、filesystem mutation 或 codec
  call 前拒绝 packed、quantized 或 latent 正式 Value；
- installed `RegionSet` 支持规范 Empty/Whole、由 ImageRect 或 rank-general TensorSlice atom
  组成的一个有界 nonempty conjunction、checked normalization/clipping/algebra/containment、
  显式 budget，以及 typed Exact/ConservativeSuperset/Unknown/Unsupported/TooComplex outcome；
- dirty source、per-node、edge、monolithic 与 HP validity record 保留规范化 Region；当前 image
  tile、ImageBuffer helper、Host/IPC v2 inspection 与 operation ABI v2 使用 checked derived
  PixelRect；
- 当前选中的精确 core `invert_dense` callback 通过 checked stride 执行 ImageRect 或
  TensorSlice；TensorSlice 是 HP-only monolithic work，same-key plugin replacement 无法继承该
  source-private contract。

V-14 新增第二种显式 `ProviderDefined` representation。它的 `DataDescriptorEnvelope` 拥有一个
Schema 和一组受界限约束且有序的 Facet；它的 `ProviderDefinedLayout` 拥有一个 Layout definition
以及经过检查的 buffer-role envelope；它的 Value 保留多个已 seal、host-readable 的
`BufferHandle` 以及一个不可变 provider generation。DenseTensor-only accessor 与当前 transfer
path 会拒绝这种表示。带 index 的 `ProviderReadLease` 会同时保留选中的 buffer 与 interpretation
generation。

一个注入式 `DataDefinitionRegistry` 在同一个 publication lock 下拥有单一 generation source、
provider table，以及严格 typed 的 Schema/Facet/Layout map。它会暂存并校验完整 candidate bundle，
原子发布新 generation 或 replacement generation，拒绝跨 provider typed-key conflict 且不产生
局部可见性，并且不在持锁时调用 provider callback。Unload 会拒绝新 lookup；旧 Value、read、
callback 与 opaque owner 则保留正在退役的 generation，直到最终 provider destroy 与 module
release。

V-12 增加的是验证，而不是新的 representation 或 provider ABI。它的 dependency-neutral
矩阵会证明：1/3/4/8/16 通道 active logical FP32/FP64 image element 穿过 padded Value 与 CPU
ImageBuffer bridge；rank-one 至 rank-five FP32/FP64 latent Value 穿过完整 rank TensorSlice；
ImageRect/TensorSlice merge 保留选中/未选中的 element；显式 CPU 与注入式 external-device
transfer 保留完整正向 producer envelope；binding、allocation、revision 与 Pending-to-Ready
事实保持精确；negative/zero-stride 不可变 view 可读，并且 transfer 会显式拒绝它们。独立
direct-offset byte oracle 会证明 rank-one 的唯一 stride
大于 element，其 required storage span 真实带有 padding、active byte 保持精确且 padding
sentinel 未被改写。CPU-copy 与 external preparation 会复用同一个 core 正向、零 offset、精确
envelope、non-overlap 权威；external rejection 发生在保留 destination owner、生成 identity、
创建 fence、调用 provider 与发布 Pending destination 之前，同时不会收紧通用 signed immutable
publisher。已准入 compute-I/O task 会在有界 budget 下保留并观察相同的 Value metadata 与字节，
但不会创建 artifact 或 persistence identity。

V-13 新增一条可执行 packed 垂直路径。FP4 E2M1 encoding、floating-point semantics 与
block-scale quantization 保持为彼此独立的事实。Version-1 `BlockedLayout` 记录 matching block
shape、nibble-aligned block bit stride、absolute bit offset 与显式 nibble order。Checked
publication 会证明完整 block、精确 byte bounds 与互不重叠的 block span。Packed TensorSlice
copy 只接受 full-rank、nonempty、block-aligned interval，会投影 row-major scale、直接复制 code，
并发布 fresh canonical blocked CPU Value。CPU 与注入式 external-device transfer 会在不同
binding 中保留 descriptor、quantization、layout、byte envelope（包括 unused nibble bit）、逻辑
revision 与 Pending-to-Ready fact。正式 memory cache 会保留精确 Value/Region fact；image disk
persistence 则 fail-closed，不会伪造 widened image byte 或通用 artifact format。

V-14 实现一个受界限约束的具体 `DataSpec`、typed pure property/Region outcome、大小精确的
C11/C++17 v3 definition-suite ABI，以及带 tag 的 SHA-256 Descriptor/Content/StorageLayout digest。
纯 callback 不接收 payload；validation 与 canonical-content traversal 只接收保留且经过检查的
buffer view。Versioned artifact-envelope encoding 可以在没有 provider 时保留未知
Schema/Facet/Layout byte 与 digest metadata。它不是 graph document、filesystem codec、cache
manifest/chunk store 或 durable output authority。

V-14 仍不含 public device registry、device queue/in-flight dimension、更多 packed encoding
或 quantization formula、未对齐 requantizing slice、access/conversion/inference/execution provider
suite、通用 graph/cache Value persistence、manifest/chunk、OpenEXR 或通用 named graph Value output。Native
executor、transfer submission、mutable producer、completion admission 与 residency owner
仍是 source-private。ImageBuffer 仍是 operation ABI v2、tiled write、codec 与 Host surface
的 compatibility representation。

`ElementSemantics`、`StorageEncoding` 与 `QuantizationSchema` 彼此独立。Describable、
executable 与 convertible 支持也彼此独立，而且 conversion 始终显式。因此 FP64、任意
channel、padded 或 signed stride、N-dimensional latent value 与 packed FP4 都可以表示，
而无需静默 float32 conversion、one-byte-per-element 假设或 channel-role 猜测。

对于当前 V-14 子集，`BufferHandle` 是已检查的不可变 byte range。Consumer read 与普通
builder write 需要 lease；已 seal Value 永不签发 `WriteLease`，consumer write 始终被拒绝。
Source-private producer 可以通过其不可复制的 capability，在预先验证的
binding/Layout/handle envelope 内完成一个 sealed pending CPU 或 native payload。该 capability 的退役
happen-before Ready、Failed 或 ProducerCancelled 发布。Pending、Failed 与
ProducerCancelled 不暴露 consumer-readable payload。CPU binding 可以提供 direct host
visibility；device-local binding 不会提供。Strided、Blocked 与 ProviderDefined Layout 都保留
有界 buffer envelope。`DeviceBackend`、`DeviceId` 与 `MemoryDomain` 彼此分离，当前 access
由 `Direct | Map | Import | Transfer | Unsupported` plan 显式表示。V-8 中只有 direct CPU
access 与显式 CPU/Metal transfer 具有 production execution；其他 plan kind 仍是 typed
outcome，而不是隐藏工作。

已实现的 `RegionSet` 是基于显式逻辑 domain key 的有界析取范式。MVP 支持 Whole、Empty、
ImageRect、TensorSlice 与一个 nonempty clause。Region algebra 返回 Exact、带标签的
ConservativeSuperset、Unknown、Unsupported 或 TooComplex，而不是静默放大。V-14 provider
子集中的 `DataSpec` 约束 Schema identity/version 与 logical-site bound，并使用 subset、
disjointness、conditional runtime guard 或
`CannotEvaluate`；它绝不授权隐式 conversion 或 device access。

Runtime revision、descriptor/content/Layout digest 与 artifact identity 是不同 identity。
Persistence 分成 graph document、canonical descriptor envelope、artifact/cache manifest 与
chunk，以及绝不持久化的 runtime binding。Provider 缺失时，未知但有效的 extension byte 会被
保留而不被解释。

Public migration 会完整收口，而不是永久保留双重边界：

```text
ImageBuffer     -> Value + ImageFacet + ImageView
PixelRect       -> RegionSet atom ImageRect
Device          -> DeviceBackend + DeviceId + MemoryDomain
OperationOutput -> named Value outputs
ParameterMap    -> configuration only
```

只有精确 record 与自有 consumer 已经存在后，operation provider 才从临时 C++ ABI v2 迁移到
单独版本化的 pure-C provider ABI v3。完成边界会删除 v2，不保留永久 wrapper、alias、
forwarding header、dual loader 或 v2-to-v3 shim。Policy ABI v1 保持独立。

### Project 4 实现依赖契约

下表冻结架构顺序，而不是 live completion status。每个链接 Issue 仍是可独立验证的实现切片，
其 Issue 与 Project field 仍是状态权威。

| 切片 | 交付边界 | 阻塞切片 |
| --- | --- | --- |
| [#78 / V-1](https://github.com/kevin-zf1123/photospider/issues/78) | 批准通用数据、内存与 Region ADR；只修改文档 | #63、#65 |
| [#79 / V-2](https://github.com/kevin-zf1123/photospider/issues/79) | 以 CPU DenseTensor 与 ImageView 跑通一条 operation | #78 |
| [#80 / V-3](https://github.com/kevin-zf1123/photospider/issues/80) | 贯通 BufferHandle ownership、allocation identity 与 cache | #79 |
| [#81 / V-4](https://github.com/kevin-zf1123/photospider/issues/81) | 让 ImageRect 与 TensorSlice 经过统一 Region | #79、#72 |
| [#82 / V-5](https://github.com/kevin-zf1123/photospider/issues/82) | 用 operation metadata 驱动 CPU implementation 与 resource routing | #80、#70 |
| [#83 / V-6](https://github.com/kevin-zf1123/photospider/issues/83) | 以 fake device 证明 fence、asynchronous completion 与显式 transfer | #80、#81、#82、#70 |
| [#84 / V-7](https://github.com/kevin-zf1123/photospider/issues/84) | 通过 DeviceExecutorRegistry 跑通一条 Metal operation | #83 |
| [#85 / V-8](https://github.com/kevin-zf1123/photospider/issues/85) | 实现显式 CPU/GPU transfer、residency 与 stale completion | #84、#74 |
| [#86 / V-9](https://github.com/kevin-zf1123/photospider/issues/86) | 在 ResourceLedger 中核算 device memory 与 scratch | #84、#70 |
| [#87 / V-10](https://github.com/kevin-zf1123/photospider/issues/87) | 批准 typed compute-I/O completion，并分离 cache、Graph 文档、daemon 与 durable-output authority；只修改文档 | #65 |
| [#88 / V-11](https://github.com/kevin-zf1123/photospider/issues/88) | 让有界 cache/asset/codec I/O mechanism 经过 `ComputeIoExecutor`，但不迁移 commit policy | #87、#70 |
| [#89 / V-12](https://github.com/kevin-zf1123/photospider/issues/89) | 验证 multi-channel、FP64、latent 与 stride matrix | #81、#85 |
| [#90 / V-13](https://github.com/kevin-zf1123/photospider/issues/90) | 跑通一个 packed FP4/quantized DenseTensor slice | #89 |
| [#117 / V-14](https://github.com/kevin-zf1123/photospider/issues/117) | 证明 dependency-free VariableSampleField definition、multi-buffer Value、pure query/digest 及 generation replacement/unload | #90 |
| [#118 / V-15](https://github.com/kevin-zf1123/photospider/issues/118) | 增加首个可选 OpenEXR deep-scanline provider/codec，且不泄漏依赖 | #117 |

V-14 是当前单独的依赖中立合成 `VariableSampleField` 切片。“依赖中立”表示该证明既不使用
OpenEXR，也不使用其他可选 codec。它会直接验证 registration、unknown byte preservation、
multi-buffer Layout 与 binding、不具备 payload 权限的 Region/DataSpec/query、独立且精确的
canonical digest、generation replacement、lease 与 unload。它的 ABI v3 仅是 definition suite，
不会提前实现 access、conversion、inference、execution 或 codec 权限。

V-15 是单独的后续可选 OpenEXR provider/codec issue/change。首个 format 是 single-part
deep-scanline read/write；它跟随 core 与 V-14 proof，而不是替代 V-14。Deep tiled、multipart
与混合 shallow/deep part 仍是后续工作。关闭 option 时，kernel、public ABI 与
dependency-disabled product 中不得出现 OpenEXR header、link、type、symbol、package
requirement 或 transitive dependency。

## 异构 Executor

当前 V-9 Metal route 组合了 process ownership、registry dispatch、queue/allocator/cache 复用、
provider-state 移除、asynchronous pending Value、显式 CPU/Metal transfer、进程级 residency 与
精确 stale-result arbitration。它的唯一 service ledger 现在会在 native allocation 前原子准入
per-device memory/scratch plan、校准 native actual byte、把 memory 绑定到 persistent Value
ownership，并把 scratch 绑定到精确 command completion。Queue、lane 与 pipeline-cache
infrastructure 仍不属于 per-invocation 核算。

GPU executor 不是第二个普通 CPU worker pool。每个物理 device executor 拥有 native queue/stream、
allocator、in-flight limit、memory/scratch reservation、pipeline cache、transfer queue 和 completion
fence。CPU worker 不阻塞等待 GPU completion；stale device completion 会释放资源，但不能提交到
更新的 graph revision。

当前 V-11 新增唯一 source-private process `ComputeIoExecutor`，其中有一个独立 worker，并在
lazy payload construction 或副作用之前，按 task 数与 estimated retained bytes 原子准入。
已接受 work 会保留显式 transaction lifetime token，并暴露 typed completion；failure、
cancellation、late return 与 shutdown 都会恰好一次 settlement。CPU compute worker 不能同步
等待该 executor。

首条生产垂直路径通过该 executor 运行 staged HP cache-save codec/filesystem mechanism，同时
由 graph-state policy 保留 eligibility、path、错误解释与既有 publication 前 commit point。
当前不可拆分的 image-codec call 整体运行在 I/O worker 上；未来拆分后的 API 必须把独立准入的
CPU-heavy phase 送回 CPU executor。同步 cache administration/load、daemon framing、Graph
文档 persistence、`OutputStore` commit policy、用户 path、retry 与 durability claim 仍由
既有 owner 负责。

### Compute I/O 耐久性与完成目标

当前基线已有上文所述有界 executor 与 staged HP cache-save 垂直路径，但仍没有 crash-durable
output store。Deferred HP cache write 仍发生在 live Graph publication 前，并可能使 Run
失败；Graph 文档保存会直接写入 destination；daemon job state 与 acknowledgement 都是
process-local；私有 IPC `OutputStore` 使用内存 lease/TTL index，提供受保护、no-replace 的
进程级 delivery。旧 `io/save` callback 也可以在包围它的 staged Run 提交前暴露文件。

[ADR 0009](../../adr/zh/0009-compute-io-durability-and-completion-semantics.zh.md)
接受以下 typed partial order 目标：

```text
成功产值 Run：
  OperationReturned(success)
    -> producer fence 成功
    -> ValueReady
    -> 经过校验的 Graph/RT publication
    -> RunTerminal(Succeeded)

Run terminal 之前的 ComputeRun failure：
  operation/readiness/dependency failure
  或 Graph/RT validation/publication/Run-result commit failure
  类型化失败 -> RunTerminal(Failed)
  （不伪造 ValueReady 或 OutputCommitted）

Run cancellation：
  cancellation 获胜 -> RunTerminal(Cancelled)
  late/stale completion -> 只清理
  （不产生新的 ValueReady 或 durable receipt）

经过校验的 empty-plan / zero-work / no-op：
  no-work validation -> RunTerminal(Succeeded)
  （不产生新的 OperationReturned、ValueReady 或 durable receipt）

RunTerminal(Succeeded) -> ResultAvailable   （保留结果时）

compute-and-persist success =
  RunTerminal(Succeeded) AND OutputCommitted

Run 后置 output transaction failure =
  RunTerminal(unchanged) AND OutputCommitFailed
  （不产生或撤销 ValueReady；不返回声称达到请求 durability 的回执）

RequestAccepted、OutputCommitFailed、GraphDocumentSaved 与 ResponseObserved
由拥有相应操作的 owner 分别排序。
```

只有 dependency-valid Graph/RT publication 或一个已准入的合法 no-op 才会把
`ComputeRun::Succeeded` 解析为成功。Cache persistence、durable output commit、Graph 文档保存、
daemon terminal state、result availability 与 caller observation 都保持为独立 outcome。Run
后置 cache、codec 与 output 工作拥有自身类型化 outcome，不能延迟或改写已发布 Run terminal。
Run terminal 之后的输出失败报告 `OutputCommitFailed`，而不是
`RunTerminal(Failed)`，既不产生也不撤销 `ValueReady`。调用方或 daemon 可以报告组合
request failure，但必须保留 Run terminal 与 output、Graph 文档、cache/codec 和 response
事实，不能把聚合结果反投射回 Run state。在之后 Run cancellation 之前已经独立提交的
receipt 继续对该输出事务保持权威。

| 持久化领域 | 目标 authority | 完成与耐久契约 |
| --- | --- | --- |
| Graph 文档 | Graph-state save transaction | 带版本、same-directory staging、expected-version validation、atomic replacement 与显式 achieved-durability result |
| Disk cache | 使用有界 I/O mechanism 的 Graph cache policy | 可丢弃 acceleration；failure 不改写成功 Run/output outcome |
| 用户输出 | `OutputStore` commit authority | 稳定 `OutputCommitId`；完整 payload/metadata 校验与文件同步；canonical manifest 校验、同步和原子 no-replace 发布；从叶目录到 durability root 的目录屏障；类型化 achieved-durability receipt 或 `OutputCommitFailed`；recovery；且默认 no-overwrite |
| Daemon transport | Job registry 与 result delivery | 只负责 acceptance、terminal state 与 response observation；不能据此推断 durability |
| Codec | 注入的 representation adapter | 只负责 conversion 与 error translation；不拥有 path、retry、identity 或 commit authority |

Durable output retry 通过稳定 commit identity 保持幂等，delivery 是 at least once，而不是 exactly
once。Manifest publication 前的 cancellation 可以中止并清理 staging；manifest commit point 后
则报告已提交 receipt，而不是假装 output 已被取消。请求 crash durability 时，必须完整校验并
同步 payload/metadata 文件，完整写入并校验 canonical manifest，在原子 no-replace 发布前同步
manifest 文件，校验已发布 identity，并为事务创建、rename 或修改的每一级目录按从叶目录到配置
durability root 的顺序执行屏障。Atomic-visible receipt 可以在其较弱提交点后返回；只有所有更强
屏障都成功后才能返回 crash-durable receipt。不支持 file synchronization、directory barrier
或 atomic no-replace publication 时必须显式失败，绝不能静默降级 durability。

所有 persistent path 都必须 rooted 且 normalized，通过 no-follow/identity check 拒绝 escape 与
symlink substitution，在保留 work 前应用 quota，并把 achieved durability 暴露为 capability/
result。`ComputeIoExecutor` 只提供有界 mechanism；上述 domain authority 保留 identity、
ordering、policy 与 receipt ownership。

## 执行画像

交互和吞吐工作负载共享物理资源，但使用不同 profile。

交互行为优先保证有界 p50/p95/p99 response、latest-wins supersession、小型/自适应 region、
progressive quality、cooperative cancellation、device residency 和低复制本地输出。

Batch、render 和 testbench 行为优先保证 throughput、deterministic execution、resource reservation、
大型/自适应 partition、artifact durability、retry/checkpoint、traceability 和 golden comparison。

两类 profile 都不能饿死另一方。Admission 会预留 interactive headroom；持续交互流量下 batch 仍有
minimum progress guarantee。公平性按 estimated work、byte 或有界 quantum 计费，而不是原始 task 数。

## 服务器与插件隔离

`photospiderd` 继续作为同 UID 的本地 workstation sidecar。网络或多租户产品使用独立 control
plane、worker manager、受限 `photospider-worker` process 和 durable artifact store。

当前 operation plugin interface 继续作为临时 C++ ABI。其 C linkage registrar symbol 与数字
handshake 只拦截预期 interface generation；跨越 DSO 的 C++ value、callback、object 与 vtable 仍
要求匹配 SDK/toolchain/runtime compatibility。Policy plugin 改用 exact-layout 的纯 C ABI v1，并且
只接收 immutable scalar candidate snapshot，但仍属于受信任的 in-process code。未来 operation ABI
replacement、policy ABI generation 或隔离 invocation protocol 都属于独立的带版本迁移，不能从
当前 gate 推导出兼容或安全承诺。

`ExecutionService` 通过 `PluginInvocationExecutor` 看到隔离插件执行。独立
`PluginRuntimeSupervisor` 拥有 worker process、protocol、heartbeat、deadline、restart backoff、
sandbox/capability policy、shared-memory 或 FD transport、quota 和 output descriptor validation。
首条隔离路径面向 CPU operation plugin；跨进程 GPU handle 依赖后续 device/fence protocol。

## 跨域不变量

1. 只有 dispatcher-ready task 能进入执行域。
2. Run 只发布一个 terminal outcome，状态单调转换。
3. 可见提交前检查 revision、supersession generation 和 cancellation。
4. 在 operation contract 允许时，queued、start、operation chunk、dependency release、completion
   和 commit 路径都观察 cancellation。
5. Deadline 使用 monotonic clock；不可抢占 kernel 可以 overrun，但过期结果不能作为当前结果展示。
6. 每项 reservation 在成功、错误、取消或 worker failure 后恰好释放一次。
7. 新就绪 dependent work 重新进入全局 policy，不会通过 local queue 永久绕过公平性。
8. Graph close 停止该 graph 的新 Run admission，为已 admission Run 保留 settlement，并取消或
   排空它们；只有进程关闭才会在 admitted work quiesce 后停止整个执行域。
9. 第三方 policy 和 plugin code 不能制造 resource token，也不能突破 Host-owned quota。
10. Terminal publication 不表示 Run 可以回收；必须先让全部 lease/grant 达到 quiescence 并释放。
11. `(RunId, RunLocalTaskId)` 是 completion identity；policy-binding 或 execution-route generation
    不是 Run identity。
12. Run success 表示已验证 Graph/RT publication 或合法 no-op；它不表示 cache、output、Graph
    文档、daemon、delivery 或 response completion。
13. Cache 是可丢弃 acceleration，绝不是 durable user-output authority；cache persistence
    failure 不能改写已经成功的 Run 或 output commit。
14. Durable output commit 使用稳定 identity、经过完整校验与同步的 payload/metadata 和
    canonical manifest 文件、原子 no-replace manifest-last publication、从叶目录到
    durability root 的目录屏障与类型化 achieved-durability receipt；recovery 保持幂等，
    delivery 是 at least once，而不是 exactly once。
15. Graph 文档保存保持为独立的带版本 transaction；daemon terminal state 或 acknowledgement
    保持为非耐久 transport observation。
16. 平台无法达到用户请求的 durability level 时必须显式失败，绝不能静默降级。
17. 调用方或 daemon 可以把 Run、output、Graph 文档、cache/codec 与 response
    事实聚合为一个 request outcome，但必须保留每个 authority 自有事实，绝不能
    把 composite failure 反投射回 Run state。

## 依赖顺序

即使设计可以重叠，架构仍有依赖顺序：

```text
依赖中立内核
    ↓
ComputeRun 与 CPU 执行域
    ↓
通用数据与异构执行
    ↓
执行画像、server runtime 与插件隔离
```

每个领域的第一条可执行纵向切片都必须保持当前 Host 行为，并先增加长期测试，再扩大迁移。
接口重命名和所有权迁移遵守仓库纪律完整完成，不保留永久兼容 wrapper。特别是，进程执行域必须在
替换 per-graph 物理 worker 所有权时保留当前 bounded-admission error 与 rollback 保证。Issue #70
通过删除旧 counter 并引入经过 checked arithmetic 的多维 ledger 满足了该规则；后续切片必须扩展
该 ledger，不能恢复第二个 resource authority。
