# Kernel 术语

下列执行术语采用[提出的 breaking design](Compiler-and-Execution.zh.md)，描述替换契约，不表示当前 C++ 类布局。[英文权威文档](../Terminology.md)。

| 术语 | 含义 |
| --- | --- |
| WorkflowDocument | 调用方持有的源图，包含具名输入与输出。 |
| GraphContext / GraphSnapshot | 源版本所有者和一致的编译快照。 |
| IRProgram / ValidatedIR | 可编辑 SSA 程序及其已校验不可变形式。 |
| DependencyPaths | 原始 DAG call 请求；重复 Call ID 表示同一调用。规划去重并合并符合条件的路径 block。 |
| ControlFlow / CFG | 包含有序 call site、类型化 block 参数和 Jump/Branch/Return terminator 的 block 图。 |
| SSA Result value | 具有唯一静态定义且使用受支配的 call 结果、外部输入或 block 参数。 |
| ExecutionPlan | 不可变可序列化 Task/Tile/Call/Block 表、常量、依赖、placement 要求和资源上界。 |
| PreparedPlan | 已绑定设备的不可变 kernel、解码 layout、直接入口，以及跨 Run 保留的 resource/module owner。 |
| PreparedKernel | 设备 prepare 生成的不可变 operation context；共享 invocation 可以并发读取。 |
| Run / RunHandle | 一次可变执行，包含固定 binding、动态 activation、completion record 和 result owner。 |
| block activation | 实际进入某静态 block 的一次实例；循环再次进入时创建新 activation。 |
| Task | Kernel、显式 COPY 或终端 effect 构成的调度单元，可以产生多个独立 tile。 |
| TileId | 静态 tile descriptor 索引；`(RunId, ActivationId, TileId)` 标识物化运行时 tile。 |
| CallGroup / CallDesc | 一个 IR call 的选中 task、返回声明与退休序号，也包括零 task 的 elided call。 |
| predecessor / successor | 去重后的 producer-task readiness 边及其转置。 |
| pending | 尚未成功完成的不同 predecessor 数量。 |
| fixed read / view | Compiler 解析的输入端口、tile、逻辑坐标、stride 和字节边界。 |
| schema-only read | 读取不可变 prepared schema，不建立 payload 依赖。 |
| ready | 依赖成功，可申请资源；额度仍可能延迟发射。 |
| issue / submission | 将 prepared invocation 及其 owner 交给 worker/backend。 |
| completion | Task 全部访问结束，成功写入具备所需可见性，内部消费者可以就绪。 |
| retirement | 按序将一个 call 的 status 与选定导出值移交 delivery stream。 |
| release | 最后一个 hold 结束，allocation 可独立于退休顺序释放。 |
| failure cut | 最小已观察失败 call ordinal；较早必要工作继续，cut 及之后未提交工作被跳过。 |
| abort | Run 关闭所有准入后 drain，用于取消、过期和 context 失败。 |
| Effect | 一个 call 的终端外部动作，只在退休头且 preparation 成功后发射。 |
| COPY | 显式传输/物化，在声明 domain 中产生新 tile allocation。 |
| packet / submission unit | 可选同 call GPU task 组，使用一个提交额度/完成事件和完整资源上界。 |
| SubmissionId | 已接受 task/packet 提交的标识，映射到成员，与 TaskId/CallId 独立。 |
| queue timepoint | 按实际 queue submission 排序的 backend 完成坐标，独立于 call 退休序号。 |
| block argument | 已有不可变值的新名称/owner；修改目标前捕获全部来源。 |
| live-through hold | 为后续 block 保留值，包括未作为显式参数传递的支配值。 |
| Result / ResultRef | 静态 schema、选中 tile handle 和 owning lease，不自动组装稠密输出。 |
| Tensor / Region | 数值存储 descriptor 和逻辑坐标区域；逻辑覆盖与物理 stride 分开。 |
| ExecutionBindings | 每个 Run 固定的精确名称、不可变输入 owner 和结构 schema。 |
| unchecked operator contract | 数值合法、声明访问和真实完成属于实现前置条件；executor 不认证样本。 |
| admission | 发射前原子取得 block 的 output/state/queue/completion/delivery/scratch 容量。 |
| compiler artifact | 版本化规范 IR/Plan 内容，不含 native pointer 或可变 Run 状态。 |
| digest | 用于复现的非安全内容 identity，不认证 native code。 |
| operation registry | 用于编译和链接的冻结受信任实现目录。 |
| ImmutableResource / ResourceSet | 内容寻址的不可变 preparation 输入，由 prepared owner 保留。 |
| operations SDK | 通用 scheduler 之外的领域算法、codec 和数值语义。 |

`SessionId`、`JobId`、daemon IPC 和进程生命周期属于 `photospider-daemon`。持久结果与恢复仍在产品范围之外。替换后的 executor 不含 payload 多级缓存、隐式 CPU fallback 或运行时 Need 协议。
