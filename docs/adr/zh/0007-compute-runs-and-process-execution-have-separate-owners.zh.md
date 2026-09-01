# ADR 0007：Execution Run 与本地资源具有不同所有者

- 状态：已接受，由 ADR 0015 收窄
- 日期：2026-09-01 边界修订

## 背景

独立 graph 可以共享 local worker，但不能共享 source state。同样，被取消或 stale 的
execution 必须释放资源且不能发布 staged result。因此 Graph、Run 与 physical-resource
lifetime 必须分离。

## 决策

`GraphContext` 只拥有 copied `WorkflowDocument`、当前 nonzero revision 与 snapshot
currentness state。它不拥有 worker、device queue、result registry 或 execution lifetime。

`ExecutionContext` 拥有 ADR 0003 定义的 fixed local pool、deterministic per-lane FIFO、
它们的 single context-wide waiting-callback admission、frozen operation set 与 modeled-byte
ledger。Running callback 已不再 waiting，因此不占该 shared queue bound。

每次 `ExecutionContext::execute` 调用创建一个 source-private `ExecutionRun`。Run 拥有：

- immutable plan reference 与 captured currentness predicate；
- dependency count、deterministic ready-step priority 与 per-call parallelism；
- intermediate Value 及其 Run-local backend label；
- cooperative cancellation observation 与第一个 terminal failure；
- operation timing、transfer、fallback、peak-byte 与 result diagnostics。

Kernel 不定义 public 或 daemon-shaped Run identifier。Run identity 是单次同步 `execute`
调用内部的 object ownership。

### Completion 与 publication

只有 caller token 未取消且 plan 捕获的 graph revision 仍为 current 时，operation completion
才会被接受。被拒绝的 late completion 会回收 callback/byte lease，但不能释放 dependent
step，也不能进入 result map。

全部 requested output 可用后，final result assembly 只执行一次。返回 `ExecutionResult`
前再次检查 cancellation/currentness gate。Failure 返回一个 typed status，并丢弃 staged
output。Result 是 caller-owned in-memory Value；graph context 不保留或发布它们。

### Determinism 与 fallback

Ready step 按 plan index 排序；只有成功 predecessor 才会释放 dependency。CPU 是必需能力。
只有 operation trait 允许时，GPU attempt 才能 fallback 到 CPU；两个 attempt 都保留在 raw
diagnostics 中。

## 边界

该 Run 不是 daemon Job，在调用之外没有 queue/status/result identity。不存在 retry、attempt
record、persistence、remote execution、external scheduler、policy DSO 或 security authority。

## 结果

- Graph replacement 使旧 work 失效，但不停止无关 context。
- 共享 local pool 不意味着共享 graph/result state。
- Cancellation、exception 与 staleness 具有精确 no-publication path。
- Daemon orchestration 可以包装 public call，但不会变成 kernel state。
