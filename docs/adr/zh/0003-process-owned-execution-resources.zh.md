# ADR 0003：执行资源由进程拥有

## 状态

作为目标架构已接受。Issue #69 已实现现行 CPU 进程执行切片：embedded composition root
显式创建并注入一个固定的 `ExecutionService`；内建 CPU 的 HP 与 RT 工作（包括 connected-parameter
preflight，以及 dirty source/downstream 阶段）只以已 ready 且由 lease 支撑的 submission 进入
该 service。来自多个 Graph 的独立 Run 可以在该 pool 上重叠，内建 CPU intent binding 也不再
分配 per-Graph scheduler owner。Serial、GPU 与 plugin route 仍是过渡期 per-Graph scheduler。
进程级 scheduler-worker budget 现在既计算每个固定 service pool，也计算 legacy scheduler worker，
但它仍只是 containment 步骤，而不是目标资源 owner。最终 resource accounting、公平性 policy、
`RunGroup`、取消与 revision-safe commit 仍是目标行为。ADR 0007 只在详细所有权与生命周期契约上
取代本 ADR；进程级所有权的高层决策及其历史背景继续有效。

## 背景

当前每个 `GraphRuntime` 都为各 intent 拥有一个 binding。该 binding 要么拥有过渡期 serial、
GPU 或 plugin scheduler，要么选择注入的内建 CPU `ExecutionService`，而不拥有 Graph-owned
scheduler。Legacy scheduler interface 仍同时包含 policy、worker lifecycle、queue、batch、
device routing、completion 和 exception。Service 则会在首次使用前冻结一个 CPU worker 数量，
为每个 Run 保持隔离的 completion/failure/trace state，并允许来自多个 Graph 的独立 HP 与 RT
Run 重叠。

当前软件通过解析后的 grant，以及所有 embedded Host 共享的一个保守 32-slot 进程 budget，
限制过渡期 worker 的乘法增长。该 budget 对每个 Kernel 的固定 CPU service pool 计费一次，并对
legacy scheduler-owned worker 按 Graph 计费。但这个 budget 与当前双优先级 service 都无法表达
最终的跨 Run 公平性、进程内存上限、取消，也无法准入 worker slot 之外的资源。

如果没有稳定 Run 生命周期和 Host-owned 资源账本，只把这些 scheduler 移入全局对象仅仅是搬移问题。

## 决策

一个显式 process-owned `ExecutionService` 拥有物理 CPU worker、device executor、compute I/O
worker、ready-store capacity、admission 和资源账本。它由产品组合根创建并注入，不是静态 singleton。

`ComputeRun` 是 request-owned 的计算身份、取消、临时输出、终态、graph revision、supersession、
resource reservation 和 commit policy 单元。

`ComputeTaskDispatcher` 继续拥有任务依赖和 ready detection。只有 `ReadyTaskSubmission` 值进入
`ExecutionService`，从而保留 ADR 0001。

`SchedulerPolicy` 是 `ExecutionService` 的内部策略 seam。它排列 ready work 并建议有界 grant，
但不拥有 thread、queue、resource token、Graph state 或 native device handle。Host-owned
`ResourceLedger` 验证所有 grant，并保证每项 reservation 恰好释放一次。

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

ADR 0001 完全有效。Issue #69 已取代
`docs/kernel-architecture/Scheduler-Architecture.md` per-Graph scheduler 章节中描述的内建
CPU 物理所有权：HP、RT、preflight 与 dirty ready work 都会在注入的固定 service 上执行。
内建 CPU binding 在 `GraphRuntime` 中不拥有 owner；serial、GPU 与 plugin route 仍保留过渡期
scheduler owner。ready-task-only 边界继续完全有效。

当前 containment contract 接受零到八的 worker 请求，把零解析为上限八的非零 grant，并在所有
embedded Host 之间最多预留 32 个保守 worker slot，同时只冻结一次 service 数量。每个固定 CPU
service 都由一项 pool-lifetime RAII reservation 覆盖。Graph load 只为 legacy HP/RT owner
预留；legacy replacement 仍需要 transient candidate headroom；内建 CPU load 或 replacement
会发布 ownerless service route，且绝不调整 pool 大小。这能阻止无界 worker 乘法增长，而 legacy
worker、queue、epoch 与 policy 仍位于各自的 `IScheduler` 内。

因此，`SchedulerWorkerBudget` 既不是当前 `ExecutionService`，也不是目标
`ResourceLedger`：它不拥有 executor、Run identity、cancellation、fairness、
memory/device/I/O quota 或 ready-store capacity。它的 slot 会计算固定 service pool 与 legacy
scheduler worker，但不会计算所有进程 thread。后续迁移会完整替换这个过渡 ownership 与 ABI
boundary，而不会保留兼容 wrapper。

## 与 ADR 0007 的关系

[ADR 0007](0007-compute-runs-and-process-execution-have-separate-owners.zh.md)
保留本决策的进程级执行方向与 ADR 0001 边界，同时取代原先隐含的细节。Run 身份与 lease、
单调终态、completion routing、目标 `GraphRuntime` 的非所有权、账本 token 权威、提交竞争、
Graph/进程 shutdown 范围，以及 issue #66–#76 的依赖契约，均以 ADR 0007 为权威。
