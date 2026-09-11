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
| execution | bounded CPU pool、optional native Metal queue/lane、private Run、cancellation、byte ledger、raw diagnostic |
| data | regional immutable `Value` 与 CPU 可访问/原生存储、rank-general `Region`、`StridedLayout` |
| plugin | exact operation ABI v9/data-definition ABI v1、typed parameter schema、demand-aware callback 与 startup-frozen registry |
| benchmark | raw compile/plan/execute observation，加 named correctness oracle 或显式 unchecked 状态；execution cancellation 会中止完整 run 且不发布 report |

CPU exact 为必需默认。显式 MetalFp32 在可选 Apple Silicon 设备选择声明支持的原生
实现。Shared buffer 保留完成结果并避免重复上传，主机访问与实际复制分别报告。
Metal/Foundation 是可关闭的 Apple 私有构建依赖，参见 [S4 工作流](S4-Workflow.zh.md)。

Cancellation 是 cooperative。Completion 与 final result return 前都会检查 plan currentness
和 cancellation。Resource lease 与 intermediate Value 使用普通精确 C++ ownership。

Planning 把 optional named output Region 按 Whole、Elementwise 和 overflow-safe clipped Halo
规则反向传播。所得 per-step output/input demand 进入 plan identity，并在 operation callback
收到 demand 前由 execution 再次验证。

Daemon 依赖 installed public package。Kernel 从不依赖 daemon source，不序列化 internal IR，
也不拥有 daemon namespace 或 Job/result lifecycle。

0.7 foundations 已实现 typed image-v2、共享静态 dtype/shape/output 推断、computed
bounded scalar 及 numeric/channel/color/expression/LUT/component 算子。
[独立 foundations workflow](Foundations-Workflow.zh.md)展示公开组合入口。
该能力交付线为 ops；main 已审计基线仍为 0.6，daemon 的 0.6 consumer 尚未迁移。
