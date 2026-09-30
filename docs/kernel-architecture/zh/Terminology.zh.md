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
| `ExecutionBindings` | 每次 Run 按名称提供的输入 owner：`Value`、`RegionalSource`、`InputSnapshot` 或 `PlanarImage`。 |
| `Value` | 不可变逻辑 descriptor 与 Region、仿射字节布局、facets 和共享 storage owner。 |
| `Region` | descriptor 轴坐标中的逻辑子集，不表示字节范围。 |
| `PlanarImage` | 显式声明轴、分量组和受控样本访问的结构化图像存储。 |
| operation traits | 冻结的输入/输出契约、资源上限和 backend 能力声明。 |
| operation registry | 编译与执行 plan 所用的受信任 operation definition 集合，注册后冻结。 |
| digest / cache key | 用于复现检查或可丢弃缓存查找的非安全 identity。 |
| cancellation | 协作式停止请求，阻止较晚的结果成功发布。 |
| CPU fallback | 原生 callback 在发布输出前报告 backend unavailable，且 operation traits 允许时，新发起的 CPU attempt。 |

`SessionId`、`JobId`、daemon IPC 和进程生命周期属于 `photospider-daemon`；Daemon Job 是临时状态。持久 result 与恢复能力在当前产品边界之外。Kernel result 是内存中的值，其 backing 生命周期由 owner 决定。
