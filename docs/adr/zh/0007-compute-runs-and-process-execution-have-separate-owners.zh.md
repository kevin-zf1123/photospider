# ADR 0007：Compute Run 与进程执行具有不同所有者

## 状态

作为目标架构已接受。作出本决策时，`ComputeRun`、显式注入且由进程拥有的
`ExecutionService`、`GraphRevision`、目标 `ResourceLedger`，以及仅负责策略的新一代
scheduler 都还不是当前软件行为。

本决策会细化 ADR 0003，并在详细所有权与生命周期契约上取代它。ADR 0003 仍作为“把物理执行
资源移出每个 Graph”的历史高层决策保留。ADR 0001 继续完全有效。

## 背景

当前实现让每个 `GraphRuntime` 分别拥有一个 HP 和一个 RT `IScheduler`。每个 scheduler
都拥有 worker、ready queue、epoch、completion counter、异常发布与排序策略。
函数内静态对象 `SchedulerWorkerBudget::process()` 会把所有 embedded Host 的保守 scheduler
worker 计费限制为 32，但它不拥有 worker、ready-store 容量、Run、内存、scratch、device、
I/O 或 plugin process 预算。

当前 `TaskHandle` 会借用一个 `TaskExecutor*`。`TaskSubmissionPlan`、临时结果、依赖状态和
executor 对象的生命周期局限在一次请求中，并由对应 scheduler completion wait 约束。
如果不先建立稳定的请求生命周期，就把这些 handle 移入进程级队列，会引入 use-after-free
与跨请求 completion 串扰风险。

ADR 0003 已选择 request-owned `ComputeRun`、process-owned `ExecutionService`、
host-owned `ResourceLedger` 和仅负责策略的 `SchedulerPolicy` 这一方向，但没有裁定：

- 一个 HP/RT 请求究竟拥有一个 Run，还是多个 domain Run；
- 哪些输入不可变，哪些请求局部对象需要稳定存储；
- Run 状态机、终态以及取消/提交竞争；
- ready submission 和 completion routing 的精确边界；
- 目标 `GraphRuntime` 的正向与负向所有权；
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

目标阶段按以下方向推进：

```text
Created -> Admitted -> Queued -> Running -> CommitPending -> Terminal
```

安全路径可以跳过部分非终态，包括 cache hit、admission failure 和提前取消路径。Run 永远不会
倒退，也不会离开 `Terminal`。

只能发布以下一种终态：

- `Succeeded`：只在经过验证的可见提交或经过验证的 no-op 成功之后；
- `Failed`：携带精确的 host-owned failure 或 exception category，包括 admission failure；
  或
- `Cancelled`：携带稳定原因，例如显式请求、Graph close、进程 shutdown、supersession
  或 deadline。

Operation completion 本身不等于成功。Dispatcher 必须完成依赖聚合，并且 graph-state 提交
事务必须验证 Run predicate。

取消意图是单调的。取消、失败和提交竞争者共用一个 Run-owned terminal arbiter。第一个被接受的
claim 获胜；后续 claim 只能完成清理与 telemetry。提交验证、可见发布和 success claim 组成
同一个 graph-state 串行事务，并共用同一个 terminal gate：

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

Service 永远不会接收或派生 `GraphModel`、`ComputePlan`、`ComputeTaskGraph`、dirty
snapshot/state、dependency map、cache authority 或可见 commit authority。
`TaskCompletion` 通过 Run lease 返回到匹配 dispatcher；dispatcher 必须在改变依赖状态前验证
Run 与 local-task namespace。新释放的 dependent work 会重新进入进程 admission、有界 ready
store 和全局 policy；永久 worker-local 路径不得绕过公平性、取消或 Run 隔离。

### 目标所有权边界

| 所有者 | 拥有 | 不拥有 |
| --- | --- | --- |
| Request / `ComputeRun` | Run 身份、不可变输入、请求计划与 dispatcher 状态、staged/temporary output、exception/cancellation/terminal 状态、Run reservation、commit policy、Run telemetry | Graph state、进程 worker、ready-store policy、资源铸造权威 |
| `GraphRuntime` | `GraphModel`、graph-scoped state、graph-state lane、单调 `GraphRevision`、revision capture、串行 commit validation/publication、graph event、platform/session metadata、Graph open/closing lifetime state | Run、CPU/device/I/O/plugin worker、进程 ready store、admission、`ResourceLedger`、`SchedulerPolicy`、物理 scheduler |
| 进程 `ExecutionService` | 物理 CPU worker 与后续 resource executor、有界 ready storage、Run/resource admission、policy 结果验证、执行异常 fence、completion routing | 任务规划/依赖、Graph/document persistence、cache authority、dirty propagation、可见 commit、Graph state |
| `ResourceLedger` | checked process limit、事务型 reservation、经过验证的 child grant、exact-once release accounting | 排序策略、任务依赖、Graph state、向第三方委托 token |
| `SchedulerPolicy` | 对不可变 ready descriptor 排序并建议有界 quantum | worker、ready store、Run、Graph state、reservation/grant/token、native device handle、executor、completion 或 lifecycle authority |

产品 composition root 会从显式进程配置构造一个 `ExecutionService` 并注入。它不是静态 singleton。
测试与 worker product 可以创建和销毁彼此隔离的 domain。同一个产品 composition 内的
embedded Host 会共享显式传入的 service。

Composition root 会先构造 service，再构造被注入的 Kernel/Host，并让 service 存活到这些 owner
停止 Run admission 且排空所有 Run。Planning、persistence、cache、dirty propagation 与
可见提交继续位于这个深模块之外。

### Host 权威的资源核算

`ExecutionService` 独占拥有一个内部 `ResourceLedger`，并用 composition-root limit 初始化。
只有可信 host code 可以铸造其 move-only、不可伪造 reservation 与 execution grant。
内建或第三方 policy、operation plugin 或 policy plugin 可以请求或建议资源，但不能构造、
复制、扩大或直接释放 token。

Ledger 最终会核算：

- CPU 执行容量；
- ready-store entry 与 byte；
- retained/in-flight memory 与 scratch；
- 每 device 的 queue、in-flight、memory 与 scratch 容量；
- compute-I/O operation 与 byte；以及
- plugin-process、invocation、IPC/shared-memory 与隔离容量。

Admission 会事务性验证一个 checked resource vector，只返回完整 Run reservation 或什么也不
返回。Policy 可以排列工作并建议有界 quantum。可信 service code 会根据进程 limit 与 Run
reservation 验证该建议，然后 ledger 才能铸造 child grant。

每个 reservation 与 grant 在成功、失败、取消、rollback、Graph close、进程 shutdown 或
worker failure 之后都会恰好释放一次。只要 child grant 仍存活，Run reservation 就不能释放。
Checked overflow 或 capacity exhaustion 永远不会导致 overcommit、部分 reservation 或静默
clamp。同步文档化 allocation exhaustion 继续表现为 `std::bad_alloc`；异步失败由精确 Run
failure channel 捕获，且不能提交部分输出。

当前 `SchedulerWorkerBudget` 在对应迁移切片之前继续作为当前过渡行为存在。它不会被包装、重命名
或 alias 成目标 ledger，并会随 per-Graph worker 所有权一起消失。

### 仅负责策略的新 scheduler generation

`SchedulerPolicy` 可以检查不可变 ready descriptor 与可信 host 提供的 capacity/telemetry
snapshot，排列 eligible work，并建议有界 quantum。可信 service code 会验证每个结果。无效或
超额建议不会执行工作，也不会改变 ledger 状态。

在内建 interactive 与 throughput policy 证明这一 seam 后，当前 worker-owning scheduler ABI
会作为一次完整破坏性迁移被替换。新一代 contract 不会适配 `IScheduler`、转发旧 worker-count
grant，也不会保留永久兼容 shim。issue #75 会在本所有权约束下裁定并实现具体替代 ABI。

### Revision、staged commit、取消与 supersession

`GraphRuntime` 会针对与计算正确性相关的 Graph mutation 推进单调 `GraphRevision`。Run 在规划前
捕获一个不可变 revision。目标 graph-state lane 只在 revision capture 和经过验证的可见 commit
期间持有，而不会覆盖长时间 planning/execution。

最低 commit predicate 要求：

- Graph 仍为 open，且 Run 保留有效 graph-lifetime lease；
- 当前权威 revision 等于捕获 revision；
- 可选 supersession key/generation 仍为 current；
- cancellation 或 failure 尚未 claim terminal state；
- dispatcher completion 与 staged output 对当前 Run 有效。

不能因为 topology 相等或输出相似，就推断 revision compatibility。任何未来 compatible-revision
优化都需要新的显式决策。验证失败会丢弃 staged output，且不能修改可见 Graph state。

Supersession 会让较新 generation 成为 current，并请求取消匹配的较旧 Run。它不会修改旧 Run
的计划或复用其身份。不可抢占工作与外部副作用可以完成，但 stale、cancelled、failed 或 overdue
output 不能提交。

### Close 与 shutdown 范围

Graph close：

1. 发布 graph closing，停止该 Graph 的新 Run admission 与普通外部 graph-state
   admission，并且只为已经 admission 的 Run settlement 保留 lifecycle admission；
2. 拒绝 visible commit，并只取消或排空属于该 Graph 的 Run；
3. 允许这些 Run 的 completion、cancellation 与 commit continuation 进入 graph-state
   finalization path，但只能观察 closing、丢弃 staged output、发布 terminal state 和释放
   ownership；
4. 等待这些 Run 的 ready、running、completion、dispatcher、commit 与 resource lease
   全部 quiescent；
5. 停止并排空 graph-state lane，再销毁 Graph state；
6. 让 `ExecutionService` 和无关 Graph Run 继续运行。

进程执行域 shutdown：

1. 停止全局新 Run admission 与普通外部 graph-state work admission；
2. 为每个已经 admission 的 Run 选择 cancellation 或 drain，同时只为这些 Run 保留有界
   ready submission、execution、completion routing 与 graph-state finalization；
3. settle 每个 Run，并恰好一次释放其 reservation 与 grant；
4. 在 Run quiescence 后停止其余 ready/execution admission；
5. join 所有物理 executor；
6. 销毁 `ExecutionService`。

Worker 与 operation exception 会在执行边界被 fence，并通过匹配 Run lease 路由。它们不能逃逸
worker thread、使不同 Run 失败或跳过资源释放。终态发布或 Graph close 后到达的 completion
只能执行清理。

## 交付边界

本决策冻结依赖契约，但不实现这些切片：

| Issue | 使用本决策的部分 | 该切片的明确非目标 |
| --- | --- | --- |
| #66 | HP `ComputeRun` descriptor、state、storage 与唯一终态 | 进程 worker 迁移 |
| #67 | 稳定 Run lease 与 `(RunId, RunLocalTaskId)` completion 隔离 | 共享 CPU service |
| #68 | 显式注入的单 Run CPU-only `ExecutionService`，只接受 ready 输入 | 多 Graph 迁移与最终 ledger |
| #69 | 多 Graph 与 HP/RT 共享 CPU domain；删除 per-Graph worker | 完整 admission/policy 模型 |
| #70 | Production admission、有界 ready store 与 `ResourceLedger` | 公平性算法 |
| #71 | Interactive 与 throughput 内建 policy | Plugin ABI 迁移 |
| #72 | `GraphRevision` capture 与 staged commit predicate | Cooperative cancellation |
| #73 | Queued/running/commit cancellation，汇合 #70 与 #72 | Latest-wins policy |
| #74 | #71 与 #73 之后的 latest-wins supersession | Scheduler ABI 替换 |
| #75 | #71 之后完整替换为 policy-generation ABI | 永久 old/new adapter |
| #76 | #69/#73/#74/#75 之后的 Graph close、进程 shutdown、telemetry 与最终不变量 | 新执行域能力 |

依赖图无环。#72 可以在 #67 之后与 #68–#71 并行；#75 可以在 #71 之后与 #72–#74 并行。
只有在每个实现及其长期行为测试落地后，当前事实文档才会改变。

## 结果

- 请求局部状态可以安全超过 caller stack 生命周期，而不需要把任务图正确性转交给执行 service。
- 不同 Run 可以复用 local task id，不会产生 completion 或 exception 串扰。
- 在目标架构中，CPU worker 数量不再由 Graph 数量决定，同时 Graph close 仍保持 graph-scoped。
- 一个可信 ledger 成为每种物理资源维度的最终权威；policy 与 plugin code 无法铸造容量。
- 取消可能在不可抢占工作被回收之前发布终态，因此测试和 telemetry 必须区分 terminal 与
  quiescent。
- 精确 revision equality 很保守，可能丢弃可复用工作；任何放宽都需要新的显式决策与证据。
- Scheduler ABI 迁移有意采用破坏性方式。
- Device、I/O、general data、plugin isolation 和 server control plane 细节可以继续演进，
  而无需改变 ready-task 或资源权威边界。

## 可测试性与故障不变量

随着行为落地，长期测试必须覆盖：

- Run id 与 local-task 隔离；
- 单调阶段与 exact-once 终态竞争；
- caller observer 析构与 lease-backed 生命周期；
- ready-only submission 与 dispatcher-owned dependent release；
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

### 把 SchedulerWorkerBudget 复用为 ResourceLedger

拒绝，因为这个过渡计数器的资源模型错误、采用隐藏的进程静态所有权，并且不拥有 Run、queue、
memory、scratch、device、I/O 或 plugin-process 权威。

### 允许 SchedulerPolicy 铸造 grant

拒绝，因为不可信或有缺陷的策略可能 overcommit 资源并规避 exact-release accounting。

### 在 adapter 后保留 worker-owning scheduler ABI

拒绝，因为物理 worker 与 token 会继续位于唯一 host-authoritative 执行域之外。

### 让 GraphRuntime 拥有 Run 或进程 ready store

拒绝，因为 Graph 数量会再次乘法放大物理所有权，Graph close 也可能停止无关工作。

## 与其他决策及文档的关系

- [ADR 0001](0001-graph-state-access-is-not-scheduler-dispatch.zh.md) 继续治理
  graph-state/ready-dispatch 分离。
- [ADR 0003](0003-process-owned-execution-resources.zh.md) 继续作为历史高层决策，只在目标所有权
  与生命周期细节上由本 ADR 取代。
- [ADR 0006](0006-kernel-documentation-separates-facts-decisions-targets-and-status.zh.md)
  要求当前事实、本 accepted target 决策、roadmap 方向与 Issue/Project 状态保持分离。
- [内核演进目标](../../roadmap/zh/Kernel-Evolution.zh.md) 记录持久目标与交付依赖顺序。
- 在实现与持久验证将各项目标提升为当前事实前，当前行为继续以
  [计算边界](../../kernel-architecture/zh/Compute-Boundaries.zh.md)、
  [计算流程](../../kernel-architecture/zh/Compute-Flow.zh.md) 和
  [调度器架构](../../kernel-architecture/zh/Scheduler-Architecture.zh.md) 为权威。
