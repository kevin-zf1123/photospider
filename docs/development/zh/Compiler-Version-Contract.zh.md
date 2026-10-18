# 编译器与包版本契约

## 当前契约

Package 0.38.0 使用 SameMinorVersion 兼容规则。C++ consumer 和外部 C operation module 必须针对匹配头文件重建。Result ABI 7 要求 table/record 大小精确匹配，不提供兼容 shim；为 ABI 1 至 6 构建的 module 被拒绝。ADR 0022 的前缀协商冻结 ABI 仍为 Proposed。

| 契约 | 版本 |
| --- | --- |
| Package | 0.38.0 |
| WorkflowDocument | 7 |
| OperationTraits | 29 |
| Result operation C ABI | 7, exact-size |
| CPU parallel / tiles service ABI | 2 / 2 |
| Native GPU service ABI | 1 |
| `.psir` / `.psxp` 文本格式 | 4 |
| Result schema encoding | `photospider.result-schema.v6` |
| Validated IR | `validated-ssa-ir-v26` |
| Example optimizer | `optimize@1` (provenance only) |
| Physical plan | `static-tile-program-v27` |
| Plan cache key | `plan-cache-key-v20` |
| 已保存算子契约 | `artifact-static-operation-contract-v2` |
| Step 依赖 template | `photospider.dependency-static.v4` |
| 固定授权 / block 输入证明 | `photospider.fixed-grant-cache.v2` / `photospider.fixed-block-inputs.v2` |
| Ops TensorDescription codecs | TDM4/TDM5 |

这些版本轴相互独立。Package `0.38.0` 使用 SOVERSION `0.38`。`SchemaTemplate` 定义结构成员和 scopes；`ResultTensorSpec` 定义 shape、dtype、batch/atomic 轴及可选 spatial layout。`ResultViewAxis` 使用锚点仿射坐标与有符号步长。`TileGrid::from_schema` 返回 `Result<TileGrid>`；`OperationRegistry::prepare_operation` 准备静态契约。`ExecutionContext` 以 `ResourceBudget` 准入 Root，snapshot reservation 使用独立的有界预算。外部 C DSO 导出 `ps_result_operation_plugin_get_api_v7`，必须提供 ABI 7 精确记录大小、作为每条 operation 记录最后一个成员的可选静态 `map_dependencies` hook、非 NULL 显式 Tensor-view transform 与支持 1..16 片的 C 事务服务。同时导出任一 `_v1` 至 `_v6` getter 的 module 以 `TypeMismatch` 失败；导入时拒绝版本或记录大小不匹配。仅重命名 getter 或修改 `abi_version` 不能使 ABI 6 module 生效，因为 operation 记录布局和运行期 Need 的含义都已改变。ADR 0022 是独立的 Proposed、未实施契约。当前权威定义来自公共头文件、CMake package 声明和 compiler 规范编码。

## Compiler 阶段与产物

前端/`bind_ir`、可选命名 optimizer 和 artifact 导入都发布同一种不可变 `ValidatedIR`。Planner 直接接受合法 DependencyPaths 或 ControlFlow，对依赖路径在私有副本中按 Call ID 去重，不调用 optimizer。CFG 循环保留动态激活语义。默认编译不调用 optimizer。有序调用包含重复名称，每次产生编号版本；内容 identity 不包含生产历史，因此 no-op 可保持 digest。

Artifact v4 的 `.psir` 使用 `stage ir` 和显式程序形式；`.psxp` 保存 lowering 后的 CFG、完整物理指令，以实际输入 IR identity 为 parent。原始及变换后的 IR 都可导入并直接规划。版本 1–3 拒绝并提示重新导出。Plan 导入链接和校验保存代码，不优化、不调用 mapper、不重新规划；registry/资源匹配和当前性约束不变。算子 manifest digest 描述契约，不认证代码。见[工具](Compiler-Tools.zh.md)、[格式](Compiler-Tool-Formats.zh.md)和[编译器架构](../../kernel-architecture/zh/Compiler-and-Execution.zh.md)。

这是破坏性的 C++ API/artifact 更新：删除 `SemanticGraphIR`、`OptimizedGraphIR`、分阶段 digest 和 `ImportedIR` variant，不保留别名。`CompiledWorkflow` 改为有序 IR 版本及可选 Plan；`ExecutionPlan::input_ir_digest()` 表示真实父输入。消费者需要重建，artifact 需要重新导出。Document schema 7、OperationTraits 29、C operation ABI 7 保持不变。

## Workflow 输入与算子执行

Workflow 声明要求静态 Result schema，binding 按精确名称提供拥有型 ResultRef。Run 固定 plan、binding snapshot 和 generation。每个输出/成员在 plan-wide tile extent 下拥有网格，默认 128。静态 mapper 在规划期返回使用成员 slot 和紧凑区间的读取；descriptor-only 读取必须显式提出，空 payload 集合不执行工作。Plan 把这些读取保存为固定授权。

Continuation 可以跨多次 poll，但 poll 时的 Need 只声明已编译授权。共享 stage 读取需要显式静态 mapping 条目。通过这些读取或 mandatory root 到达的 stage，在每个 call、stage 和 block activation 内共享一个 producer，跨多个输出也是如此。普通 tile 指令发布一个完整成员 tile。固定 bundle/prefix 可见性被编译进读取闭包。每个 call 产出的输出在其指令完成后、下一个 call 开始前完成认证。Mandatory root 和取消遵循执行契约。

## Result 与图像接口

Result 包含 1 到 16 个具名静态 Tensor 成员。Dtype、正 shape、rank、batch/atomic 轴和拓扑在执行前固定，包括静态推导的 extent。运行期 count 和 mask 是固定容量内的普通 payload。不提供动态 field 行、End 协议、数值端口 kind、failure-delivery selector 或内核 scalar-bounds 字段。

Tensor 统一 backing、片段和 view 访问。宿主默认 tiled planar 分配，继续支持合法的有符号/零 stride、外部 owner 和稀疏物理覆盖。拥有型窗口暴露真实 run；显式 collection 和 packing 消耗容量与 work。普通图像的 straight alpha 默认由算子实现和 helper 体现，不通过内核合规 validator 认证。

C metadata resolution 提供 schema、所选资源和静态共享 stage 声明；C mapping hook 提供静态读取。保留窗口具有显式 retain/release 生命周期。Query 和服务表在 callback 返回时失效。内核检查地址、授权、owner 与同步，不解释像素值。

## Identity 与运行期匹配

规范 compiler identity 包含程序形式、程序标识符、准备后的算子契约、静态 schema、参数、所选资源、输入投影、共享 stage 定义、相关 backend 选择，以及 plan 的每条指令与授权。因此重新编号 call 或 value 会改变 IR 和 plan digest。内容缓存 identity 不含物理 node、step 和 plan 标识符。原始指针、padding 和 allocation 地址不定义 identity。C import 拒绝错误 ABI 或大小；native 服务可用性还取决于构建和设备。Compiler 产物是可编辑、带版本的文本，不携带原生指针、库路径、设备句柄、Run 状态或缓存内容。
