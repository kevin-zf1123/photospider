# 计算边界

## 1. 模块职责与所有权

| Owner | 持有 | 边界 |
| --- | --- | --- |
| `GraphContext` | 复制的 workflow source、revision 和 snapshot currentness | 不持有 compiler 输出或执行 worker |
| `Compiler` | 校验、不可变 IR 阶段、plan 和阶段 identity | 不持有 runtime Value、callback 或 daemon state |
| `ExecutionRun` | 单次执行的就绪 step、中间值、取消状态和诊断 | 不拥有共享 pool 或持久结果 |
| `ExecutionContext` | CPU worker pool、可选原生 lane、冻结的 operation registry、缓存和资源 ledger | 不管理 graph mutation 或 daemon Job |
| Operation definition | 冻结的 traits 和 callback 契约 | 不拥有执行容量或发布权限 |

## 2. 数据与内存边界

`ExecutionPlan` 包含复制的 operation metadata 与 demand 几何，不保留调用方输入地址。`ExecutionBindings` 为单次 Run 提供 Value、regional source、snapshot 或结构化 planar image。Run 在 callback 可能访问期间保留这些 owner；返回的 `Value` 或 `PlanarImage` 在 Run 结束后继续持有自己的 backing 和资源租约。

Estimated bytes 用于规划和预留。Resource ledger 按资源类别计入已准入或已分配容量；估算值本身不是分配租约。`Region` 描述逻辑坐标，`StridedLayout` 将坐标映射到字节地址。

## 3. 执行边界

每个 `ExecutionRun` 按 `ExecutionOptions::maximum_parallelism` 调度就绪 step。CPU callback 竞争 context 的固定 CPU worker；可选原生 callback 使用配置的 backend lane。Context 级等待准入限制尚未开始的 callback；普通 callback 开始运行后释放等待名额。Staged CPU job 会持续占用名额，直到 tile callback 退役且 job 离开队列。lane 和 staged-work 契约见[并行执行](Parallel-Execution-Model.zh.md)。

Public API 暴露 plan、binding、result、status 和 diagnostics。Queue entry、byte-ledger lease、native handle 和 callback owner 属于私有实现状态。Daemon 使用 public package，并自行管理 IPC 与 Job 生命周期。

## 4. 限制与非目标

- Graph revision 表示 compiler currentness，不是 daemon session id。
- Run 是同步 kernel 调用，不是 public scheduling object。
- Backend label 记录放置意图或结果，不是 device handle。
- 只有通过 cancellation 与 graph-currentness 检查后才发布结果。
- Kernel 不负责 document persistence 或 result recovery。
