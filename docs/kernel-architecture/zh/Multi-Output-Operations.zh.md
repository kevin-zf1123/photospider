# 具名输出与 Result 端口

## 范围与所有权

一个节点可以暴露多个具名 Result 输出。每个输出具有静态 Tensor-only schema、输入投影、可见性策略和独立 demand。Workflow 边指定节点和输出端口。`ExecutionResult::results` 保存具名 Result owner，不再区分数值端口种类。

`WorkflowNodeOutput` 指定 `source_node` 和 `source_port`；`WorkflowOutput` 指定公开的 `name`、`node_id` 和 `port`。默认端口名是 `value`。一个 Result 本身也可以包含多个具名静态 Tensor 成员，这与节点的多个输出不同。

## 输出选择与依赖

`OperationTraits::outputs` 声明输出，`select_operation_output` 选择其 traits，`ResultStaticQuery::output_index` 和 `ResultProgramQuery::output_index` 分别在 mapper 与 callback 中标识输出。映射的读取分别选择 Result 成员的 `tensor_slot` 和局部 tile 索引。

Plan 把声明与可执行工作分开。`ExecutionPlan::steps()` 为每个 call 的每个输出保存一个 `PlanStep`，无论是否有读取者。只有某个 root（所选请求中的具名 return、分支条件、边参数、有副作用的 call 或 mandatory stage）通过映射读取到达的 tile 才有指令。因此未选中的 sibling 输出保留其 step 声明但没有指令，用 `select_outputs` 过滤时可以移除一个 sibling 的指令而保留另一个的指令。

输出只能读取准备后输入投影内的输入。共享 stage 声明具有自己的准备后投影和依赖。同一 call 的所有输出共享其 stage：planner 把 stage 单元规范化到该 call 的第一个 step，因此同一 call 的各输出到达相同的 stage 指令，VM 为每个 call、stage 和 block activation 保留一份 stage 状态。需要联合计算的算法可以发布多成员 stage Result，输出 tile 指令从中发布已授权的 tile view。没有任何 mapper 指名的纯 stage 不产生指令；mandatory stage 作为副作用根编译。

所有样本读取，包括控制值，都需要已编译的 payload 授权。消费者继承其消费的共享提交所冻结的证据，并添加自身授权的证据。继承证据不授予读取 stage 输入的权限。消费者可见性遵循认证：独立输出 tile 在其指令认证后即可读取，而 `CompleteBundle`、`StablePrefix` 或完整校验输出只有在其认证闭包执行后才可读取。

## 身份与资源影响

输出契约、stage 声明、参数、资源和输入 schema 进入准备后的 operation identity。普通 tile 指令的 key 包含所选输出、成员和 tile。活动共享状态不跨 Run 复用。可选 completed-result、checkpoint 和 block 缓存有独立的纯度和指令资格规则；见 [Global Results](Global-Results.zh.md)。

每个 tile 以事务提交 payload、coverage 和依赖证据。`CompleteBundle` 使用准备后的固定成员范围，planner 把所有必需的兄弟 tile 编译进读取方的闭包；`StablePrefix` 遵循显式的成员局部 tile 顺序，读取 tile `k` 依赖 tile `0..k`。Bundle 可以扩大为单次请求编译的工作范围。具名 return 由该 call 已认证的值投影而来；投影得到的 return 对象与之前通过 `result_publication` 交付的对象不一定共享 `object_id`，因此消费者应比较覆盖范围、内容和 owner 语义。默认存储布局不创建 bundle。

## 可执行 fixture

当前行为注册由 `cmake/KernelTests.cmake` 定义，保留回归包括共享 stage 和成员 tile 执行场景。注册本身不证明某次执行结果。测试生命周期遵循[测试与验证](../../development/zh/Testing-and-Validation.zh.md)。
