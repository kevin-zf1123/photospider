# ADR 0015：让编译与执行归内核所有

- 状态：Accepted

## 1. 核心摘要 (TL;DR)

Photospider 将嵌入式图工作流编译与执行和本地进程编排分开。内核拥有类型化工作流数据、规划、执行、资源、取消和结果；daemon 拥有本地 session、job 和 IPC。这个边界让内核保持可嵌入，并让 daemon 消费公开 kernel 与 ops target。

## 2. 架构心智模型 (Mental Model & Intuition)

```text
应用 -> WorkflowDocument -> Compiler -> ExecutionPlan
                                   |             |
                                   v             v
                              内核注册表     ExecutionContext
                                                | CPU / 可选 GPU
                                                v
                                             命名结果

客户端 -> 本地 IPC -> daemon Session / Job -> 公开内核 API
                           |                    (版本化 kernel artifact 文本)
                           +-> job 生命周期和临时结果生命周期
```

编译器将调用方拥有的文档转换为类型化语义阶段与优化阶段，再生成物理计划。`ExecutionContext` 使用有界的本地 worker 和资源运行计划。daemon 通过公开内核 facade 提交工作，并拥有临时 job 记录与结果的生命周期。

## 3. 契约规约与接口 (Formal Contracts & APIs)

```cpp
class Compiler final {
 public:
  explicit Compiler(std::shared_ptr<OperationRegistry> operations,
                    std::shared_ptr<const OptimizerRegistry> optimizers = {},
                    std::shared_ptr<const PlannerRegistry> planners = {},
                    std::shared_ptr<const FrontendRegistry> frontends = {});
  Result<ValidatedIR> analyze(const GraphSnapshot& snapshot,
                             ResourceSet resources = {},
                             const std::string& frontend = "dag") const;
  Result<ValidatedIR> optimize(
      const ValidatedIR& ir,
      const OptimizerInvocation& invocation = {"optimize", {}}) const;
  Result<ExecutionPlan> plan(const ValidatedIR& ir,
                             const PlanningOptions& options = {}) const;
};

class PHOTOSPIDER_API ExecutionContext final {
 public:
  Result<ExecutionResult> execute(
      const ExecutionPlan& plan, ExecutionBindings bindings = {},
      const CancellationToken& cancellation = CancellationToken(),
      const ExecutionOptions& options = {});
};
```

这些代码还原公开 compiler 和 execution 方法的签名；省略了头文件与无关声明。`Compiler::analyze` 接收 graph snapshot 和可选的 `ResourceSet`，后者由按内容寻址的 `ImmutableResource` 句柄组成。Operation preparation 通过显式 `ResourceIdentity` 从该集合中选取资源；prepared operation、plan 和执行状态持有所选 owner，Result 只持有它实际保存的存储。`ExecutionPlan` 是公开 class。

内核拥有 `WorkflowDocument` 的解释、图验证、语义与优化 IR、算子 traits、规划、`Tensor` 和 `Region` 契约、本地执行、资源计账、取消、后端选择、fallback 与结果发布。`GraphContext` 和 `ExecutionContext` 是独立对象，既不是 daemon session，也不是全局注册表条目。阶段 digest 和 plan/cache key 是用于复现与可丢弃派生数据的非安全身份。

daemon 拥有本地 IPC，以及围绕公开内核软件包的临时进程编排。内核通过公开 API 拥有编译器 IR、执行资源、值和执行结果。本 ADR 规定组件边界；daemon session、job、取消和清理的实现细节由 daemon 仓库维护。

算子和数据 provider 通过公开 C 接口加载到宿主进程。配置好的 operation registry 在执行期间保持冻结。ABI 校验、边界检查、异常隔离和清理用于保障正确性，不提供不可信代码隔离。

内核保留防御性正确性检查，包括 ABI 版本和结构大小、对齐、指针/count 与数组边界、整数和分配溢出、图和 plan 有效性、过期句柄/完成、取消时的结果发布检查、异常隔离及精确资源清理。仅当算子契约允许时，可选 GPU 失败才会回退 CPU。这些检查不提供进程隔离。

维护中的 benchmark 报告原始 compile/plan/execute/operation 时间、所选 backend、传输次数和字节数、活动字节峰值、fallback/错误原因、plan/result identity 及正确性观测。提供 oracle 时，每个样本记录有界且规范的 `oracle_name`；未提供 oracle 的运行标记为 `unchecked`。benchmark 不创建证据权威、持久制品身份或发布判定。

## 4. 负面清单与边界 (Non-Goals & Explicit Boundaries)

- 内核是单机可嵌入库，不定义 daemon job 身份、全局队列、服务状态、自动重试或持久化。
- daemon 是本地编排进程。session 是进程内逻辑命名空间，不是租户或隔离边界。
- 两个组件都不提供网络服务、认证、授权、多租户配额、远程执行或分布式设备。
- 持久化 job、checkpoint、恢复日志、持久结果、制品权威身份、回执、发布证据和运行 SLO 判定不属于当前产品。daemon 的临时 job 仍属于本地编排。
- operation 和 provider DSO 在宿主进程权限下运行。系统不提供沙箱、进程隔离、密码学准入或策略插件产品。
- Native IR/Plan 对象布局不作为 wire format。`CompilerArtifact` 拥有显式版本化的 `.psir`、`.psxp` 文本，依据冻结注册表和资源校验导入，不承诺跨版本兼容。Daemon IPC 可以传递公开 artifact 文本，序列化实现仍由 kernel 拥有。

## 5. 后果与代价 (Consequences)

内核调用方管理自己的 graph 和 execution context、输入分配、并发调用及结果生命周期。受控执行缓冲区受 context 配置限制；调用方拥有的输入和未计账的进程开销不受 RSS 上限约束。普通执行以协作方式观察取消，并拒绝从过期 graph revision 发布结果。

daemon 调用方通过公开 package target 与 API 集成。本内核仓库不能证明与某个 daemon 修订版兼容，也不规定 daemon 的清理和重启行为。

消费者匹配公开 package 与 ABI 版本。Kernel 开发期间，daemon 可消费 kernel 自身的公开 build-tree export，安装消费验证保持暂停。公开契约变化时，下游需要重建。插件校验可以拒绝不匹配或格式错误的接口表，但不能保证已接受的进程内 DSO 安全。
