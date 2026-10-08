# 架构概览

## 1. 核心摘要 (TL;DR)

Photospider 是可嵌入的 C++17 内核，用于校验本地图工作流、编译不可变计划，并使用调用方提供的 Result bindings 执行。`GraphContext` 持有源文档及其修订状态；`ExecutionContext` 持有有界线程、算子定义、可选缓存保留和资源计费。Daemon 使用已安装的内核 API，并独立管理自己的会话和进程生命周期。

## 2. 架构心智模型

```text
WorkflowDocument -> GraphContext -> GraphSnapshot
                                    |
                                    v
                         analyze -> SemanticGraphIR
                                    |
                                    v
                         optimize -> OptimizedGraphIR
                                    |
                                    v
                                  Plan
                                   |
ExecutionBindings ----------------> Run
                                   |
                        +----------+----------+
                        |                     |
                    就绪 CPU step         就绪原生 step
                    竞争有界 CPU pool       进入一个 GPU lane
                        +----------+----------+
                                   |
                    命名 Result outputs
                           + diagnostics
```

Compiler 负责静态校验与规划。每次 execution 持有就绪任务、Result producers、取消状态、dependency recorder 和诊断。多个 Run 共享 context 的 worker pool 与 resource root；返回的 Result 独立持有 backing 与 leases。

## 3. 契约与接口

```cpp
class Compiler {
 public:
  Result<SemanticGraphIR> analyze(const GraphSnapshot&,
                                  ResourceBindings = {}) const;
  Result<OptimizedGraphIR> optimize(const SemanticGraphIR&) const;
  Result<ExecutionPlan> plan(const OptimizedGraphIR&,
                             const PlanningOptions& = {}) const;
};

class ExecutionContext {
 public:
  Result<ExecutionResult> execute(
      const ExecutionPlan&, ExecutionBindings = {},
      const CancellationToken& = CancellationToken(),
      const ExecutionOptions& = {});
};
```

以上是省略其他成员后的 public method 签名。每个阶段返回完整不可变值或带类型的失败。Optimizer 将 semantic node 复制到独立阶段并计算独立 digest。Planning 保留静态 metadata，以完整校验 schema 和 identity，再根据选定 output 的 input projection 推导 executable demand。Execution 在 callback 前检查 plan currentness 与冻结的 operation registry identity，然后校验 Result schema、请求 coverage、dependency evidence 和 publication finality。`analyze` 可接收不可变的 `ResourceBindings`，绑定静态 facet 所需的资源，例如色彩配置文件。

`CpuExact` 是默认执行模式。`NativeGpu` 仅允许把声明了原生实现且匹配当前配置后端的算子放到 GPU。CPU 工作使用固定 context pool；原生回调使用配置的 backend lane。Cancellation 是协作式的，已经进入回调的任务可能需要完成后 Run 才能返回。

## 4. 明确边界

- Kernel 不拥有 daemon session、IPC、持久 Job 或进程生命周期。
- Compiler IR 和 plan 是进程内值，不是可序列化的 workflow 格式。
- Native operation module 是受信任的进程内代码；ABI 校验不构成沙箱。
- GPU 可用不代表所有算子都有原生实现。
- Result 是内存中的 owner，不是持久身份或恢复记录。

## 5. 后果与代价

Plan 可与新 bindings 重用，但不包含运行时输入地址。配置的上限耗尽时，队列准入和资源限制会拒绝 Run；调用方应以返回状态作为权威结果。保留 Result 会继续持有存储租约，因此调用方决定结果内存继续计费多久。Native placement、fallback、数据传输和可选的 completed-result retention 会改变 Run 的计算量与内存成本。Block-state cache-hit 数量属于实现观察，不构成性能保证。
