# ADR 0007：ExecutionRun 与上下文资源由不同对象拥有

- 状态：已接受

## 1. 核心摘要 (TL;DR)
Graph context 拥有源 revision，execution context 拥有本地资源，每次执行调用拥有自己的 Run 状态与暂存结果。这样的边界允许 graph 替换和 worker 共享，同时避免保留或发布过期结果。

## 2. 架构心智模型

```text
GraphContext --> immutable snapshot --> compiler plan --+
                                                        +--> Run A --+--> caller results
ExecutionContext --> shared bounded CPU/GPU resources --+            |
                                                        +--> Run B --+ (separate state)
```

Run 会竞争共享 context 容量，同时各自保有取消状态、绑定、中间值和诊断。只有通过显式的 context-managed demand 机制才会共享计算。

## 3. 契约规约与接口

```cpp
class GraphContext { public: GraphSnapshot snapshot() const; std::uint64_t replace(WorkflowDocument); };
class ExecutionContext {
 public:
  Result<ExecutionResult> execute(const ExecutionPlan&, ExecutionBindings = {},
      const CancellationToken& = {}, const ExecutionOptions& = {});
};
```

Plan 捕获 graph 当前性与冻结 operation registry 身份。一次调用拥有自己的绑定快照和 Run 状态；返回的 Value 由调用方持有，可以比 context 活得更久。Worker 从队列取出一次尝试后，会在 transfer、资源准入或进入 operation 之前检查 cancellation 和 plan 当前性。若停止信号在此 cutoff 后出现，可能与进程内 callback 并发；callback 按协作方式排空，迟到的完成不能释放后继工作或发布结果。最终发布前还会再次检查 cancellation 和 plan 当前性。完整顺序见 [Compiler 和执行](../../kernel-architecture/Compiler-and-Execution.md)。

失败时丢弃收集模式的暂存结果。Streaming 调用同步交付 tile，因此后续 callback 失败时，已被 sink 消费的 tile 无法回滚。

## 4. 非目标与明确边界
- Run 状态没有 daemon Job ID 或持久身份。
- 取消采用协作方式。正在运行的 callback 可能必须返回后调用才能排空。
- 结果不由 `GraphContext` 保留；可选 context cache 是可丢弃的派生状态。

## 5. 后果与代价
Graph 替换会使旧 plan 失效，但不会停止无关 Run。调用方必须在直接执行期间保持 context 存活，并处理 `Stale`、取消和 callback 错误。析构会等待自有 callback 结束；慢 callback 会延迟资源释放。
