# 具名输出与 Result 端口

## 范围与所有权

节点可暴露多个具名 Result 输出，每个输出有静态 Tensor-only schema、输入投影
和独立 selection。Workflow 边使用 `WorkflowNodeOutput{node, port}`，公开输出
使用 `WorkflowOutput{name, node, port}`，默认 port 为 `value`。节点的多个输出
与一个 Result 内的多个具名 Tensor 成员分别定义。`ExecutionResult::results`
持有所选具名返回。

## 固定 lowering 与选择

`OperationTraits::outputs` 声明输出契约；
`OperationLowering::requests` 为每个输出提供规范化 TileRequest。
`OperationProgramBuilder` 创建固定 task、显式 input read、schema read、
output write 和私有 value。Read 指明输入 port、成员、tile 与物理 layout，
invocation 期间不能扩张。

Plan 保留每个 IR call 及其输出 value 声明。可执行 task 是所选 returns、
分支条件、边参数和 mandatory effects 的闭包，未选择的纯输出可以没有 task。
`ExecutionPlan::select_outputs` 限制既有 coverage，并重建依赖、call membership、
liveness 与资源界，不再次调用 operation lowering。保留 task 的全部 writes
继续属于该 task 的固定 invocation。

同一 call 的多个输出可共享 producer task 和私有中间 value，其依赖使用普通
物理边。共享计算在每次 block activation 执行一次，另一个 Run 或循环 activation
持有独立存储。Input projection 限定输出可读取的输入 port，实际 payload 闭包
仍由显式 reads 决定。

## 可见性与身份

Publication policy 参与编译闭包。`CompleteBundle` 可要求兄弟成员 tiles，
`StablePrefix` 可要求成员局部的先前前缀。Kernel 保留这些静态要求，不在运行期
发现新依赖。Storage layout 本身不创建 bundle。

Task 成功完成后，其 tiles 可供内部后继读取。CallGroup settle 并到达 IR 退休头
后，才通过 `on_call_retired` 交付公开 exports；失败和 elided call 没有 values。
控制流成功结束且 delivery settle 后才投影具名 RET。Call exports 和最终返回
独立持有 Result，不保证共享 ObjectId。

输出契约、schema、参数、资源、固定 reads/writes 和物理 selection 参与编译
identity。活动中间状态属于其 Run，没有跨 Run 的 completed-Result、checkpoint
或 block cache。见[全局结果](Global-Results.zh.md)和
[执行契约](Compiler-and-Execution.zh.md)。

## 验证

内核行为检查覆盖独立输出选择、共享物理 producer、成员 tile 闭包、有序退休
及最终所有权。`cmake/KernelTests.cmake` 定义保留的检查入口；注册本身不代表
某次运行通过。内置算子的数值检查遵循
[测试与验证](../../development/zh/Testing-and-Validation.zh.md)。
