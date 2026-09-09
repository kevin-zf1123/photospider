# Compute 流程

1. Caller 创建 `WorkflowDocument` 与独立 `GraphContext`。
2. Coherent `GraphSnapshot` 捕获 source/revision。
3. `Compiler::analyze` 验证并产生 typed `SemanticGraphIR`。
4. `Compiler::optimize` 产生独立的 conservative `OptimizedGraphIR`。
5. `Compiler::plan` 产生 dependency-ordered local `ExecutionPlan` step。
6. `ExecutionContext::execute` 验证 exact frozen operation-registry identity，并创建一个
   private `ExecutionRun`。
7. Dependency-ready step 在 per-Run parallelism 与 single context-wide waiting admission
   下进入 bounded CPU queue 或 optional local GPU callback queue。
8. 显式原生上传把需求 Region 打包到 shared buffer；有效驻留副本避免重复上传，GPU
   后继复用 buffer，完成后的主机访问不虚构 D2H 复制。
9. Scheduler/admission failure 成为 first failure 前，以及 operation callback complete
   时，Run 都按 cancellation、graph staleness、original failure 的顺序选择结果；随后在
   Value publication 前检查 type/shape 与 dependency identity。
10. 完整 named output 成为一个 in-memory `ExecutionResult`；temporary Value 在最后 reader 后释放；返回 Value
    持有分配租约，直到最后一个所有者释放。

Cancellation 是 cooperative，不是 preemption。Running callback 可以晚返回，但其 Value
不能在 cancellation 或 graph replacement 后发布。Exception 被隔离成 typed failure，不会
停止无关 context。
First-failure selector 不分配内存，并在 failure diagnostic construction 本身失败时继续复用，
因此已经观察到的 stop 不会被降级为 queue、admission 或 backend failure。

GPU selection 在显式 MetalFp32 下选择原生 Metal。单 GPU lane 包含取消在内均在
退役前排空提交。Trait 允许发布前 CPU 回退；已提交设备执行错误终止 Run。分别报告
尝试、实际 dispatch/复制和缓存复用。
