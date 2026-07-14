# ADR 0003：执行资源由进程拥有

## 状态

作为目标架构已接受；在迁移完成前，当前每图 scheduler 所有权仍是现行软件行为。

## 背景

当前每个 `GraphRuntime` 拥有 HP 和 RT scheduler 实例，而 scheduler interface 同时包含 policy、
worker lifecycle、queue、batch、device routing、completion 和 exception。多个 Graph 会乘法增加
物理 thread 和 device context，但 scheduler 仍无法表达跨 Run 公平性、进程内存上限、取消或全局
admission。

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

ADR 0001 完全有效。实施后，本决策会取代
`docs/kernel-architecture/Scheduler-Architecture.md` 当前每图 scheduler 章节描述的物理资源所有权，
但不会取代 ready-task-only scheduler 边界。
