# ADR 0001：图状态访问不是调度器派发

## 状态

已接受。

## 背景

图文档加载、cache 命令、inspection、ROI projection 和图变更会读取或修改 `GraphModel`，
但它们不是 compute callback。若把这些操作当作 scheduler work，scheduler 实现就必须依赖
图所有权、持久化、dirty propagation 和变更语义。

Compute execution 具有不同的职责划分：计算子系统派生任务图，拥有 dependency counter 和
dirty work selection，并且只在依赖满足后释放具体工作。

## 决策

图状态操作通过显式的每图图状态访问边界执行，当前实现为 `GraphStateExecutor`；它们不会进入
`SchedulerTaskRuntime`。

`SchedulerTaskRuntime` 只接收具体 ready compute callback，不接收任务图、不维护图依赖计数、
不派生 dirty work set、不修改 dirty-region snapshot，也不提交图状态。

Dependency accounting、source-first dirty task release、task pruning 和 ready-task submission
保留在 compute dispatcher。当前默认行为是每图独占访问：图状态操作和计算请求不会并发读取或
修改同一可见 `GraphModel`。Parallel request 可以把 ready callback 派发到 scheduler worker，
但其外层请求会保持图状态边界直到一致提交。

Compute commit policy 与 `ComputeIntent` 保持分离。Staged、interruptible policy 可以改变
可见提交与取消的交互，但不会把图状态操作变成 scheduler work。

## 结果

- Scheduler 实现无需拥有 Graph、persistence、ROI 或 cache 即可测试。
- 图变更顺序只有一个显式所有者。
- 并行 callback 执行不意味着可见图状态会被并发修改。
- 未来进程级执行域必须保持相同的 ready-task-only 输入边界。
- 更高并发的图编辑需要显式 revision 和 staged-commit 模型，不能通过 compute scheduler 路由变更。

## 与其他决策的关系

ADR 0003 会改变物理执行资源所有权，ADR 0007 则冻结目标 Run、ready submission、
completion 与生命周期的详细边界；两者都保留本决策。当前没有决策取代 ADR 0001。
