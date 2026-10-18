# 架构概览

## 1. 核心摘要 (TL;DR)

Photospider 是可嵌入的 C++17 内核，用于校验本地图工作流，经由相互独立的 IR 与 plan 阶段把它们编译为固定的 tile 指令程序，并使用调用方提供的 Result bindings 执行该程序。`GraphContext` 持有源文档及其修订状态；`ExecutionContext` 持有有界线程、算子定义、可选缓存保留和资源计费。Daemon 使用已安装的内核 API，并独立管理自己的会话和进程生命周期。

## 2. 架构心智模型

```text
WorkflowDocument -> GraphContext -> GraphSnapshot      IRProgram (other frontend)
                                    |                         |
                                    v                         v
                         analyze -----------------> ValidatedIR <---- .psir
                                                          |
                                                          v
                                     optional optimize + validate -> ValidatedIR <---- .psir
                                                          |
                                                          v
                                   plan: static mapping, tile expansion,
                                   fixed grants, CFG of instruction blocks
                                                          |
                                                          v
                                                    ExecutionPlan <---- .psxp
                                                          |
ExecutionBindings --------------------------------------> Run
                                                          |
                                     sequential VM: pc -> block -> instructions
                                                          |
                                  +-----------------------+---------------------+
                                  |                                             |
                     CPU callback on the context pool            native callback on the
                     (range or tile service inside it)           configured GPU lane
                                  +-----------------------+---------------------+
                                                          |
                                         named Result outputs + diagnostics
```

Compiler 负责静态校验、依赖映射与规划。每个 Run 持有其 program counter、block 值、已绑定授权、Result producers、取消状态、dependency recorder 和诊断；它同一时刻只执行一条 plan 指令。并行来自共享 context pool 的不同 Run，以及单个算子回调内部的 CPU range、CPU tile 和 GPU 工作。返回的 Result 独立持有 backing 与 leases。

Planner 直接接受初始 ValidatedIR，optimizer 路径可跳过。

## 3. 契约与接口

```cpp
class Compiler {
 public:
  Result<ValidatedIR> analyze(const GraphSnapshot&,
                                  ResourceSet = {}) const;
  Result<ValidatedIR> bind_ir(IRProgram, ResourceSet = {}) const;
  Result<ValidatedIR> optimize(const ValidatedIR&,
                               const OptimizerInvocation& = {"optimize", {}}) const;
  Result<ExecutionPlan> plan(const ValidatedIR&,
                             const PlanningOptions& = {}) const;
};

class CompilerArtifact {
 public:
  static Status save_plan(const ExecutionPlan&, const std::string& path);
  static Result<ExecutionPlan> load_plan(std::shared_ptr<OperationRegistry>,
                                         const std::string& path,
                                         ResourceSet = {});
};

class ExecutionContext {
 public:
  Result<ExecutionResult> execute(
      const ExecutionPlan&, ExecutionBindings = {},
      const CancellationToken& = CancellationToken(),
      const ExecutionOptions& = {});
};
```

以上是省略其他成员后的 public 签名。每个阶段返回完整不可变值或带类型的失败。`analyze` 从具名输出出发，把 DAG 展开为 `DependencyPaths`，在每条引用路径中保留共享祖先，并使用相同的 Call 和 SSA ID。`optimize` 保留每个 call 的首次请求，移除空 chain，把剩余 chains 连接为可执行的 `ControlFlow`。例如，原始 `ABG / AB / AC / DE / F` 转换为 `ABG / C / DE / F`。`bind_ir` 也接受显式 ControlFlow，包括循环和基于单个 `UInt8` sample 的条件分支；优化保持这种形式不变。

`plan` 调用每个算子的静态 mapper，把可达的输出 tile 展开为带固定读取授权的去重指令，并验证结果。Tile 去重记录已安排到 plan 中的工作，与运行时缓存独立。Execution 在 callback 前检查 plan currentness 与冻结的 operation registry identity，然后遵循指令和 CFG 执行。每个 call 产出的输出都在下一个 call 开始前完成认证。`analyze` 接收不可变的内容寻址 `ResourceSet`；每个算子的 preparation 选择它需要的 identity，例如由显式颜色参数命名的 ICC profile。Result 与 Tensor 只携带结构和数据；领域解释是显式算子参数，内核不在连线上做标签匹配。

`CompilerArtifact` 保存和加载 v4 文本（`.psir` 是统一 IR，`.psxp` 是完整 Plan）。原始和优化 IR 都允许依赖路径或 CFG；Plan 保存私有 lowering 的 CFG。v1–v3 拒绝并要求重新导出。加载对象独立于原 GraphContext，仍需匹配的冻结 registry 和资源。Plan 导入不调用 optimizer、mapper 或 planner。见[Compiler 与本地执行](Compiler-and-Execution.zh.md#compiler-产物)。

`CpuExact` 是默认执行模式。`NativeGpu` 仅允许把声明了原生实现且匹配当前配置后端的算子放到 GPU。CPU 工作使用固定 context pool；原生回调使用配置的 backend lane。Cancellation 是协作式的，已经进入回调的任务可能需要完成后 Run 才能返回。

## 4. 明确边界

- Kernel 不拥有 daemon session、IPC、持久 Job 或进程生命周期。
- 内核只附带 WorkflowDocument DAG 前端。不存在 Python 或文本语言前端、机器码 backend、JIT，也不会根据产物自动查找算子库。
- Optimizer 消除重复的依赖路径请求，同时保留每个唯一 call；显式 ControlFlow 输入保持不变。Tensor shape 和 tile grid 在编译期固定；控制流不提供动态 shape。
- Compiler 产物是可编辑、带版本、无签名的文本文件；其 digest 是可复现辅助。
- Native operation module 是受信任的进程内代码；ABI 校验不构成沙箱。
- GPU 可用不代表所有算子都有原生实现。
- Result 是内存中的 owner，不是持久身份或恢复记录。

## 5. 后果与代价

完整依赖路径展开的规模可能随 DAG 共享结构呈指数增长。前端把原始 IR 限制为最多 1,048,576 次 call 引用和 65,536 条 chain，超过任一上限就返回 `ResourceExhausted`。较少的可执行 blocks 会减少 block 跳转，但共享 stage 状态持续到下一次 block activation；仅合并 blocks 不保证降低内存用量。

Plan 可与新 bindings 重用，也可以保存和重新加载，但不包含运行时输入地址。规划代价和 plan 规模随可达输出 tile 数增长；`PlanningOptions` 限制指令数和依赖边数，极大的逻辑输出应以稀疏请求规划，而不是事后过滤。配置的上限耗尽时，队列准入和资源限制会拒绝 Run；调用方应以返回状态作为权威结果。保留 Result 会继续持有存储租约，因此调用方决定结果内存继续计费多久。Native placement、编译期回退、数据传输和可选的 completed-result retention 会改变 Run 的计算量与内存成本。Block-state cache-hit 数量属于实现观察，不构成性能保证。
