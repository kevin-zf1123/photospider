# ADR 0003：执行资源由进程拥有

## 状态

作为目标架构已接受。Issue #70 至 #75 已实现现行 CPU 执行/资源、policy 与私有 route 切片：每个
embedded composition root 显式创建并注入一个固定的 `ExecutionService`；内建 CPU 的 HP 与 RT
工作（包括 connected-parameter preflight，以及 dirty source/downstream 阶段）只以已 ready 且由
lease 支撑的 submission 进入该 service。来自多个 Graph 的独立 Run 可以在该 pool 上重叠。
`GraphRuntime` 只存储复制的 HP/RT route id 与 nonzero generation；它不拥有物理 worker、queue、
policy context 或 plugin DSO lifetime。Service 独占一个 Host 权威 ledger 与按 entry/byte 有界的
ready store；完整 CPU/retained/scratch/ready Run vector 共享该 authority。恰好一个 Interactive 与
一个 Throughput policy binding 会在 Host 编写的 class、frontier、fairness 与 fallback 规则之后
排列工作。Issue #72 把强类型 Graph identity、authoritative revision、request-owned staging 与
revision-safe publication 保持在 execution service 之外。Issue #73 为每个当前 Run 提供私有弱
生命周期 cancellation source、read-only lease/deadline observation、唯一 terminal/commit arbiter、
精确 Run queued purge、running drainage、dependent rejection 与 RT-denies-HP cancellation。Issue
#74 增加 request-level realtime `RunGroup`、checked per-Graph latest-wins generation、在现有
compute-lane worker 上运行的有界 ticket-backed coalescing，以及 current-generation commit
authority。Issue #75 移除拥有 worker 的 scheduler SDK/ABI，并增加纯 C policy ABI v1、原子
binding replacement、generation-local sticky fault、reserved start 与封闭的私有 execution route。
最终 lifecycle registry/graph-close/process-shutdown/telemetry contract（#76），以及 public
Host/CLI/IPC cancellation control 仍是未来行为。ADR 0007 只在详细所有权与生命周期契约上
取代本 ADR；进程级所有权的高层决策及其历史背景继续有效。

## 背景

当前每个 `GraphRuntime` 都为 HP 与 RT intent 存储复制的 route binding。Route vocabulary 封闭为
`cpu`、`serial_debug` 与 `gpu_pipeline`；其物理 worker、queue、device routing、completion
与 exception 都保持为 Host execution module 的私有实现。Policy binding 是 process/service state，
绝不属于 Graph state。Service 根据 composition-root configuration 冻结一个 CPU worker 数量，为每个
Run 保持隔离的 completion/failure/trace state，并允许来自多个 Graph 的独立 HP 与 RT Run 重叠。

当前软件使用每个 Host ledger 默认的 32-slot CPU 维度作为 Run execution grant。固定 service
worker 与 route machinery 属于基础设施。Retained Host memory、scratch、ready entry 与 ready byte
也会被准入。当前 service
执行 Issue #71 的 CPU 公平性与 headroom 契约。在 Issue #72 的交付快照中，exact revision
validation 仍是 service 之外的 Kernel/graph-state commit concern，cancellation 与 supersession
则仍不属于那个历史切片。当前软件已把 Issue #73 cooperative cancellation 实现为 Run-owned
terminal correctness：service 只观察并 purge/drain 匹配的 Run，graph-state transaction 则仲裁
cancellation 与 commit。Latest-wins supersession 与 request-level realtime grouping 现在已是
Issue #74 当前行为；lifecycle-driven graph-close/process-shutdown cancellation 仍属于 Issue #76。
Issue #75 把 policy comparison 与 execution ownership 分离：Host 构造 frontier 并验证 decision，
纯 C callback 只能选择一个 immutable candidate 或 abstain。

如果没有稳定 Run 生命周期和 Host-owned 资源账本，只把物理 executor 移入全局对象仅仅是搬移问题。

## 决策

一个显式 process-owned `ExecutionService` 拥有物理 CPU worker、device executor、compute I/O
worker、ready-store capacity、admission 和资源账本。它由产品组合根创建并注入，不是静态 singleton。

`ComputeRun` 是 request-owned 的计算身份、取消、临时输出、终态、graph revision、supersession、
resource reservation 和 commit policy 单元。

`ComputeTaskDispatcher` 继续拥有任务依赖和 ready detection。只有 `ReadyTaskSubmission` 值进入
`ExecutionService`，从而保留 ADR 0001。

Policy binding 是 `ExecutionService` 的内部比较 seam。恰好一个 Interactive 与一个 Throughput
binding 通过同一条 Host 编写的 frontier 与 validation path 排列已经准入的 ready work。它们不拥有
thread、物理 ready store、resource token、budget、Graph state、native device handle、completion
route 或 lifecycle authority。Service 拥有 binding state 与 store，而 Host-owned
`ResourceLedger` 验证所有 reservation，并保证其恰好释放一次。`PolicyRegistry` 拥有 immutable
built-in 与 DSO policy type record；DSO callback 使用自包含的 C11 policy ABI v1，并且只接收
scalar candidate snapshot。

物理执行划分为 resource executor：

- 进程 CPU executor；
- 每个物理 GPU/device 一个 executor，拥有 native queue 和 fence；
- 有界 compute I/O executor；
- plugin invocation adapter，由独立 `PluginRuntimeSupervisor` 负责 process、IPC、安全和故障隔离。

拥有 worker 的 scheduler plugin ABI、SDK target、`IScheduler` hierarchy 与 per-Graph 物理 owner
已经通过一次完整的破坏性迁移被移除。没有留下 compatibility adapter 或 forwarding layer。

## 结果

- Thread 和 device-queue 数量由进程配置控制，不再由 Graph 数量决定。
- Interactive 和 throughput Run 可以在显式公平性、deadline 和 headroom policy 下共享资源。
- Graph revision、取消和 stale-result rejection 成为 Run-level 正确性规则，而不是 policy-binding
  或 route-generation hint。
- GPU completion 和 I/O continuation 可能超过 caller stack 生命周期，因此 task handle 需要稳定
  Run lease，而不是借用 executor pointer。
- 该 service 必须保持为深模块；Graph planning、persistence、cache authority 和 commit 语义在其外部。
- Plugin process supervision 保持独立，避免执行资源所有权变成巨型安全子系统。

## 与当前文档的关系

ADR 0001 完全有效。Issue #69 至 #75 已取代
`docs/kernel-architecture/Policy-and-Execution-Architecture.md` 历史版本中描述的 per-Graph 物理
所有权与拥有 worker 的 scheduler model：HP、RT、preflight 与 dirty ready work 都会经过注入的
固定 service。`GraphRuntime` 只拥有复制的 route id/generation；serial-debug、shared-CPU 与
GPU-pipeline execution 都保持在私有 Host route 之后。ready-task-only 边界继续完全有效。Issue #72 还会把 request-owned staged
Graph/proxy state、精确 identity/revision validation 与 visible publication 保持在该边界的
compute/graph-state 一侧。Issue #73 会在同一侧增加 private request cancellation coordinator、
彼此独立的 HP/RT child source、cooperative monotonic deadline expiry 与 Run-owned terminal/commit
contention。`ExecutionService` 会注册 read-only cancellation notification，只清除匹配 Run 的
queued entry、抑制 dependent re-entry，并等待 non-preemptible running callback 排空；它不会成为
cancellation authority 或 visible-commit owner。

Composition-root execution configuration 会解析并冻结 service worker count；它不是 policy-plugin
grant。每个 Host ledger 都拥有不可变 composition limit。Run admission 会在 queue publication 前
提交一个完整 vector；initial 与 dependent submission 会进入同一个 policy-aware 有界 store，并在
临时为空时保留同一 Run fairness row。Ready cost 为 `work_units + ceil(bytes / 4096)`；Graph 在每个
已选 service class 中独立按原始 cost 计费，Run 在自己的不可变 class 中按
`ceil(cost / weight)` 计费。Host 会选择 class、构造有界 frontier、在 Throughput 仍可 start 时最多
允许三个连续 Interactive start，并针对 original snapshot 与当前 state 验证每个 built-in 或 DSO
decision。首个无效 DSO decision 对其精确 binding generation 保持 sticky。Reserved start 会在任何
executor callback 开始前，原子移除精确 ready entry、把 ready authority 交换为 execution grant、
更新 fairness/burst state，并把 callback ownership 转移给私有 route。

配置的 interactive headroom 只把 active Throughput root reservation 限制在 general ceiling。
Interactive Run 不会扣减该 class quota，而 ledger 仍是全部共享物理容量的最终 authority。
Throughput check、reservation 与 class charge 是原子的；该 charge 会一直保留到全部 child grant
结束后的精确 root release。在 graph-state commit contender 之前被接受的 cancellation 不会发布
Graph、proxy 或 deferred cache state。Contender 一旦获胜，后续 cancellation 就是 no-op；
predicate/persistence failure 或 visible success 会解析同一个 Run arbiter。Proxy commit 之前的 RT
cancellation 会拒绝并取消 HP，而 HP cancellation 不能回滚已经提交的 RT proxy。

Graph load 与 route replacement 只复制经过验证的 route id 与 nonzero generation，不预留或构造
Graph-owned 物理 owner。Service-level policy replacement 会在 publication 之前准备新 context、原子
发布一个新 generation、排空旧 generation 的 active invocation，并恰好一次 retire 其 context/DSO
lease。原 worker-only budget 与 scheduler SDK 已经完整删除，不保留 wrapper、alias 或第二
authority。

## 与 ADR 0007 的关系

[ADR 0007](0007-compute-runs-and-process-execution-have-separate-owners.zh.md)
保留本决策的进程级执行方向与 ADR 0001 边界，同时取代原先隐含的细节。Run 身份与 lease、
单调终态、completion routing、目标 `GraphRuntime` 的非所有权、账本 token 权威、提交竞争、
Graph/进程 shutdown 范围，以及 issue #66–#76 的依赖契约，均以 ADR 0007 为权威。
