# 计算边界

## 1. 模块职责与所有权

| Owner | 持有 | 边界 |
| --- | --- | --- |
| `GraphContext` | 复制的 workflow source、revision 和 snapshot currentness | 不持有 compiler 输出或执行 worker |
| `Compiler` | 校验、不可变 IR 阶段、静态 mapper 调用、把 tile 展开为 plan 指令、plan 验证和阶段 identity | 不持有 runtime Tensor、运行期 callback 或 daemon state |
| `ExecutionRun` | 单次执行的 program counter、block 值与参数、固定授权绑定、依赖证据、取消状态、work 预算和诊断 | 不拥有共享 pool、持久结果、依赖映射或指令顺序 |
| `ExecutionContext` | CPU worker pool、可选原生 lane、冻结的 operation registry、缓存和资源 ledger | 不管理 graph mutation 或 daemon Job |
| Operation definition | 冻结的 traits、静态依赖 mapper 和 callback 契约 | 不拥有执行容量、超出其编译期 mapping 的读取授权或发布权限 |

## 2. 数据与内存边界

`ExecutionPlan` 保留静态元数据、成员 tile 几何和完整的 tile 指令程序，不保留调用方地址。`ExecutionBindings` 为一个 Run 提供拥有型 Result 引用。每个 Result 由静态 Tensor 成员组成；返回 Result 和读窗口在 Run 完成后继续保留 backing 和租约。

Estimated bytes 用于规划和预留。Resource ledger 按资源类别计入已准入或已分配容量；估算值本身不是分配租约。`Region` 描述逻辑坐标，`StridedLayout` 将坐标映射到字节地址。

## 3. 执行边界

Planner 在执行前完成全部依赖展开。每个 `ExecutionRun` 按保存的顺序一次执行一条 plan 指令，只调用每条指令所需的既定服务：授权绑定、可选缓存查找、算子 start 与 poll、发布验证和 block 转移。它不调用 mapper、不创建 producer task、不推导 tile 映射，也不重排指令。每次进入 block、每条指令、每次授权声明和证据合并都消耗 `ExecutionOptions::maximum_dependency_work`，算子 work 计费消耗 Root work 预算；两条路径都检查取消与 currentness，因此空循环和很长的指令流都保持有界。耗尽时返回 `ResourceExhausted`。CPU callback 竞争 context 的固定 CPU worker；可选原生 callback 使用配置的 backend lane。Context 级等待准入限制尚未开始的 callback；普通 callback 开始运行后释放等待名额。Staged CPU job 会持续占用名额，直到 tile callback 退役且 job 离开队列。lane 和 staged-work 契约见[并行执行](Parallel-Execution-Model.zh.md)。

Public API 暴露 plan、binding、result、status 和 diagnostics。Queue entry、byte-ledger lease、native handle 和 callback owner 属于私有实现状态。Daemon 使用 public package，并自行管理 IPC 与 Job 生命周期。

## 4. 限制与非目标

- Graph revision 表示 compiler currentness，不是 daemon session id。
- Run 是同步 kernel 调用，不是 public scheduling object。
- Backend label 记录放置意图或结果，不是 device handle。
- 只有通过 cancellation 与 graph-currentness 检查后才发布结果。
- Kernel 不负责 document persistence 或 result recovery。
