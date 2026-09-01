# 架构概览

Photospider 是用于 local graph 的 C++17 可嵌入 compiler/execution kernel。Caller 拥有独立
`GraphContext`，并选择独立或共享 `ExecutionContext`。

## Pipeline

```text
WorkflowDocument -> GraphContext/GraphSnapshot
  -> Compiler::analyze -> SemanticGraphIR
  -> Compiler::optimize -> OptimizedGraphIR
  -> Compiler::plan -> ExecutionPlan
  -> ExecutionContext::execute -> ExecutionResult
```

每个 compiler stage 返回完整 immutable value 或一个 failure。Source、semantic、optimized、
plan、runtime Value 与 daemon identity 相互分离。

## Module ownership

| Module | 当前 ownership |
| --- | --- |
| graph | `GraphContext`、`GraphSnapshot`、copied source revision/currentness |
| compiler | fail-closed parameter validation、typed IR、conservative no-op optimization、demand-aware local plan、typed digest/key |
| execution | bounded CPU pool、optional GPU callback lane、private Run、cancellation、byte ledger、raw diagnostic |
| data | dense immutable `Value`、rank-general `Region`、`StridedLayout` |
| plugin | exact operation ABI v2/data-definition ABI v1、typed parameter schema、demand-aware callback 与 startup-frozen registry |
| benchmark | raw compile/plan/execute observation，加 named correctness oracle 或显式 unchecked 状态 |

CPU execution 是必需能力。Optional GPU support 是 configured local callback lane，只为声明
支持它的 operation 选择。Cross-backend input 是显式 immutable copy；kernel package 不需要
native GPU SDK。

Cancellation 是 cooperative。Completion 与 final result return 前都会检查 plan currentness
和 cancellation。Resource lease 与 intermediate Value 使用普通精确 C++ ownership。

Planning 把 optional named output Region 按 Whole、Elementwise 和 overflow-safe clipped Halo
规则反向传播。所得 per-step output/input demand 进入 plan identity，并在 operation callback
收到 demand 前由 execution 再次验证。

Daemon 依赖 installed public package。Kernel 从不依赖 daemon source，不序列化 internal IR，
也不拥有 daemon namespace 或 Job/result lifecycle。
