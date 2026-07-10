# 图状态访问不是调度器派发

YAML 加载、cache 命令、inspection 和 ROI projection 等 graph-state operation
应通过明确的图状态访问边界执行，而不是通过 `SchedulerTaskRuntime` 执行。
`SchedulerTaskRuntime` 继续作为 scheduler-owned 的已经 ready 的 compute task
callback 派发边界。它不接收 task graph，不维护 graph dependency counter，
不派生 dirty work set，也不修改 dirty-region snapshot。Dependency accounting、
source-first dirty task release、task pruning 和 ready-task submission 保持在
compute-service dispatcher 边界内；graph-state access 由 `GraphStateExecutor`
单独保护，因此 scheduler 实现不会承担非计算命令。

当前默认语义是 per-graph exclusive access：compute 与 graph-state operation
不会并发读取或修改同一个可见 `GraphModel`。`Kernel` 通过
`GraphStateExecutor` 提交同步、异步、返回图像以及 scheduler-backed parallel
compute request。Parallel compute 仍可用 `SchedulerTaskRuntime` 派发 ready task
callback，但外层 request 会持续持有 graph-state access boundary，直到 compute
service 提交一致的可见 graph state。

后续可以设计 `StagedInterruptibleCommit` 策略，让长时间 compute 将输出暂存到
可见图状态之外，并允许 graph-state operation 在 commit 之前请求取消。该策略应与
`ComputeIntent` 分离；
`ComputeIntent` 继续表达 HP/RT 语义，而不是并发或提交模式选择。
