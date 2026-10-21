# 编译器与包版本契约

## 当前契约

Package 0.39.0 使用 SameMinorVersion 兼容规则和 SOVERSION `0.39`。C++ consumer 和外部 C operation module 必须针对匹配头文件重建。Operation module ABI 8 与 fixed invocation ABI 8 要求记录大小精确匹配。加载器要求 `ps_operation_module_get_api_v8`；入口缺失时返回 `TypeMismatch`，module 版本或大小非法时返回 `InvalidArgument`。

| 契约 | 版本 |
| --- | --- |
| Package | 0.39.0 |
| WorkflowDocument | 7 |
| OperationTraits | 30 |
| Operation module C ABI | 8, exact-size |
| Fixed kernel invocation C ABI | 8, exact-size |
| Native kernel preparation service ABI | 2 |
| `.psir` / `.psxp` 文本格式 | 5 |
| Result schema encoding | `photospider.result-schema.v6` |
| Validated IR | `ordered-retirement-ir-v5` |
| Example optimizer | `optimize@1` (provenance only) |
| Physical plan | `physical-task-program-v5` |
| 已保存算子契约 | `fixed-operation-contract-v3` |

这些版本轴相互独立。Module ABI 描述静态准备、物理 lowering 和 kernel preparation。Invocation ABI 描述不可变输入、独占输出与 scratch、取消和完成。Native service 2 在 Shared 与 Device 存储上准备 Metal 或 Vulkan kernel；显式 GPU COPY 通过已准入的 Shared staging 支持 Host 端点。其可用性取决于构建和所选设备。所有权、记录大小和支持的内存域见 [plugin ABI](../../kernel-architecture/zh/Plugin-ABI.zh.md)。

## Compiler 阶段与产物

前端/`bind_ir`、可选命名 optimizer 和 artifact 导入都发布同一种不可变 `ValidatedIR`。Planner 接受 DependencyPaths 或 ControlFlow；对依赖路径，在私有 lowering 副本中按 Call ID 去重。CFG 循环保留动态激活语义。默认编译不调用 optimizer。有序 optimizer 调用包含重复名称，每次产生编号版本；内容 identity 不包含生产历史，因此 no-op 可保持 digest。

Artifact v5 的 `.psir` 使用 `stage ir` 和显式程序形式；`.psxp` 保存 lowering 后的 CFG、完整物理 task 表，以实际输入 IR identity 为 parent。原始及变换后的 IR 都可导入并直接规划。Plan 导入链接并校验保存的表，不再次执行 lowering；registry/资源匹配和当前性约束继续生效。算子 manifest digest 描述契约，不认证代码。见[工具](Compiler-Tools.zh.md)、[格式](Compiler-Tool-Formats.zh.md)和[编译器架构](../../kernel-architecture/zh/Compiler-and-Execution.zh.md)。

`CompiledWorkflow` 包含有序 IR 版本及可选 Plan。`ExecutionPlan::input_ir_digest()` 标识父 IR。消费者必须针对当前公共 API 重建，并重新导出使用版本 1–4 写入的 artifact；导入会拒绝这些版本。

## Workflow 输入与算子执行

Workflow 声明要求静态 Result schema，binding 按精确名称提供拥有型 `ResultRef`。Plan 固定 schema、tile 几何、placement、task 依赖、kernel symbol 和 block 控制流；tile extent 默认 128。Lowering 将所请求的输出与显式输入读取转换为物理 task；call 内共享计算通过 task 和私有 value 表达。

`ExecutionContext::prepare` 将 Plan 链接成 `PreparedPlan`，解析 kernel 并准备 native capacity。`submit` 创建 Run，持有独立的 binding snapshot、取消状态和动态 activation 状态。CPU kernel 同步返回；已接受的 GPU kernel 通过完成 callback 结束。就绪 task 可以跨 call 执行。CallGroup 按 IR 顺序退休；最终 RET 在前序 call 退休后暴露所选 Result。失败和取消必须排空已接受的工作，之后才能释放其借用的存储。

## Result 与图像接口

Result 包含 1 到 16 个具名静态 Tensor 成员。Dtype、正 shape、rank、batch/atomic 轴和拓扑在执行前固定，包括静态推导的 extent。运行期 count 和 mask 是固定容量内的普通 payload。

Tensor backing 支持片段、合法的有符号/零 stride、外部 owner 和稀疏物理覆盖。拥有型窗口暴露真实 run；显式 collection 和 packing 消耗容量与 work。Plan 的 input layout 契约决定 Run 接受的表示。CPU COPY task 可以显式连接 Host 与 Shared tile。图像数值语义由算子实现与 helper 定义。

静态 C callback 提供 schema、所选资源和物理 operation program。运行期 kernel 在 invocation 生命周期内借用固定 view 与 scratch。Host 在编译、准备和 binding preflight 阶段校验结构契约；kernel 负责数值正确性，并必须遵守声明的访问范围。

## Identity 与运行期匹配

规范 IR identity 包含程序形式、标识符、准备后的算子契约、静态 schema、参数、所选资源和控制流。Plan identity 还包含 layout、placement、task 读写、依赖、scratch 需求、CallGroup、block reservation、返回签名和输出选择。重新编号 call 或 value 会改变这些 identity。原始指针、padding 和 allocation 地址不定义 identity。

Compiler artifact 是可编辑、带版本的文本，不携带原生指针、库路径、设备句柄、Run 状态或缓存内容。运行期 module/device 所有权保留在链接后的 `PreparedPlan` 及其 kernel 中。
