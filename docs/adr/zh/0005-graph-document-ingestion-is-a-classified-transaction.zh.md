# ADR 0005：GraphContext 先发布源文档再编译

- 状态：已接受

## 1. 核心摘要 (TL;DR)
`GraphContext` 保存不可变的 `WorkflowDocument` 副本，并在替换时发布新的 revision。`Compiler` 将 snapshot 验证为完整且不可变的阶段对象。源发布与语义有效性分开，使无效输入不会泄漏部分 compiler 结果。

## 2. 架构心智模型

```text
caller document --> GraphContext --snapshot/revision--> analyze --> optimize --> plan
                         |                                  |           |         |
                         +-- replace: revision advances ----+-----------+---------+
                                                            stale stages rejected
```

Context 串行化 snapshot 捕获与文档替换。Compiler 阶段在发布结果前检查捕获的 revision；替换源文档会让旧阶段失效。

## 3. 契约规约与接口

```cpp
struct WorkflowDocument { std::uint32_t schema_version = 3; /* inputs, nodes, outputs */ };
class GraphContext {
 public:
  explicit GraphContext(WorkflowDocument);
  GraphSnapshot snapshot() const;
  std::uint64_t replace(WorkflowDocument);
};
class Compiler {
 public:
  Result<SemanticGraphIR> analyze(const GraphSnapshot&, ResourceBindings = {}) const;
  Result<OptimizedGraphIR> optimize(const SemanticGraphIR&) const;
  Result<ExecutionPlan> plan(const OptimizedGraphIR&, const PlanningOptions& = {}) const;
};
```

Context 复制调用方数据。`snapshot()` 返回一致的文档与 revision。`replace()` 在加锁前分配新的不可变文档，再在锁内一起发布文档和下一个 revision。分配失败时源文档和 revision 保持不变；revision 溢出时在发布前抛出异常。Context 析构会把共享 revision token 设为零，使仍存在的 snapshot 失效。

Analysis 校验有界文档和文本、唯一 ID、引用、端口、operation 与必需参数类型、无环拓扑，以及推导出的输出类型/形状/Region 元数据。它会在发布完整 `SemanticGraphIR` 前检查 snapshot 当前性。Optimization 和 planning 同样只返回完整阶段或类型化错误；分配失败可能传播为 `std::bad_alloc`。当前校验细节见 [Graph 生命周期](../../kernel-architecture/Graph-Lifecycle.md) 和 [Compiler 和执行](../../kernel-architecture/Compiler-and-Execution.md)。

## 4. 非目标与明确边界
- `WorkflowDocument` 是 compiler 输入。解析、文件系统、存储和 daemon 错误映射属于调用方。
- 替换源不代表通过验证或执行。
- Snapshot 不会使已析构 context 保持有效。

## 5. 后果与代价
调用方可以原子替换源文档，而不修改既有 snapshot。替换后必须重新编译；若替换与编译并发，调用方需要处理 `Stale`。无效 graph 会消耗验证工作，但不会发布部分 IR 或 plan。
