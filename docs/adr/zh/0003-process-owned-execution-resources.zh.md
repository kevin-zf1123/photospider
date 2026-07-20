# ADR 0003：执行资源由进程拥有

## 状态

作为目标架构已接受。Issue #68 已实现第一个现行进程执行切片：embedded composition root
显式创建并注入一个 CPU `ExecutionService`，内建 CPU 的非 realtime、非 dirty full-HP Run
只把已 ready 且由 lease 支撑的工作提交到它的单 Run 物理 worker domain。所有路由仍保留已分配
的过渡期 per-Graph scheduler owner，serial、GPU、plugin、dirty 与 realtime 执行仍直接使用
这些 owner。进程级 scheduler-worker admission ledger 仍是 containment 步骤，而不是目标资源
owner。多 Graph 共享、通用 dirty/realtime lease 覆盖、resource accounting、policy 与
revision-safe commit 仍是目标行为。ADR 0007 只在详细所有权与生命周期契约上取代本 ADR；
进程级所有权的高层决策及其历史背景继续有效。

## 背景

当前每个 `GraphRuntime` 仍拥有 HP 和 RT scheduler 实例，而 scheduler interface 同时包含
policy、worker lifecycle、queue、batch、device routing、completion 和 exception。对于
issue #68 的内建 CPU full-HP 切片，私有 route metadata 会选择注入的 `ExecutionService`；
其 concrete CPU runtime 会按精确 planning grant 重新配置，并且每次执行一个完整 Run。
该 service route 运行时，过渡期 Graph-owned CPU scheduler 仍保持存活且被计费，因此在
issue #69 删除 per-Graph worker 前，这个切片有意保留重复 worker 所有权。

当前软件通过解析后的单实例 worker grant，以及所有 embedded Host 共享的一个保守 32-slot
进程 ledger，限制过渡期资源的乘法增长。但这个 ledger 与当前单 Run service 都无法表达跨 Run
公平性、进程内存上限、取消、多 Graph shared execution，也无法准入 scheduler-owned worker
slot 之外的资源。

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

ADR 0001 完全有效。Issue #68 已部分取代
`docs/kernel-architecture/Scheduler-Architecture.md` per-Graph scheduler 章节描述的物理
所有权：只有内建 CPU 的非 realtime、非 dirty full-HP ready work 会在注入 service 上执行。
其余路由和过渡期 scheduler owner 仍是现行行为，ready-task-only 边界继续完全有效。

当前 containment contract 接受零到八的 worker 请求，把零解析为上限八的非零 grant，并在所有
embedded Host 之间最多预留 32 个保守 scheduler-worker slot。Graph load 会共同预留 HP+RT，
replacement 需要 transient candidate headroom，move-only RAII owner 只有在 concrete scheduler
销毁后才释放 slot。这能阻止无界的 per-graph 乘法增长，但 worker、queue、epoch 与 policy 仍位于
每个 `IScheduler` 内。

因此，`SchedulerWorkerBudget` 既不是当前 `ExecutionService`，也不是目标
`ResourceLedger`：它不拥有 executor、Run identity、cancellation、fairness、
memory/device/I/O quota 或 ready-store capacity，其 slot 既不计算 service pool，也不计算所有
进程 thread。后续迁移会完整替换这个过渡 ownership 与 ABI boundary，而不会保留兼容 wrapper。

## 与 ADR 0007 的关系

[ADR 0007](0007-compute-runs-and-process-execution-have-separate-owners.zh.md)
保留本决策的进程级执行方向与 ADR 0001 边界，同时取代原先隐含的细节。Run 身份与 lease、
单调终态、completion routing、目标 `GraphRuntime` 的非所有权、账本 token 权威、提交竞争、
Graph/进程 shutdown 范围，以及 issue #66–#76 的依赖契约，均以 ADR 0007 为权威。
