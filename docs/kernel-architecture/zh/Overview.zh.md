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
| compiler | validation、typed IR、conservative no-op optimization、local plan、typed digest/key |
| execution | bounded CPU pool、optional GPU callback lane、private Run、cancellation、byte ledger、raw diagnostic |
| data | dense immutable `Value`、rank-general `Region`、`StridedLayout` |
| plugin | exact operation/data-definition C ABI 与 startup-frozen registry |
| benchmark | raw compile/plan/execute observation 与 correctness-oracle result |

CPU execution 是必需能力。Optional GPU support 是 configured local callback lane，只为声明
支持它的 operation 选择。Cross-backend input 是显式 immutable copy；kernel package 不需要
native GPU SDK。

Cancellation 是 cooperative。Completion 与 final result return 前都会检查 plan currentness
和 cancellation。Resource lease 与 intermediate Value 使用普通精确 C++ ownership。

Daemon 依赖 installed public package。Kernel 从不依赖 daemon source，不序列化 internal IR，
也不拥有 daemon namespace 或 Job/result lifecycle。
