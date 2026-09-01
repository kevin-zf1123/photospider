# Compute 流程

1. Caller 创建 `WorkflowDocument` 与独立 `GraphContext`。
2. Coherent `GraphSnapshot` 捕获 source/revision。
3. `Compiler::analyze` 验证并产生 typed `SemanticGraphIR`。
4. `Compiler::optimize` 产生独立的 conservative `OptimizedGraphIR`。
5. `Compiler::plan` 产生 dependency-ordered local `ExecutionPlan` step。
6. `ExecutionContext::execute` 验证 exact frozen operation-registry identity，并创建一个
   private `ExecutionRun`。
7. Dependency-ready step 在 per-Run parallelism 下进入 bounded CPU queue 或 optional local
   GPU callback queue。
8. Cross-backend input 被复制成不同的 validated Value；Run 记录 backend label 与 transfer
   observation。
9. Operation callback 返回一个 Value。Run 检查 type/shape、cancellation、plan
   currentness 与 dependency identity。
10. 完整 named output 成为一个 in-memory `ExecutionResult`；全部 byte lease 与 temporary
    Value 通过精确 ownership 退役。

Cancellation 是 cooperative，不是 preemption。Running callback 可以晚返回，但其 Value
不能在 cancellation 或 graph replacement 后发布。Exception 被隔离成 typed failure，不会
停止无关 context。

GPU selection 表示 optional in-process callback lane，不是 hardware SDK 或 remote device。
只有 operation trait 允许时，GPU attempt 才 fallback 到 CPU；两个 attempt 都保留在 raw
diagnostic 中。
