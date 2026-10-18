# Kernel 术语

| 术语 | 含义 |
| --- | --- |
| `WorkflowDocument` | 调用方持有、作为 compiler 输入的 source graph。 |
| `GraphContext` | 持有复制的 workflow document 和当前 revision。 |
| `GraphSnapshot` | 编译使用的一致 source/revision 快照。 |
| `IRProgram` | 可编辑、与前端无关的 SSA 程序：输入声明与绑定、calls、blocks、entry block 和有序 return 签名。由 `Compiler::bind_ir` 接受。 |
| `SemanticGraphIR` | 由 `analyze`、`bind_ir` 或 `.psir` 导入产生的不可变、已验证并已准备的 SSA IR。 |
| `OptimizedGraphIR` | Optimizer 之后具有独立 identity 的阶段；当前 optimizer 为 no-op，复制每个 call、value 和 block。 |
| SSA value | 以 `IRValueId` 命名、恰好由一个 call 输出、block 参数或外部输入绑定定义一次的 Result。每次使用都必须被其定义支配。 |
| basic block / CFG | `IRBlock`：以一个 terminator（`IRJump`、`IRConditionalJump` 或 `IRReturn`）结尾的有序 call 列表。Blocks 与边构成控制流图；DAG 前端为每个 call 生成一个 block，并以 jump 连接。 |
| block 参数 | 边进入 block 时赋值的带类型值；同一条边的全部参数先被捕获，再修改任何参数。循环通过 block 参数携带值。 |
| conditional jump | 基于恰好含一个 `UInt8` sample 的 Result 的分支：零选择 `if_false`，其他值选择 `if_true`；只运行所选后继。 |
| `ExecutionPlan` | 已验证的本地程序：`steps()` 声明每个 call 输出及其后端与固定网格，`blocks()` 保存执行所遵循的 tile 指令与 terminator。 |
| plan step | `PlanStep`：一个 call 输出的声明。Step 可以没有指令；step 数量不是执行次数。 |
| tile 指令 | `PlanTileInstruction`：一个输出 tile 或共享 stage 目标，带有 publication 目标、固定输入与共享授权、block 内依赖和编译期 CPU 回退标志。 |
| 静态 mapping | `OperationDefinition::map_dependencies`（C：`map_dependencies` hook）：纯编译期回调，返回一个目标可以读取的 tile。默认的 `complete_input_dependencies` 授权每个 projection 输入的全部 tile。 |
| 固定授权 | 保存在指令中的 `PlanTensorRead` 或 `PlanSharedRead`；该指令在运行时唯一可以读取的数据。Descriptor-only 授权暴露 schema 事实，不暴露 payload。 |
| synthetic return 记录 | 具名 return 的依赖记录，索引为 `steps().size()` 加 return 序号，由 `ExecutionPlan::output(name).record_index` 返回。 |
| compiler 产物 | 由 `CompilerArtifact` 写入和读取的可编辑、带版本文本文件：`.psir` 保存语义或优化后 IR，`.psxp` 保存执行 plan。格式版本 2。 |
| 产物 manifest | 产物中列出的 operation key 及其 contract digest 和资源 identity，可通过 `CompilerArtifact::inspect` 在没有 registry 的情况下读取。 |
| `ExecutionContext` | 持有有界 worker、冻结的 operation definition、缓存和资源计费。 |
| `ExecutionRun` | 单次同步执行调用的私有状态：program counter、block 值、已绑定授权、证据和诊断。 |
| 静态 VM | Run 内部的解释器。它按保存的顺序一次执行一条 plan 指令并遵循 CFG；从不映射依赖、创建 producer task 或重排指令。 |
| `ExecutionBindings` | 每次 Run 按精确名称提供的输入 owner，每个都是按 declaration schema 校验的 owning `ResultRef`。 |
| `ResultRef` | 拥有静态 Tensor-only Result schema、捕获覆盖、backing owner 和 ObjectId 来源历史。 |
| `Tensor` | Descriptor、精确授权 Footprint 和拥有型 TensorSpan 片段组成的唯一数值载体。 |
| operation metadata | `OperationMetadata`：算子边界上的完整 Result schema。Specialization 根据输入结构和静态参数推导输出结构。 |
| `metadata_owner` | 为 schema 的 managed 嵌套容器付费的资源租约；属于预算所有权，不是解释。 |
| `ImmutableResource` / `ResourceSet` | 内容寻址的不可变字节（长度与 SHA-256）及其不可变集合。算子在 preparation 时按 identity 选择资源，plan 保留它们。 |
| validator（校验器） | 已注册的 `ValidationDefinition`，通过 key、version 和 implementation 显式绑定到端口，检查宿主授权的样本。 |
| diagnostic report（诊断报告） | 注册为 `DiagnosticDefinition`、由 `DiagnosticAccumulator` 合并的固定大小不透明记录类型。 |
| `DomainErrorCode` | 不透明的 64 位失败码，内核把它保存在 `FailureDetail::domain_code` 中且不解释。 |
| operations SDK（算子 SDK） | `Photospider::ops_sdk`，头文件位于 `include/photospider/ops/`：颜色描述、领域 codec 与算术、描述编辑、LUT bake 契约、质量与数值报告、Layer 类型。不属于内核。 |
| `Region` | descriptor 轴坐标中的逻辑子集，不表示字节范围。 |
| `TensorSpan` | Tensor 内一个拥有型矩形映射，记录实际有符号 stride 和 backing owner。 |
| operation traits | 冻结的输入/输出契约、资源上限和 backend 能力声明。 |
| operation registry | 编译与执行 plan 所用的受信任 operation definition 集合，注册后冻结。 |
| digest / cache key | 用于复现检查或可丢弃缓存查找的非安全 identity。Compiler digest 包含程序标识符；内容缓存 key 不包含。 |
| cancellation | 协作式停止请求，阻止较晚的结果成功发布。 |
| CPU fallback | GPU callback 在发布输出或读取输入之前报告 backend unavailable 后，以相同授权新发起的 CPU attempt；仅在 planner 设置了指令的 `cpu_fallback` 标志时允许。 |
| canonical image（规范图像） | 算子领域约定：普通图像算子默认 straight alpha，宿主 backing 默认 tiled planar。不要求 Tensor 合规标签或自动 validator；显式 association 转换仍可使用。 |
| boundary payload（边界 payload） | FMT-04 适配器为导出或外部接口生成或消费的 premultiplied 数值 tensor。关联方式由适配器的显式参数说明；tensor 本身不带标签，因此消费者依靠自己的显式参数或绑定的校验器。 |

`SessionId`、`JobId`、daemon IPC 和进程生命周期属于 `photospider-daemon`；Daemon Job 是临时状态。持久 result 与恢复能力在当前产品边界之外。Kernel result 是内存中的值，其 backing 生命周期由 owner 决定。
