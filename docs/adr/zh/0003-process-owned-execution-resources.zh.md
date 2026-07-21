# ADR 0003：执行资源由进程拥有

## 状态

作为目标架构已接受。Issue #70 与 #71 已实现现行 CPU 执行/资源与调度切片：每个 embedded
composition root 显式创建并注入一个固定的 `ExecutionService`；内建 CPU 的 HP 与 RT 工作（包括
connected-parameter preflight，以及 dirty source/downstream 阶段）只以已 ready 且由 lease 支撑的
submission 进入该 service。来自多个 Graph 的独立 Run 可以在该 pool 上重叠，内建 CPU intent
binding 也不再分配 per-Graph scheduler owner。Serial、GPU 与 plugin route 仍是 per-Graph
scheduler。Service 独占一个 Host 权威 ledger 与 policy-aware、按 entry/byte 有界的 ready store；
完整 CPU/retained/scratch/ready Run vector 与保守 legacy scheduler CPU slot 共享该 authority。
私有 interactive 与 throughput 策略通过 work/byte cost、分层 Graph/Run 计费、deadline 偏好、
aging、interactive headroom 与有界 throughput 进展来排列工作。Issue #72 现在会把强类型 Graph
identity、authoritative revision、request-owned staging 与 revision-safe publication 保持在
execution service 之外。Device、I/O 与 plugin-specific accounting、`RunGroup`、取消和
generation supersession 仍是目标行为。
ADR 0007 只在详细所有权与生命周期契约上
取代本 ADR；进程级所有权的高层决策及其历史背景继续有效。

## 背景

当前每个 `GraphRuntime` 都为各 intent 拥有一个 binding。该 binding 要么拥有过渡期 serial、
GPU 或 plugin scheduler，要么选择注入的内建 CPU `ExecutionService`，而不拥有 Graph-owned
scheduler。Legacy scheduler interface 仍同时包含 policy、worker lifecycle、queue、batch、
device routing、completion 和 exception。Service 则会在首次使用前冻结一个 CPU worker 数量，
为每个 Run 保持隔离的 completion/failure/trace state，并允许来自多个 Graph 的独立 HP 与 RT
Run 重叠。

当前软件通过解析后的 grant，以及每个 Host ledger 默认的 32-slot CPU 维度，限制 legacy worker
的乘法增长。固定 service worker 属于基础设施；active Run 与 legacy scheduler owner 会竞争同一
CPU authority。Retained Host memory、scratch、ready entry 与 ready byte 也会被准入。当前 service
执行 Issue #71 的 CPU 公平性与 headroom 契约。Issue #72 的 exact revision validation 仍是
service 之外的 Kernel/graph-state commit concern；取消、supersession 以及新的 device/I/O/plugin
维度仍不属于该切片。

如果没有稳定 Run 生命周期和 Host-owned 资源账本，只把这些 scheduler 移入全局对象仅仅是搬移问题。

## 决策

一个显式 process-owned `ExecutionService` 拥有物理 CPU worker、device executor、compute I/O
worker、ready-store capacity、admission 和资源账本。它由产品组合根创建并注入，不是静态 singleton。

`ComputeRun` 是 request-owned 的计算身份、取消、临时输出、终态、graph revision、supersession、
resource reservation 和 commit policy 单元。

`ComputeTaskDispatcher` 继续拥有任务依赖和 ready detection。只有 `ReadyTaskSubmission` 值进入
`ExecutionService`，从而保留 ADR 0001。

`SchedulerPolicy` 是 `ExecutionService` 的内部策略 seam。
`InteractiveSchedulerPolicy` 与 `ThroughputSchedulerPolicy` 排列已经准入的 ready work。它们不拥有
thread、物理 ready store、resource token、budget、Graph state 或 native device handle。Service
拥有 policy state 与 store，而 Host-owned `ResourceLedger` 验证所有 reservation，并保证其恰好
释放一次。

物理执行划分为 resource executor：

- 进程 CPU executor；
- 每个物理 GPU/device 一个 executor，拥有 native queue 和 fence；
- 有界 compute I/O executor；
- plugin invocation adapter，由独立 `PluginRuntimeSupervisor` 负责 process、IPC、安全和故障隔离。

在内建 policy 证明新 seam 后，当前拥有 worker 的 scheduler plugin ABI 会作为一次完整破坏性迁移
被替换。

## 结果

- Thread 和 device-queue 数量由进程配置控制，不再由 Graph 数量决定。
- Interactive 和 throughput Run 可以在显式公平性、deadline 和 headroom policy 下共享资源。
- Graph revision、取消和 stale-result rejection 成为 Run-level 正确性规则，而不是 scheduler epoch hint。
- GPU completion 和 I/O continuation 可能超过 caller stack 生命周期，因此 task handle 需要稳定
  Run lease，而不是借用 executor pointer。
- 该 service 必须保持为深模块；Graph planning、persistence、cache authority 和 commit 语义在其外部。
- Plugin process supervision 保持独立，避免执行资源所有权变成巨型安全子系统。

## 与当前文档的关系

ADR 0001 完全有效。Issue #69 至 #71 已取代
`docs/kernel-architecture/Scheduler-Architecture.md` per-Graph scheduler 章节中描述的内建
CPU 物理所有权：HP、RT、preflight 与 dirty ready work 都会在注入的固定 service 上执行。
内建 CPU binding 在 `GraphRuntime` 中不拥有 owner；serial、GPU 与 plugin route 仍保留 legacy
scheduler owner。ready-task-only 边界继续完全有效。Issue #72 还会把 request-owned staged
Graph/proxy state、精确 identity/revision validation 与 visible publication 保持在该边界的
compute/graph-state 一侧。

当前 contract 接受零到八的 worker 请求，把零解析为上限八的非零 grant，并只冻结一次 service
数量。每个 Host ledger 都拥有不可变 composition limit。Run admission 会在 queue publication
前提交一个完整 vector；initial 与 dependent submission 会进入同一个 policy-aware 有界 store，
并在临时为空时保留同一 Run fairness row。Ready cost 为
`work_units + ceil(bytes / 4096)`；Graph 在每个已选 service class 中独立按原始 cost 计费，Run
在自己的不可变 class 中按 `ceil(cost / weight)` 计费。同一 policy class 内更早的 interactive
deadline 优先；ready item 在八次成功 dispatch 后 aging；throughput 持续 ready 时，至多允许三次
interactive dispatch，随后必须保证 throughput 进展。配置的 interactive headroom 只把 active
内建 Throughput root reservation 限制在 general ceiling。Interactive 与过渡期 Issue #70 legacy
root 不会扣减该 class quota，而 ledger 仍是全部共享物理容量的最终 authority。Throughput check、
reservation 与 class charge 是原子的；该 charge 会一直保留到全部 child grant 结束后的精确 root
release。Graph load 只为 legacy HP/RT owner 预留；
legacy replacement 在旧 owner 保持存活时仍需要 transient candidate capacity；内建 CPU load 或
replacement 会发布 ownerless service route，且绝不调整 pool 大小。原 worker-only budget 已经
完整删除，不保留 wrapper、alias 或第二 authority。

## 与 ADR 0007 的关系

[ADR 0007](0007-compute-runs-and-process-execution-have-separate-owners.zh.md)
保留本决策的进程级执行方向与 ADR 0001 边界，同时取代原先隐含的细节。Run 身份与 lease、
单调终态、completion routing、目标 `GraphRuntime` 的非所有权、账本 token 权威、提交竞争、
Graph/进程 shutdown 范围，以及 issue #66–#76 的依赖契约，均以 ADR 0007 为权威。
