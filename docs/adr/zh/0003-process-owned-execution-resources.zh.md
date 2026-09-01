# ADR 0003：本地执行资源具有显式所有者

- 状态：已接受，由 ADR 0015 收窄
- 日期：2026-09-01 边界修订

## 背景

可嵌入 kernel 必须运行独立 graph，不能创建隐藏的 process singleton，也不能让
physical worker 数量随 graph 数量增长。Queue capacity、modeled memory、optional GPU
work、cancellation 与 shutdown 需要一个可见 owner。

## 决策

每个 `ExecutionContext` 拥有一套固定的 local execution composition：

- deterministic FIFO 与固定 CPU worker pool；
- 可选的单 worker local GPU lane，拥有自己的 deterministic FIFO；
- 跨两个 FIFO 的一个 nonblocking shared waiting-callback admission limit；
- 被全部 invocation 共享的 frozen `OperationRegistry`；
- 对 configured modeled-byte limit 进行 exact release 的 `ResourceLedger`。

Context configuration 在构造后不可变。CPU worker 为零时选择有上限、基于硬件的数量；
CPU execution 始终存在。Queue 或 byte limit 为零属于非法配置。Embedder 可以创建多个
独立 execution context，也可以让多个 graph 显式共享一个 context。

### Admission 与 work ownership

每次 `execute` 调用创建一个 private `ExecutionRun`。Run 拥有 dependency counter、
deterministic ready-step ordering、staged Value、per-step backend residency、in-flight
accounting、cancellation observation 与 raw diagnostics。
`ExecutionOptions::maximum_parallelism` 限制该 Run 的 in-flight plan step。
`maximum_queued_tasks` 是一个 ExecutionContext-wide limit，统计任一 backend FIFO 已接受但
尚未开始的 callback；它不会按 lane 重复计算。Worker pop callback 时释放 move-only waiting
token，因此 running callback 不占 waiting limit。Enqueue rejection、allocation failure、
shutdown drop 与 exception unwinding 都会把 token 恰好回滚一次。该 shared admission 与
byte ledger 提供 nonblocking backpressure。

调用 operation 前，Run 获取该 step 的完整 planned-byte charge。Move-only lease 在 attempt
后恰好释放一次，包括 fallback、exception、cancellation 与 stale-completion 路径。

### Local transfer 与 fallback

CPU backend 是必需能力。GPU lane 启用时，是另一个 local in-process callback lane。若
dependency Value 由不同 backend 产生，execution 会执行显式 immutable byte copy，并记录
transfer count/bytes。Residency 是 Run-local derived state，不会持久化，也不是 global
manager。

只有 copied operation trait 允许时，unavailable GPU capability 与 recoverable GPU failure
才可 fallback 到 CPU；原因只作为普通 diagnostic。

### Cancellation、exception 与 shutdown

每个 callback 都有 exception fence。Cooperative cancellation 与 graph-revision
currentness 会在 admission 前、completion 期间和 final result assembly 前检查。Late work
可以完成 cleanup，但不能在 cancellation 或 staleness 后发布 caller result。

析构会停止 queue admission、拒绝 queued callback、释放每个 dropped waiting token、唤醒
worker 并 join 自有线程。调用者不得让 `execute` 与 context 析构并发。

## 边界

全部资源都在进程内。本 ADR 不创建 daemon Session/Job、external scheduler、remote
worker、process supervisor、plugin sandbox、durable state 或 security domain。

## 结果

- Physical resource 数量跟随显式 context configuration，而不是 graph 数量。
- Queue/byte backpressure 有界且可测试。
- Transfer、fallback、stale rejection 与 cleanup 具有唯一 Run-local 路径。
- 多个 context 可并发执行，不需要 kernel-global registry。
