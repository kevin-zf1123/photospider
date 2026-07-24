# 策略与执行架构

本文档是 Photospider 如何选择就绪工作以及如何执行这些工作的当前权威说明。
策略与执行分别属于不同的所有权域：

- **策略（policy）** 对 Host 已准入的不可变候选项排序，不拥有任何资源；
- **执行路由（execution route）** 是 Host 的私有实现，拥有物理队列、工作线程、
  设备和完成适配器；
- **Run** 拥有请求身份、取消/取代状态、依赖进度、暂存输出及其终态结果；
- 只有 **Host** 可以校验策略输出、预留资源、提交启动并进入执行器。

原有的工作线程所有型 scheduler SDK、`IScheduler` 层次结构、每 Graph 物理所有者
以及 scheduler 插件 ABI 均已移除。不存在兼容适配器或转发 API。

## 所有权模型

`ExecutionService` 是嵌入式 Host 使用的进程级执行域所有者。它拥有：

- 有界就绪存储，以及其中完整的就绪字节计费；
- Interactive 和 Throughput 各一个策略绑定；
- 进程公平性状态和三比一类别仲裁状态；
- 固定 CPU 工作线程池、一个由 service 拥有的 Metal 工作线程 lane，以及私有
  `serial_debug` 和 `gpu_pipeline` 路由；
- 由 Host 生成的候选项、Graph、Run、条目版本、入队、快照和选择身份；
- 从就绪到执行的资源交换，以及飞行中回调的所有权。

`GraphRuntime` 只保存复制得到的 HP 和 RT 路由 ID 及其非零代次。它拥有 Graph
状态、计算/事件/跟踪观察能力和请求串行化，但不拥有物理工作线程池或策略插件
上下文。

`ComputeRun` 在 Run 租约后方保持稳定，直到每个回调、依赖释放、完成发布以及
暂存提交竞争者都已结算。策略回调永远不会收到 Run 指针或租约。

资源锁顺序为：

```text
ExecutionService 就绪存储/服务状态
  -> Run 状态
    -> ResourceLedger 预留状态
```

持有上述任一锁、Graph 锁或策略注册表/绑定锁时，不得调用策略回调。

## 策略类别与绑定

系统恰好有两个服务类别：

| 类别 | 目标工作 | 内建类型 |
| --- | --- | --- |
| Interactive | 延迟敏感工作，可选单调时钟截止期限 | `interactive` |
| Throughput | 带权后台工作 | `throughput` |

进程为每个类别各拥有一个绑定。即使两个类别使用同一个 DSO 类型，也会得到彼此
独立的上下文和独立的非零绑定代次。同名替换仍会创建新代次、清除旧代次故障、
排空旧代次的活动调用、只销毁一次其上下文，并且仅在最后一个依赖值消失后才
退役其 DSO 租约。

`configure_policy_defaults` 在发布锁外准备两个候选绑定，并且要么同时提交，
要么都不提交。`replace_policy` 对单个类别采用相同的准备/发布/排空纪律。
创建、校验或发布失败时，原绑定及其代次保持不变。

## 纯 C 策略 ABI v1

唯一安装的策略头文件是
`include/photospider/policy/policy_plugin_api.h`。它在 C11 和 C++17 下均可自包含，
并在 64 位平台上定义自然布局 ABI，且恰好只有两个导出：

```c
uint32_t ps_policy_plugin_get_abi_version(void);
ps_policy_status_v1 ps_policy_plugin_get_api_v1(
    ps_policy_plugin_api_v1 *out_api);
```

API 表包含四个必需回调：元数据、创建、选择和销毁。各记录的精确大小为：

| 记录 | 字节数 |
| --- | ---: |
| `ps_policy_string_view_v1` | 16 |
| `ps_policy_type_metadata_v1` | 80 |
| `ps_policy_create_args_v1` | 40 |
| `ps_policy_candidate_v1` | 120 |
| `ps_policy_selection_snapshot_v1` | 64 |
| `ps_policy_decision_v1` | 48 |
| `ps_policy_plugin_api_v1` | 80 |

ABI v1 只接受精确匹配的大小、kind、对齐、偏移、回调指针、枚举值、边界以及
必须为零的保留字。它没有尾部扩展规则。记录形状一旦变化，就必须引入新的 ABI
代次。

策略只会收到标量候选项描述符：不透明 ID、截止期限、权重、可信工作量与字节
计费、预计 Graph/Run 服务分数、派发年龄、入队序列和标志。它永远不会收到
执行器、工作线程、设备、队列、分配服务、资源授权、Run、Graph、完成路由、
日志器或生命周期回调。借用的快照内存只在 `select` 返回之前有效。

Host 会立即以本地方式打开 DSO；在精确确认 ABI 相等之前，只解析并调用版本
导出；随后才校验完整 API 和每一条元数据。一份 DSO 以全有或全无的类型注册
事务发布。内部重复、冲突、无效 UTF-8、非规范名称、保留的内建名称、无效类别
掩码或格式错误的回调输出，都不会发布任何条目。

活动元数据、绑定、上下文和调用分别保留独立的 DSO 租约。注册表卸载只移除
可见性，不能使活动绑定失效。对于诚实但永不返回的进程内回调，系统不承诺超时
或强制恢复；进程隔离是单独的未来边界。

## Host 生成的前沿

Host 在调用策略前先选择服务类别。两个类别都有可启动工作时，最多允许连续启动
三个 Interactive 工作，随后必须启动一个 Throughput 工作。在选定类别内，
每个活动 Run 最多暴露一个 lane 头。

插件看到候选项之前，Host 会按以下规则收缩候选集合：

1. 只考虑当前、可启动、取消安全、路由兼容的 lane 头；
2. 同类别连续启动八次后，只保留年龄最大的前沿；
3. 否则，只保留具有最早有限截止期限的 Interactive 工作；
4. 移除不在最低预计 Graph 服务量子内的候选项；
5. 移除不在最低预计 Run 服务量子内的候选项；
6. 分数饱和时通过最早的稳定入队序列退出饱和状态；
7. 最终内建平局规则是稳定入队顺序。

内建策略与 DSO 策略使用同一套前沿和校验路径。插件可以从不可变的原始快照中
选择一个候选项，也可以弃权；它不能扩大前沿或凭空生成工作。

## 决策分类与回退

Host 首先根据原始调用校验回调完成状态和每一个决策字节：状态、大小、kind、
保留字段、决策种类、代次回显以及候选项身份。只有来自有效原始快照的选择，
才会继续与 Host 当前状态比较。

结果明确分为两类：

- **因 Host 状态而过时**：决策生成时有效，但就绪、取消、取代、路由、
  公平性或代次状态已经变化。Host 最多再取得两个新的插件快照，之后使用当前
  同类别的内建选择。这不会记录策略故障。
- **无效插件决策**：回调失败、抛出可捕获的外部异常、弃权、返回格式错误的
  字节、回显错误代次，或者指定原始快照之外的候选项。该绑定代次的第一次故障
  会被粘滞记录，此后的调用会绕过该绑定；成功替换会清除故障。

故障类别为 `Abstained`、`CallbackStatus`、`CallbackException`、
`MalformedDecision`、`GenerationMismatch` 和 `CandidateOutsideSnapshot`。
可选 Host 快照分配/边界失败不算故障，而是使用未截断的内建路径。可信内建
不变量被破坏时，只以 `GraphErrc::ComputeError` 使受影响的 Run 失败。

## 预留后启动

返回的候选项 ID 并不是执行权限。Host 保留一个私有 `SelectionPin`，其中包含
原始条目身份/版本，并按规定的锁顺序重新检查当前状态。`StartTransaction` 使用
不抛异常的 RAII 暂存 CPU、保留内存和临时空间授权。

最终提交不进行分配且不抛异常。它以原子方式：

- 移除精确匹配的就绪条目；
- 将其就绪授权交换成执行授权；
- 推进类别、Graph 和 Run 服务计账；
- 更新 Interactive 突发计数和飞行中状态；
- 把回调所有权转移给所选私有路由。

提交之前不会开始任何执行器回调。提交前的每一次拒绝或异常都会保持就绪/公平性/
突发/飞行中状态不变，并且只释放一次暂存授权。完成、取消、取代、依赖释放和
Run 结算同样只释放一次各自授权。

重验后 execution grant 暂时耗尽不属于 plugin fault 或 obsolete-decision retry。ready
store 只在该 worker 的当前 cycle 中标记精确 candidate/version，并在不移除 entry、不释放
ready grant、也不 charge fairness 的情况下重算 class/frontier selection。这样，独立的较低
优先级 Run 可以从剩余 current candidate 中 start。如果每个 lane-compatible candidate 都被
标记，worker 会等待 predicate-protected notification epoch；enqueue、dependency release、
completion/grant release、cancellation/failure purge、policy replacement 与 shutdown 都会推进
该 epoch。spurious wake 不触发 retry；50 ms low-frequency fallback 覆盖其他不可观测的外部
child-grant release，随后清除 cycle mark，并重验 current Host state。

## 私有执行路由

路由词汇表是封闭的：

| 路由 | 所有权与行为 |
| --- | --- |
| `cpu` | Host 生命周期固定 CPU 工作线程池，支持可复用的多条目执行；只暴露 CPU |
| `serial_debug` | CPU 工作线程零，只允许一个回调处于飞行中；只暴露 CPU |
| `gpu_pipeline` | CPU fallback 使用同一个固定 CPU 池，Metal 使用一个由 service 拥有的 lane；Host 报告 Metal 时依次暴露 Metal、CPU，否则只暴露 CPU |

`heterogeneous` 不是别名。执行路由不是插件，不能扫描或加载。

`HostExecutionConfig` 控制未来会话的 HP/RT 路由 ID，以及 `[0,8]` 范围内的
工作线程请求。零表示选择有界自动值。进程 CPU 池固定后，零或相同请求会保留
现状；不同的正值请求会被拒绝。已有 Graph 会话保留其路由绑定。

`replace_execution` 校验封闭词汇表中的路由，在不创建所有者的情况下准备新绑定，
与同一会话的活动请求串行化，并发布新的非零代次。同名替换同样推进代次。
失败时保留旧路由。

操作选择会在 Run 准入前同时冻结实现 callback 及其 `Device`。完整 HP、dirty HP/RT
和连接参数预检都使用同一份 route-aware inventory。connected-preflight preparation 还会在
不进入 provider code 的情况下冻结每个 callable/DSO lease 与完整 service root；只有已安装 Run
才能执行 reserved start 并调用 provider，之后依赖 output 的 dirty planning 仍由 Run 拥有。
每个 ready submission 都携带冻结的 device；如果 device 不在已配置 route/Host inventory 中，
`ExecutionService` 会在发布 Run 前拒绝它。CPU submission 进入固定 CPU 池，Metal submission
进入单一 GPU lane。两个 lane 共用 ready store、policy decision、reserved-start transaction、Host
ledger、Run maximum-parallelism grant、cancellation、completion、exception、reuse、
shutdown 与 drainage 规则；不会创建第二套 device-capacity authority 或 per-Graph
executor。

完整 HP、dirty HP/RT、连通性预检、初始就绪工作和依赖释放工作，都会进入同一套
就绪存储、策略、预留后启动、私有路由及 Run 租约完成路径。

## Host、CLI 与 IPC 接口面

公共 Host 有八个策略操作和六个执行操作。其最终非析构虚函数数量为 58。
策略发现和绑定属于进程作用域；执行信息/替换和执行跟踪属于会话作用域的复制值。

`graph_cli` 暴露：

```text
policy list|get|set|scan|load|plugins|help
execution list|get|set|help
```

配置使用 `policy_dirs`、`policy_interactive_type`、
`policy_throughput_type`、`execution_hp_type`、`execution_rt_type` 和
`execution_worker_count`。已移除的 `scheduler` 命令和 `scheduler_*` 键会被
直接拒绝，不进行翻译。

IPC 协议版本 2 用八个 `policy.*` 和六个 `execution.*` 方法取代旧方法族，其中
包括非破坏性的 `execution.trace`。守护进程恰好通告 60 个排序且唯一的方法。
协议版本 1 和旧方法名会在访问 Host 之前被拒绝。精确 schema 与边界维护在
[`IPC-Protocol-v2.zh.md`](../../codebase-structure/zh/IPC-Protocol-v2.zh.md) 中。

## 可观察性与生命周期证明

执行跟踪分页包含复制得到的序列、epoch、节点、工作线程、动作和时间戳值。
每页非破坏性读取，上限为 4,096 条，并保持丢弃/耗尽语义。跟踪数据不携带队列
或回调能力。

`ExecutionService` 还拥有 source-private `ExecutionLifecycleTelemetry`：一个带 schema version、
固定 65,536 条记录的 ring，支持非破坏性的 1..4,096 条 snapshot page、atomic cut、显式 cursor
gap 与饱和累计 drop accounting。其 15 个 post-transition counter 会合并 registry state 与精确的
ready entry、已进入 operation callback、live root reservation、live child grant、policy
invocation 以及 current/displaced policy-binding ownership。Record 只包含复制的 scalar
identity，不含 label、path、pointer、callback、lease 或 mutable handle。该 store 不会加入 Host、
CLI 或 protocol v2。

`RunLifecycleRegistry` 现在驱动 Graph-close 与 process-shutdown cancellation。Shutdown 会让已
admission 的 ready/execution/completion path 保持可用，直至所有 Run settle；随后 join 物理 worker、
retire policy binding，并在 15 个 counter 全部为零时发布 `ServiceStopped`。因此，永不返回的
callback 会如实阻塞 shutdown，而不会被伪装成可恢复状态。同一 service 的 worker 或 policy
caller 会被 mutation-free preflight 拒绝；Kernel 一旦关闭 publication gate，意外 transition
failure 就会 fail-stop，因为该 gate 无法重开。通用数据异构执行属于 Issue #77；
进程隔离的插件监管属于 Issue #91。

## 实现与验证入口

- `include/photospider/policy/policy_plugin_api.h`
- `src/lib/policy/policy_registry.hpp` 和 `.cpp`
- `src/lib/compute/execution_service.hpp` 和 `.cpp`
- `src/lib/compute/run_lifecycle_registry.hpp` 和 `.cpp`
- `src/lib/compute/execution_lifecycle_telemetry.hpp` 和 `.cpp`
- `src/lib/execution/execution_task_runtime.hpp`
- `src/lib/runtime/graph_runtime.hpp` 和 `.mm`
- `src/lib/runtime/kernel_execution_facade.cpp`
- `include/photospider/host/host.hpp`
- `src/lib/host/embedded_host.cpp`
- `src/lib/ipc/{codec,client,host,request_router}.cpp`
- `tests/unit/test_policy_registry.cpp`
- `tests/unit/test_compute_run.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_ipc_daemon.cpp`
- `tests/integration/static_product_consumer_smoke.py`

另请参阅[计算流程](Compute-Flow.zh.md)、
[计算边界](Compute-Boundaries.zh.md)、[插件 ABI](Plugin-ABI.zh.md)和
[Graph 生命周期](Graph-Lifecycle.zh.md)。
