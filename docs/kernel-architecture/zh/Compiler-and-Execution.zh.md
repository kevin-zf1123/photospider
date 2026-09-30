# Compiler 与本地执行

## 1. 核心摘要 (TL;DR)

Compiler 校验 workflow，并将其编译为可与独立 bindings 重用的本地 plan。每次执行创建私有 Run，同时共享 context 的有界 CPU worker、可选原生 lane、operation registry、缓存和资源 ledger。只有 callback 输出、cancellation 和 graph currentness 均通过校验后，Run 才发布命名结果。

## 2. 架构心智模型

```text
document + static resources
          |
     GraphSnapshot -> analyze -> SemanticGraphIR
                                   |
                             optimize (copy)
                                   |
                                  plan
                          output Region demands
                                   |
bindings ------------------------> Run
                           /                 \
                  就绪 CPU callback      就绪 GPU callback
                     竞争有界              由一个配置好的 lane
                   CPU worker pool            串行处理
                           \                 /
                      汇合、校验并最终检查停止状态
                            /                 \
                           成功                失败 / stale / cancelled
                            |                           |
                  命名 Value / PlanarImage     丢弃输出并返回 typed status
```

Compiler 在像素数据执行前推导依赖顺序和 metadata。Planning 按 Whole、Elementwise 和裁剪后的 Halo 规则，把每个命名输出请求的 Region 反向传播。Run 调度依赖已就绪的 step，并只传输所需数据；通用 regional source 和 planar image 因此可延迟提供数据。

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

以上是省略其他成员后的 public method 声明。`analyze` 在发布不可变 `SemanticGraphIR` 前检查 snapshot currentness、graph 结构、operation availability、封闭的 typed parameter schema、输入/输出契约和推导出的 descriptor。缺少、未知或类型错误的 parameter 会使 analysis 失败；默认值必须由调用方提供或由 operation contract 表达。`ResourceBindings` 为静态 facet 提供所需的不可变 owner。

Operation definition 可在设置 `requires_metadata_specialization` 后，通过 `specialize_metadata` 依据完整静态输入 metadata 特化模板 traits；也可通过 `prepare_static` 创建不可变的 prepared program。静态 preparation 仅适用于 deterministic、side-effect-free operation。Registry 在锁外调用这些纯 hook，不读取 Value payload。推导后的输出 metadata 和 workspace 上限参与 stage identity。`PreparedOperation` 持有对应 definition/library owner；不同调用不会自动共享 preparation，除非调用方传入匹配 owner。Preparation 和 plan storage 使用的宿主分配不计入每个 observation 的 runtime scratch 准入。Callback 与 owner 生命周期见 [Plugin ABI](Plugin-ABI.zh.md)。

`optimize` 当前将 semantic node 复制到另一个不可变阶段，并计算独立 digest。`plan` 将 operation trait 复制到按依赖排序的本地 step，根据调用方的放置选项记录资源估算，并推导每步输出和输入 demand。Digest 和 cache key 用于 identity 检查，不是安全边界。Plan 不包含 callback pointer、native device handle、daemon object 或 runtime 输入地址。执行要求 plan 与生成它时使用同一个冻结的 operation registry。

`ExecutionBindings` 使用精确名称。普通通用 execution 的输入提供 `Value`、`RegionalSource` 或 `InputSnapshot` 之一；planar declaration 提供 `PlanarImage`。Planar execution 中，非 planar declaration 只接受 metadata 匹配的 `Value`，planar declaration 则要求显式 `PlanarImage`。Execution 在 callback 前校验 binding 名称与 metadata。它在 binding 阶段校验图像 descriptor、facets、storage layout 和 plan tile 几何；依赖数据内容的样本则在读取需求区域时校验。

`ExecutionContext` 持有固定 CPU pool；配置的原生设备可用时，还持有一个 GPU callback worker。两个 lane 都使用确定性的 FIFO 队列，并共享一个非阻塞等待 callback 上限。普通 callback 开始运行后，worker 会释放等待名额，因此运行中的 callback 不占用该上限。Staged CPU job 会持续持有等待名额，直到其 tile callback 退役且 job 离开队列。每次 Run 的 `maximum_parallelism` 限制同时在途的 plan step 数。CPU callback 可并发运行；原生 callback lane 一次运行一个 callback。

Planner 对完整逻辑输入调度 `Whole` operation，并在该边界物化结果。CPU Whole callback 可使用显式授权的 parallel range service。区域化的 `Elementwise` 和 `Halo` 工作按请求 tile 执行；每个 tile callback 是不可拆分的调度任务，不接收该 range service。`ExecutionContext::execute_stream` 按 name/row/column 顺序同步向借用 sink 交付非 planar 的命名 Value 输出 tile。Demand-driven operation 使用独立的 `open_demand`、`execute_fragments` 和 `execute_atoms` 入口；continuation、query 和每轮上限见 [Dependency Data](Dependency-Data.zh.md)。

每个 Run 在 mutex 下确定首个失败。在 scheduler、queue 和 callback 边界，Run 依次检查 cancellation、graph stale、再保留原始失败。Worker 在传输依赖、预留 buffer 或调用 operation 前检查停止状态。若 Run 已停止，worker 退役自己的在途 slot，不再准入新工作。已进入的 callback 可以继续；Executor 等待它们退役。Cancellation 是协作式的，不会抢占任意进程内代码。

Operation callback 接收 plan 计算的输入 demand。Value 被传输或传给 callback 前，Execution 会确认其 Region 覆盖 consumer demand。它还会在 Value 对后继可见前，按 plan 检查生成 Value 的 type、shape 和输出 demand。只有成功时，Run 才把完整命名输出组装为一个 `ExecutionResult`。失败、stale 或取消会丢弃本地已组装输出并返回带类型的 status。

直接调用 registry 时，校验顺序是 operation lookup、端口与 demand 数量、读取 descriptor 前逐个确认输入 Value 有效、demand bounds、parameters、cancellation、backend vocabulary/capability，最后检查静态 descriptor 兼容性。Preserve/Match 冲突在用户代码运行前失败。Registry 在 callback 前推导预期输出 descriptor，并核对 callback 返回值；Run 另行检查 plan 派生的 coverage。需要仿射输入视图的 Whole callback 若 demand 跨越不兼容 owner，会在 callback 前返回 `ViewUnavailable`；只有 traits 允许时才可自动收集输入。输入视图、输出 sink 和 sticky failure 规则见 [Plugin ABI](Plugin-ABI.zh.md)。

原生执行使用 `ExecutionMode::NativeGpu`，仅允许放置 copied traits 声明支持当前配置 backend 的 operation。Plan 包含有类型的 upload、operation 和 host-access action。Run 校验 packed upload 大小并复用兼容的驻留 buffer。访问 CPU 可读的 shared storage 不代表发生复制。原生 allocation 按实际 backing capacity 计入共享资源 ledger；CPU reservation、callback workspace、transfer、cache entry 和 planar backing 受各自资源限制。

只有当 operation traits 允许 fallback，且 GPU callback 在发布任何输出前返回 `BackendUnavailable` 时，可选原生 operation 才会重新尝试 CPU。若已尝试发布输出，BackendUnavailable 即为终止错误。已提交 GPU 工作会在 callback 退役前排空，包括 cancellation 期间。Diagnostics 会分别记录 attempt、dispatch、transfer、cache reuse 和观察到的内存峰值。

当前安装的 operation C ABI 为版本 11，导出 `ps_operation_plugin_get_api_v11`；结构化 planar C 扩展为版本 3。`ps_gpu_service_v11` 同步运行，且只能在 callback 所在线程使用。Service pointer 与 token 在 callback 返回时失效；已提交 dispatch 会在返回前排空，service error 会保持 sticky。ABI 校验受信任进程内模块的表布局与能力，不隔离 native code。MSL dispatch 使用 Metal，SPIR-V dispatch 使用 Vulkan；backend/format 不匹配会返回 `BackendUnavailable`。每个 operation 自行声明数值行为。

返回的 Value 和 PlanarImage 在 Run 或 context 结束后仍通过 storage owner 和资源租约保持有效。临时 Value 在最后一个 reader 完成后释放。最后一个返回结果 owner 释放后，保留容量归还。Run 会保留调用方输入 payload，但不会将其视为新分配的 execution buffer；受控输出、scratch、transfer 和 intermediate 会计入 context ledger。

## 4. 明确边界

- Kernel 不拥有 daemon session、IPC、持久 Job、进程隔离或结果恢复。
- 内部 IR、plan 和阶段 digest 不是序列化格式或安全证明。
- Native operation module 以受信任代码形式在宿主进程执行。
- GPU mode 不保证每个 operation 都有原生实现，也不保证图像输出在节点间始终驻留设备。
- Diagnostics 记录观察到的工作和资源使用，不认证数值正确性或发布就绪状态。

## 5. 后果与代价

Graph 结构、参数、binding 或 operation capability 无效时，受影响的 callback 会在启动前失败。并发竞争可能触发 queue 或资源限制；调用方需处理带类型的准入失败，并释放不再需要的结果。慢速或不响应停止请求的 callback 会在 cancellation 后继续延迟 Run，因为 Executor 必须等待其所有权退役。保留结果会继续持有 storage lease，并减少后续 Run 可用容量。ABI 改变后，consumer 需要针对匹配的已安装 package 重新构建。
