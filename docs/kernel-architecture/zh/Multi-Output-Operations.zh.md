# 具名输出与 Result 端口

## 范围与所有权

一个节点可以暴露多个具名 Result 输出。每个输出具有静态 Tensor-only schema、输入投影、可见性策略和独立 demand。Workflow 边指定节点和输出端口。`ExecutionResult::results` 保存具名 Result owner，不再区分数值端口种类。

`WorkflowNodeOutput` 指定 `source_node` 和 `source_port`；`WorkflowOutput` 指定公开的 `name`、`node_id` 和 `port`。默认端口名是 `value`。一个 Result 本身也可以包含多个具名静态 Tensor 成员，这与节点的多个输出不同。

## 输出选择与依赖

`OperationTraits::outputs` 声明输出，`select_operation_output` 选择其 traits，`ResultProgramQuery::output_index` 在 callback 中标识输出。Tile Need 分别选择 Result 成员的 `tensor_slot` 和局部索引。

输出只执行准备后输入投影内的依赖。共享 stage 声明具有自己的准备后投影和依赖。Stage 在 Run 内由原始节点和 stage 标识，不依赖第一个请求的输出或 tile。需要联合计算的算法可以发布多成员 stage Result，输出 actor 从中发布已授权的 tile view。纯且未请求的 stage 不启动；mandatory stage 遵循 Run 的副作用根规则。

所有样本读取，包括控制值，都需要普通 payload Need。消费者继承其消费的共享提交所冻结的证据，并添加局部 Need。继承证据不授予读取 stage 输入的权限。

## 身份与资源影响

输出契约、stage 声明、参数、资源和输入 schema 进入准备后的 operation identity。普通 actor 的 key 包含所选输出、成员和 tile。活动共享状态不跨 Run 复用。可选 completed-result、checkpoint 和 block 缓存有独立的纯度和 actor 资格规则；见 [Global Results](Global-Results.zh.md)。

每个 tile 以事务提交 payload、coverage 和依赖证据。`CompleteBundle` 使用准备后的固定成员范围，并推进所有必需兄弟 tile；`StablePrefix` 遵循显式的成员局部 tile 顺序。Bundle 可以扩大单次请求触发的工作范围，默认存储布局不创建 bundle。

## 可执行 fixture

当前行为注册由 `cmake/KernelTests.cmake` 定义，保留回归包括共享 stage 和成员 tile 执行场景。注册本身不证明某次执行结果。测试生命周期遵循[测试与验证](../../development/zh/Testing-and-Validation.zh.md)。
