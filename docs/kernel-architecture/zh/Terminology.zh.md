# Kernel 术语

| 术语 | 含义 |
| --- | --- |
| `WorkflowDocument` | 调用方持有、作为 compiler 输入的 source graph。 |
| `GraphContext` | 持有复制的 workflow document 和当前 revision。 |
| `GraphSnapshot` | 编译使用的一致 source/revision 快照。 |
| `SemanticGraphIR` | Analysis 产生的不可变、已推导 operation graph。 |
| `OptimizedGraphIR` | 独立的语义等价阶段；当前 optimizer 复制 semantic graph。 |
| `ExecutionPlan` | 按依赖排序的本地 operation、放置选择及传播后的 demand。 |
| `ExecutionContext` | 持有有界 worker、冻结的 operation definition、缓存和资源计费。 |
| `ExecutionRun` | 单次同步执行调用的私有状态。 |
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
| digest / cache key | 用于复现检查或可丢弃缓存查找的非安全 identity。 |
| cancellation | 协作式停止请求，阻止较晚的结果成功发布。 |
| CPU fallback | 原生 callback 在发布输出前报告 backend unavailable，且 operation traits 允许时，新发起的 CPU attempt。 |
| canonical image（规范图像） | 算子领域约定：普通图像算子默认 straight alpha，宿主 backing 默认 tiled planar。不要求 Tensor 合规标签或自动 validator；显式 association 转换仍可使用。 |
| boundary payload（边界 payload） | FMT-04 适配器为导出或外部接口生成或消费的 premultiplied 数值 tensor。关联方式由适配器的显式参数说明；tensor 本身不带标签，因此消费者依靠自己的显式参数或绑定的校验器。 |

`SessionId`、`JobId`、daemon IPC 和进程生命周期属于 `photospider-daemon`；Daemon Job 是临时状态。持久 result 与恢复能力在当前产品边界之外。Kernel result 是内存中的值，其 backing 生命周期由 owner 决定。
