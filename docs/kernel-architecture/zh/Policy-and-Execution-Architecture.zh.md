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
- 固定 CPU 工作线程池、一个由 service 拥有的 Metal 工作线程 lane、固定的
  `DeviceExecutorRegistry`，以及私有 `serial_debug` 和 `gpu_pipeline` 路由；
- 由 Host 生成的候选项、Graph、Run、条目版本、入队、快照和选择身份；
- 从就绪到执行的资源交换、精确 implementation/exclusive-key gate，以及飞行中回调的所有权。

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

Host 在调用策略前先选择服务类别。两个类别都有 scheduler-selectable 工作时，最多允许
连续启动三个 Interactive 工作，随后必须启动一个 Throughput 工作。Scheduler-selectable
表示某个 current ready lane 头对该 worker 通过 Run lifecycle、cancellation、operation-gate
与 physical-route eligibility；它刻意不包含暂时性的 execution child-grant capacity。在选定
类别内，每个活动 Run 最多暴露一个 lane 头。

插件看到候选项之前，Host 会按以下规则收缩候选集合：

1. 只考虑当前、scheduler-selectable、取消安全、路由兼容的 lane 头；
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
原始条目身份/版本，并按规定的锁顺序重新检查当前状态。在不可逆的 ready/fairness
mutation 之前，`StartTransaction` 会暂存 CPU、retained-memory 与 scratch child grant，
以及首次使用的 implementation/key gate row。可能失败的 gate allocation 先于 active-counter
mutation；任何拒绝都会通过不抛异常的 RAII 释放暂存 grant。

余下的提交不进行分配且不抛异常。它以原子方式：

- 获取精确 implementation count 与非空 exclusive key；
- 移除精确匹配的就绪条目；
- 将其就绪授权交换成执行授权；
- 推进类别、Graph 和 Run 服务计账；
- 更新 Interactive 突发计数和飞行中状态；
- 把回调所有权转移给所选私有路由。

提交之前不会开始任何执行器回调。提交前的每一次拒绝或异常都会保持就绪/公平性/
突发/飞行中状态不变，并且只释放一次暂存 grant 与 gate row。完成、取消、取代、
依赖释放和 Run 结算同样只释放一次各自拥有的状态。已启动 callback 会一直持有
operation gate，直到 provider exit 或 callback skip；即使 cancellation 或 failure 已清除
其 queued sibling 也不例外。

Observation sink 可以在 route commit 前立刻、在 Run terminal arbiter 下预留 service-start
causal coordinate。该 coordinate 是 staged observation，而不是 committed start 的证明：
commit 返回 false 时会调用 sink 的显式 abort，且不发布 callback；commit 成功后则让
它保持 open，直到 callback delivery 完成。M1 reservation entry/completion frontier 因而为
reserve → commit → callback/abort 建立 fence，也覆盖 copied record count 仍未变化的两个
间隙。

重验后 execution grant 暂时耗尽不属于 plugin fault 或 obsolete-decision retry。ready
store 只在该 worker 的当前 cycle 中标记精确 candidate/version，并在不移除 entry、不释放
ready grant、也不 charge fairness 的情况下重算 class/frontier selection。这样，独立的较低
优先级 Run 可以从剩余 current candidate 中 start。如果每个 lane-compatible candidate 都被
标记，worker 会等待 predicate-protected notification epoch；enqueue、dependency release、
completion/grant release、cancellation/failure purge、policy replacement 与 shutdown 都会推进
该 epoch。spurious wake 不触发 retry；50 ms low-frequency fallback 覆盖其他不可观测的外部
child-grant release，随后清除 cycle mark，并重验 current Host state。

只有成功提交的 service start 才会发布 evidence-startable class fact。该 observation cut 会
重验相同的 ready/lifecycle/operation/route predicate，并额外要求每个 class 至少一个
candidate 的 live child-grant capacity 足够。这些 capacity-aware evidence fact 只决定 M1
applicability；既不筛除 policy snapshot，也不更新三比一 `consecutive_interactive_` state。
失败的 `try_grant()` 不发布 start observation。

Implementation cap 或已占用 exclusive key 会把对应 candidate 从 scheduler-selectable
frontier 移除，但不会向 policy plugin 暴露 operation metadata。Worker retirement 释放 gate
时会推进同一个 notification epoch。Direct sequential caller 会在不持有 resource reservation
的情况下进行 cancellation-aware wait，随后只在 provider entry 周围获取同一 gate 以及一份
CPU/byte/scratch root。

每个由 Host 拥有的 retained operation 或 constraint key 都按实际复制的
`std::string::capacity()` 加空终止符计费。Full-plan admission 会为每个逻辑 task（包括每个
tile）预先分配一份独立拥有、已经计费的 constraint record，并把每份 record 恰好一次移入
对应 task 的唯一 `ReadyTaskSubmission`。Dirty admission 会对每个 active task 采用相同做法。
二者都在移动任何 record 之前冻结完整 shared estimate。Connected-preflight
callable/submission 的副本与 direct lease 分别独立计费。operation gate 只保存 borrowed
`string_view`，并在稳定 submission 或 direct-lease owner 退出前擦除。Queued work 借用其
`QueueEntry` 所拥有的 submission；direct acquisition 会在查询 gate 前把 caller constraints
复制进自身 lease state，其 wait predicate、start 与 finish 都借用该 state-owned 副本。因此
caller 输入不必比返回的 lease 存活更久。该做法既不重复也不漏算 string ownership。Checked
terminator overflow 与少一个 byte 的 retained limit 都会在 provider entry 前失败，不留下 gate
或 ledger 残留。

## 私有执行路由

路由词汇表是封闭的：

| 路由 | 所有权与行为 |
| --- | --- |
| `cpu` | Host 生命周期固定 CPU 工作线程池，支持可复用的多条目执行；只暴露 CPU |
| `serial_debug` | CPU 工作线程零，只允许一个回调处于飞行中；只暴露 CPU |
| `gpu_pipeline` | CPU fallback 使用同一个固定 CPU 池，Metal 使用一个由 service 拥有的 lane；固定 registry 拥有 Metal executor 时依次暴露 Metal、CPU，否则只暴露 CPU |

`heterogeneous` 不是别名。执行路由不是插件，不能扫描或加载。

`HostExecutionConfig` 控制未来会话的 HP/RT 路由 ID，以及 `[0,8]` 范围内的
工作线程请求。零表示选择有界自动值。进程 CPU 池固定后，零或相同请求会保留
现状；不同的正值请求会被拒绝。已有 Graph 会话保留其路由绑定。

`replace_execution` 校验封闭词汇表中的路由，在不创建所有者的情况下准备新绑定，
与同一会话的活动请求串行化，并发布新的非零代次。同名替换同样推进代次。
失败时保留旧路由。

操作选择会在 Run 准入前冻结一份 coherent callback、metadata、`Device` 与非零 implementation
revision。Planning 只保留 callback-free identity/metadata/shape；submission 必须重新解析同一个
identity，之后才能保留 callable/DSO lease。完整 HP、dirty HP/RT 和连接参数预检都使用同一份
规范化 route-aware inventory；full-task cache identity 会包含该 inventory 与 registry generation。
Region propagation 与 dirty TensorSlice eligibility 也会使用该 request inventory 和匹配的
HP/RT intent，在检查 source-private core identity 前选出实际的 revisioned
`OpImplementation`。它们不会使用 scalar-only lookup，也不会过滤掉 route 已选中的 same-key
device candidate。HP TensorSlice planning 会立即把每个已接受的 executable target/upstream
selection 转换成 callback-free operation key 与完整 identity/device/shape/metadata route，
随后释放临时 callable/DSO lease。Dirty preparation 会在 ROI mutation、task materialization、
callable resolution 或 admission 前，把这些冻结 route 与 active task-population route 比较；
空 active view 会在 frozen context 比较前返回，而任何剩余 active route 或 context 不匹配都返回
`NoOperation`。最终 callable re-resolution 仍是强制步骤。
connected-preflight preparation 还会在不进入 provider code 的情况下冻结每个 callable/DSO
lease 与完整 service root；只有已安装 Run 才能执行 reserved start 并调用 provider，之后依赖
output 的 dirty planning 仍由 Run 拥有。
每个 ready submission 都携带冻结的 device；如果 device 不在已配置 route/registry inventory 中，
`ExecutionService` 会在发布 Run 前拒绝它。CPU submission 进入固定 CPU 池，Metal submission
进入单一 GPU lane，再进入匹配的 registry executor。两个 lane 共用 ready store、policy
decision、reserved-start transaction、Host
ledger、Run maximum-parallelism grant、operation implementation/key gate、cancellation、
completion、exception、reuse、shutdown 与 drainage 规则；不会创建第二套 device-capacity
authority 或 per-Graph executor。

完整 HP、dirty HP/RT、连通性预检、初始就绪工作和依赖释放工作，都会进入同一套
就绪存储、策略、预留后启动、私有路由及 Run 租约完成路径。

V-6 不新增 configured execution route，也不新增第二套 ready store。
`ReadyFence::async_wait` 接收一个共享的注入 executor；该 executor 必须入队，而不能 inline
调用。预先构造的 continuation 会在 pending 或 queued 时保留 executor，并在 callback 进入时
把该 owner 转移到 callback-local retention。这既让临时 executor ownership 存活到 callback
完成或 exception unwinding 结束，也会释放 executor-owned queue 的 self-reference。Fence
state 与 source-private `ValueTransferTask` 都不拥有 worker 或 queue。仓库 fake executor 只是
确定性且线程安全的测试机制；C++17 mutex/condition-variable rendezvous 会在不使用 sleep 的
情况下执行真实的 registration/publication、cancellation/callback-entry 与
transfer-destruction/callback-entry 竞争。

V-7 在同一个 `ExecutionService` domain 中增加 source-private 的固定
`DeviceExecutorRegistry`。仓库 Metal plugin 启用时，Apple entry 拥有一个 device 与 command
queue，提供 invocation-scoped texture/buffer allocator，并保留经过校验的 process-lifetime
pipeline cache。
Reserved-start worker 会同步进入该 executor，并通过同一条 Run completion/exception path 调用
已经选中的 operation。Metal Perlin provider 现在只借用这些 resource，不保留 static native
state；`GraphRuntime`、`Kernel`、operation metadata 与 policy state 都不暴露 native handle 或
capability hook。

V-8 在同一个 execution domain 中增加 source-private 的显式 CPU/Metal access 与 residency。
`AccessPlan` 精确选择 direct、map、import、transfer 或 unsupported 中的一种，且不会执行隐藏
工作。Transfer 在发布不同且经过检查的 `StorageBinding` 时保留逻辑
`ValueRevisionId`；CPU-to-Metal upload 与 Metal-to-CPU readback 都是显式 asynchronous task。
Metal Perlin provider 发布 pending native Value，并编码 texture-to-shared-buffer readback，
不会等待 command buffer，也不会调用同步 `getBytes`。

`ExecutionService` 拥有唯一的进程级 `ResidencyManager`，并让 pending Value continuation
经过既有 ready store。精确 completion identity 包含 Graph/revision/target/intent/generation、
Run/task、source/destination revision、producer 与 binding fact。Freshness admission、
source/destination terminal publication 和 resident insertion 构成一个由 manager lock 保护的
事务。为该 publication 提供的每个 source 或 destination producer 还必须保留与对应 pending Value
完全相同、非空的私有 `ReadyFence` control state；revision、producer、allocation、binding 或
其他 scalar fact 即使匹配，也不能代替这项 terminal authority。Capability 不匹配会被拒绝，且不
消费 rightful admission、不结算任一 fence、不插入 residency，也不释放被保留的 resource owner，
因此经过精确准入的 producer pair 可以重试。晚到、重复或 identity 不匹配的 completion 不能发布 Ready、释放 dependent work、进入
residency 或恢复旧 commit right；如果它仍拥有对应 producer，就以 typed failure 结算受影响的
destination。Run settlement 会保留 executor 与 continuation，直至每个 pending fence callback
都退役。V-8 不新增第二套 ready store、Graph authority、persistence path 或 device-memory
capacity authority。已结算 replica 可在 Run 释放后继续复用，但 manager 默认的 64-entry
上限会在 publication pressure 下释放 revision 最低的 entry，从而限制强
native/provider retention；generation 指派本身不会批量清除它们。这个 entry 数量既不
测量也不准入 bytes。

Source-private I2 验证路径还提供精确 resident release 操作。它在 manager mutex 下验证一个
非零 revision、完整 `StorageBinding` 与 producer identity，然后只 extract 匹配的 map node；
解锁后才析构被保留的 Value/native owner。错误 identity 不产生任何效果。该窄操作不会 broad
clear residency、用 capacity pressure 代替清理，也不会改变普通 lookup、publication、
replacement、capacity 与 eviction 行为。

V-9 把权威 device-memory 与 scratch admission 放入既有 service `ResourceLedger`，而不是
policy 或 residency。每个已配置非 CPU `DeviceId` 都有隔离 limit。Metal 会在 allocation
前原子预留 native size/alignment plan、审计 `allocatedSize`，并在 command submission 前提交
actual byte。Persistent memory 随 native Value owner 跨 residency 延续；scratch 随精确
command completion 延续。Policy 看不到 native handle 或 token，不排列 byte owner，也不会
获得第二套 waiting/fairness queue。

Freshness publication 分为两个阶段。Kernel 先要求 `ExecutionService` 预跟踪 lineage，
但不指派 managed current identity；该可失败 allocation 会在 coordinator submission 前完成。
Candidate 被接受为 current 时，coordinator 会在持有自身 mutex 且发布自身 current row 前，
调用一个 no-throw、no-allocation 的 service callback。该 callback 会在 manager mutex 下把
精确的 accepted generation 指派给 manager，包括 coordinate 授权的数值下降。失败、被 close
拒绝或 born-stale 的 candidate 绝不调用它。之后才启动的 stale Run 不能替换这个由
coordinator 发布的 exact identity。未使用该 callback 的 standalone manager lineage 则另行
保留 numeric-maximum generation order。

## Compute I/O 执行边界

`ExecutionService::PoolState` 拥有唯一 source-private `ComputeIoExecutor`，其中有一个独立
worker。通过 limit check 后，会在同一 mutex 下、且早于 lazy payload construction、queue
publication、filesystem mutation 或 codec entry，暂时预留 task 数与正数 estimated-retained-
byte。Factory 抛异常、返回空 callback 或 task/queue-entry allocation 失败时，reservation 会回滚
且不签发 Accepted event。成功构造出的非空 callback 只会进入二选一的最终 decision：若准入仍
开放，则 queue ownership 与 Accepted 一起发布；若外部 shutdown 已获胜，则 Accepted 与其精确
关联的 Cancelled settlement 原子发布，且 callback 不会进入。每项已接受任务保留显式
transaction lifetime token，并返回 typed completion。Success、failure、queued cancellation、
running late cancellation、construction rollback 与 graceful shutdown 最终都会恰好一次释放账本。

Executor 会在这些相同 accounting linearization point 签发不可变 attribution event。
Admission 记录单调非零 sequence、typed decision、精确 charged task/byte delta 与同锁
process snapshot；settlement 关联该 admission，并记录精确 released delta 与其同锁
snapshot。Rejection 的 delta 为零。Process snapshot 可以包含无关并发 user；当 consumer
只观察其中一部分时，sequence gap 也合法，这两点都不会削弱精确单 task proof。

Worker 与 completion 边界会阻止按 identity 生效的 self-blocking。当准入仍开放时，owning
I/O worker 的 nested submission 会在改变任一 budget 或 lazy factory 前返回 inactive
`InvalidRequest`；若并发 admission stop 已发生，则 `ShuttingDown` 保持更高优先级。
Owning worker 可以复制已经 terminal 的 completion，但 nonterminal completion wait 会在
condition-variable blocking 前失败。Completion 只为该比较弱保留 executor identity。向另一套
独立 executor 提交并等待仍然合法。

Lazy factory invocation 使用无分配、异常安全的 thread-local scope stack。`shutdown()` 会在
改变 `accepting`/`stopping`、取得 join authority 或等待 worker 前，拒绝 stack 任意位置中的
目标。这既覆盖 direct factory re-entry，也覆盖间接
`A factory -> B factory -> A shutdown`，且不会拒绝无关 executor。外部 shutdown 仍会停止
准入并等待每个已经计费的 factory。若 factory 在停止后返回，则产生既有
Accepted/Cancelled submission 并精确结算；若 factory 抛出，则执行精确 rollback。只有
construction 已结算且 FIFO 已 drain，worker join 才会完成。

首条生产垂直路径是 staged HP cache save。`GraphCacheService` 仍选择 eligibility、path、
precision、codec 与错误解释。既有 live lifecycle、supersession 与 revision predicate 通过后，
graph-state policy 提交 mechanism callback，并在既有 no-throw Graph publication 前等待。
CPU compute worker 被禁止同步等待该 completion，因此阻塞的 cache codec 不会占用 CPU
execution domain。由于当前 image-codec API 不可拆分，其整个 I/O-facing call 都在 I/O worker
上运行；未来拆分后的 API 必须把独立准入的 CPU-heavy phase 送回 CPU executor。

V-15 在不改变该 mechanism 的前提下增加第二个有界使用方。Source-private OpenEXR deep
adapter 会把一次完整且不可拆分的 single-part deep-scanline read 或 write 作为 callback 提交。
在 `ComputeIoExecutor::try_submit` 前，一个共享的 source-private path check 会把 empty 或包含
embedded NUL 的 `std::string` 映射为既有 inactive `InvalidRequest` fact。它不会计入 budget、
执行 lazy construction、捕获 path/Value/registry、进入 hook 或 worker、访问 filesystem 或调用
OpenEXR；因此 C-string filename API 无法静默选择 NUL 前缀。Direct write preflight 会复用这项
contract，并抛出 Host 自有 adapter `InvalidRequest` 类别。

对于有效 path，executor admission 会接收正数 retained-byte estimate，并先于 path capture、
Value/provider generation retention、result-state construction、filesystem side effect 或
OpenEXR entry 发生。任务一旦被接受，就会在完整 codec call 期间保留 transaction token、复制后的
path、精确 provider generation，以及 input Value 或 decoded-result state。调用 OpenEXR 时使用
`numThreads=0`，因此 executor 的唯一 worker 仍是 adapter 创建的唯一 execution lane。Running
cancellation 无法抢占 foreign codec code；它会抑制延迟 result publication，并仍恰好一次释放
task/byte account。

完成通用 Value 检查后，write path 必须先跨过一个 source-private continuation barrier，才可以
准备 OpenEXR Header/frame buffer 或打开 output path。该 barrier 会验证两个有符号 window、
logical-site 与 row-width 算术、inclusive `Box2i` 坐标的精确可表示性，以及 `writePixels` 消费的
`int` scan-line count。只有完整 preflight 放行的 continuation 才能准备或打开 output。因此，
类型化 shape rejection 会逐字节保留既有 destination，也不会创建原本缺失的 destination。

这是机制边界，不是第四种 scheduler 或 persistence authority。它不新增 execution route、
ready store、Graph owner、policy decision surface、Host/device ledger dimension 或 public ABI。
同步 cache administration/load、Graph 文档 operation、daemon job state 与私有 `OutputStore`
保持不变。Executor 不拥有用户 path、retry、overwrite、receipt 或 durability policy。当前仍无
组件提供 crash-durable user-output commit，ADR 0009 中 Run publication 之后的独立 cache
outcome 仍是未来工作。

### DI-2 statistics task 所有权

`GraphCacheService` 拥有一个有界 `ImageStatisticsStore`，但该 store 不拥有 worker、ready
queue、execution route 或 policy context。其 `schedule_image_statistics()` boundary 接受一个
可信的单 task ownership receiver。发生 miss 时，callback 会独立保留精确 Ready Value 与完整
query 直至 settlement；发生 hit 时不会提交 task。receiver 可以 inline 执行 callback，也可将其
转移给既有内部 scheduler，并且必须恰好一次接收它，或者在 invocation 前抛出异常。

Cancellation 与 result publication 会在 derived-cache mutex 之前，于 request-local state 内
linearize。cancellation 先胜出时不发布 result；result 先胜出时会保留为普通有界 cache entry。
scan exception 只会 settle future。该机制不授予 Run、Graph、HP/RT generation、allocation、
formal-cache、persistence 或 worker authority，也不会改变 policy fairness 或 resource-ledger
accounting。

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

Registry mutation 与 `WorkerJoined`、`BindingRetired` record 中由 registry 派生的部分由同一个
lifecycle fence 串行化。因此，这些 physical-retirement record 携带一个精确的九 counter registry
cut，而 ready、entered-callback、root、child、policy-invocation 与 binding ownership 独立采样。
M1 evidence replay 会重建 Graph/candidate/bundle/Run/generation 因果关系，并在每个 event 与
page cut 精确校验九个 registry counter。对于六个 physical sample，它只验证 capacity 与
ownership 可达性，包括 bundle publication 前由 pending candidate 持有的 resource；绝不从
event kind 虚构精确 physical delta。最终 M1 evidence 必须以 `ServiceStopped` 结束，且全部
15 个 counter 为零。

## 当前执行画像证据与限制

内建 policy 行为本身不是执行画像 SLO。现有路径具有确定性 weighted ordering、八次
dispatch aging、3:1 class-start 上界与 Interactive headroom。长期测试会证明这些机制
和精确资源释放；ADR 0010 则把 latency、throughput、fairness、determinism、waste 与
memory 分别定义为四个不可变 workload 上的独立判定。

Issue #93 现已实现 isolated `I1-edit-storm-v1` mechanism 与 inner evidence path。
Source-private `I1Host` 会经过普通 embedded asynchronous Host、InteractionService、Kernel、
supersession 与 `ExecutionService` path 提交精确 HP request，同时提供显式 Interactive QoS、
weight one、cap eight 与每次 edit 的 immutable deadline。只读
`ComputeRunObservationSink` 会记录 current generation、带 `(RunId, RunLocalTaskId)` 与 charge
的 physically committed service start、accepted cancellation、current-visible output、terminal
outcome、Run quiescence、精确 root-resource return，以及 caller-visible future 与 Host-tracking
settlement。每个产品 transition 都会在自己的 linearization point 从同一个 request-scoped causal
sequence 预留 coordinate；尤其是逻辑 service-start commit 与 cancellation acceptance 会在任一
callback delivery 之前完成排序。该 sink 不授予 scheduling、cancellation、ledger、graph 或
commit authority，也不是 installed Host、IPC、CLI、policy-plugin 或 operation-plugin contract。
Live HP Graph swap 完成后，唯一 commit contender 会在同一次 Run-arbiter resolution 中依次
发出 current-visible output 与 terminal-success observation；被拒绝或已经解析的 contender
不会发出其中任何一个 event。

在最终 I1 Host call 之前，collector 会预留 typed accepted-row coordinate
`(A_i, event_sequence_i)`，并通过 source-private Host/Kernel request seam 传递它。Kernel 会把
这一精确 coordinate 绑定进 `SupersessionIdentity`，`ComputeRequestCoordinator` 会发布并观察
完整 current identity，而不是通过较晚 callback 重建 identity。对于已绑定 coordinate 的 I1
lineage，replacement 只要求 accepted coordinate 严格更新；timestamp 相等时由 row-local
accepted sequence 排序。Generation 保持非零且唯一，但它记录 preparation arrival，因此 coordinate
授权的 publication 可以携带更低 generation。Mixed 或 unbound traffic 仍按 generation 排序。
Native freshness manager 会采用 coordinator 精确发布的 generation，并阻止数值更高的 stale Run
observation 恢复自身。Observation sink 的 causal sequence 属于独立的 allocator 与 ordering
domain，同样从一开始。Current-generation evidence 会复制 product-bound accepted coordinate，
evaluator 要求 row 与 product 精确绑定、generation 非零且唯一，并要求 product coordinate 严格
有序。
失败的 Host call 可以为诊断保留 proposed coordinate，但不能产生 accepted row、current
observation 或 product binding。

普通 public request 与 source-private I1 request 使用同一个 embedded-Host preparation
transaction。Caller promise/future ownership、成功 result envelope、one-delivery backend
bridge、status worker 与 close-visible tracking 都会在 `InteractionService` 进入 Kernel 前建立。
Kernel 可能在返回前并发发布 current，因此 accepted Host tail 被刻意设计为 no-fail：它只会
share backend future、通过 prebuilt bridge delivery 它，并移动 prebuilt result。确定性的
source-private test seam 会在最后一个 pre-Kernel point 失败，并证明 Host resource failure 不会
暴露 current generation 或 product output；它不会改变 installed Host、IPC、CLI 或 plugin
contract。

I1 的 curve 算术已经冻结，而不是委托给 OpenCV bulk reciprocal 近似。每个
coefficient 舍入一次到 binary32 RNE，每个 sample 使用
`RNE32(1/RNE32(1+RNE32(input*k32)))`。provider 在这些显式 scalar 截断前后保存、安装并
恢复 worker 浮点环境。版本为 `i1-coordinate-pattern-curve-chain-fp32-v1` 的独立 oracle
不依赖 Host/Kernel/cache/scheduler/YAML/provider，独立重建 source 与四个 stage。对 HWC
`[2048,2048,4]` NativeScalar32 tensor、零原点 `[0,2048) x [0,2048)` 数据窗口，
以及冻结的 DenseTensor schema/Image facet 结构版本 2，其精确
`Sha256CanonicalV1` digest 是
`18d88b59782daa7ef92b0aa2acc23c7fec5e61baa5e631d9c1c4c8b6abc2eed0`。
DI-1 改变的是这些 descriptor 结构记录，而不是 `Sha256CanonicalV1` 算法或 workload
算术。I2 preview golden 是
`2af5a5b2e88646c541a60a7b437194f16d1bc2c34ff20bc571d37bfd3cac3ae2`；
34 项 B1 logical golden 由其独立 oracle 重新生成，而其 raw-payload hash 与三个
workload 标识保持不变。

冻结的 I1 graph、十二项 coefficient/Region、仅成功时产生 accepted coordinate 的 collector 与
product binding、连续 cold/warmup/measured 221-slot grid、tie/guard rule、canonical DenseTensor
output digest、resource snapshot，以及 fail-closed episode/replicate evaluator 均已成为当前实现。
`ResourceLedger` 的 Host/device snapshot 现在除 current/limit 外，还保留 successful reservation
的 lifetime high-water value。在 `Q_end`，I1 会先捕获首个被排除的 causal coordinate；必需的
terminal/quiescence/resource/Host settlement 只有在 timestamp 不晚于 boundary 且 sequence 位于
cut 之前时才属于该 history。较晚的 resource/lifecycle snapshot 可以证明最终精确归还，却不能把
settlement 回填到该 history。封闭的 `execution-profile-i1-inner-row-v1` evidence 会分别判定
latency、waste、
memory settlement 与 output correctness。它不声称实现 ADR 0010 canonical 15-field outer row、
section、bundle、reference comparison，也不覆盖 Issues #94 至 #96 负责的 profile。

expected digest 在候选执行前安装，且必须等于该 immutable oracle；缺失或被替换属于
Invalid，完整候选不匹配属于 Fail。每个 visible `Value` 在 `Q_end` 前只遍历一次，其
类型化 result 被冻结，handle 随后释放。因此 evaluation 与 JSON 无法重新计算 payload
hash。一个自有且不含 Value 的 evaluator 可以与下一 baseline preparation 重叠，但必须在
admission 前完成；JSON、dump 与 durable ordered flush 等待到 `T^I1`，或等待到撤销 later
submission 的 abort。live evidence set 受一个 evaluator 与 221 条不含 Value 的 row 限制，
exception 通过唯一 future 返回。

手工 `i1_edit_storm_benchmark` 被排除在默认 build 与 CTest 之外。它会执行精确 221-slot
workload，并把 raw inner row 与 replicate summary 写入 checkout 外显式指定的 disposable
directory。仅构建 harness 或运行 deterministic test 不构成机器符合性结果：I1 声明要求一轮
完整、有效且 cadence 精确的运行及其 retained evidence；本文不声明 Interactive、batch/render/
testbench 或 mixed-profile 符合要求。`BenchmarkService` 与
`opencv_operation_concurrency_benchmark` 保留各自更窄的 legacy metric，不是 canonical
execution-profile evidence。

目标契约有意复用合法的当前 descriptor value，而不是虚构 execution-profile enum。
I1 使用 `GlobalHighPrecision`/`Full`；I2 的 realtime request lineage 携带
`Interactive` quality 的 `RealTimeUpdate` preview child 与 `Full` quality 的
`GlobalHighPrecision` final child，二者都使用显式 Interactive QoS。当前 #94 实现通过
embedded Host、Kernel 与 ComputeService 贯通可选的 source-private progressive options。
成功且仍为 current 的 RT preview publication 会 arm 一个按取消顺序裁决的 gate，发出仅用于
观测的 final-trigger coordinate，并紧接着提交 HP child；supersession 或 cancellation 可以
deny 该 gate，而未提供该 option 的普通 realtime request 保留原先的并发行为。该 state
machine 不是 public Host、IPC、CLI 或 plugin API。
必需 logical equality 使用 `compute_content_digest(Value)` 与 typed
`Sha256CanonicalV1` `ContentDigest`，而不是 raw storage byte。

Source-private I2 profile 与 evidence evaluator 已实现冻结的 111-slot grid、十二次 edit
admission、child descriptor、publication ordering、Host acquisition、条件式真实 Metal
residency、lifecycle/resource settlement，以及四项相互独立的 inner verdict。Evaluator 要求
唯一 accepted current-generation observation 先于每个匹配的 child event；每个 Cancelled
terminal 必须恰有一个 descriptor 完全一致且更早的 cancellation，而所有非 Cancelled terminal
都不得有 cancellation。缺失、重复、迟到、多余或漂移的 evidence 会使四项 verdict 全部
Invalid。每个 edit 的 Host settlement 还必须具有严格大于每个已 materialize child resource
settlement 的 sequence，steady timestamp 也不得早于其中任何一个。其 status 当且仅当至少一个
child 已 materialize 且所有已 materialize child 均为 Succeeded 时成功：preview-only 与
preview 加 successful final 成功，preview 加 cancelled final 与 no-child 失败。Sequence、time
或 status 矛盾会使四项 axis 全部 Invalid，且不得虚构 child outcome。复制第二次 Metal reuse、
diagnostic、resource 与 no-I/O fact 后，Host 会在最终 snapshot 前执行精确 row-scoped resident
release；每个已配置 device 的完整 memory-and-scratch `reserved` vector 必须等于 row 前
baseline。Output axis 还会在 candidate 比较前独立要求 caller 提供的 expected preview 与 final
digest 分别等于 `i2_frozen_preview_content_digest()` 与
`i1_frozen_final_content_digest()`。Expected evidence 损坏时，即使 candidate evidence 与其
同步，也属于 Invalid；expected oracle 完整时，candidate-only mismatch 属于 Fail。在 replicate
层面，memory 与 output 消费全部 111 行。Latency 与 waste 只消费 measured slot `11..110`
的 sample、service 与完整 verdict；cold 和 warmup 只传播 Invalid，因此其 Pass 或 Fail value
不能污染 100 行 steady-state aggregate。

Issue #125 在不改变该 grid 或任何 verdict threshold 的前提下，闭合了 I2 capture 与手工 runner
finalization boundary。每个 episode 都会在其不可变 1.5 秒终点之前 100 ms 派生一个排他的
absolute capture deadline。Collector 会把同一个 time point 原样贯穿 `I2Host`、embedded Host
与 `ExecutionService`；在新 digest、direct Host acquisition、residency lookup/reuse 或 Metal
submission 之前，`now >= deadline` 都会失败。每次 Host ReadLease 关闭后都会 fresh sample，
第二条 record 在该 sample 严格早于 deadline 前保持为局部值。Host-only 最终 I/O snapshot 完成后，
或 conditional Metal evidence 与精确 resident cleanup 完成后，Host 会再次采样。Collector 同样把
完整 Host return 保持为局部，直到紧接调用后的 sample 通过；tie 或更晚的 sample 不提交
acquisition、不释放 Pending Value，也不建立替代 deadline。Miss 时，source-private invocation 会把同一个
absolute point 贯穿 serialized executor admission、upload planning/allocation/encoding、不超过
64 KiB 的 host-copy chunk，以及 native command-buffer commit 前立即执行的最后一次语义检查。
Exact tie 会 fail closed。Commit 前到期时不执行 native commit，RAII retirement 也不会留下
published Value、transfer admission、resident、live pending-fence owner 或 ledger lease。已经 commit
的 Metal miss 只能在原 deadline 内等待，不能重新建立 relative timeout。Wait 会在每次
`ReadyFence::poll()` 前后立即采样同一个 monotonic clock。只有 fresh post-poll sample 严格早于
deadline 时，才接受返回的 Ready、Failed 或 ProducerCancelled snapshot；sample 与 deadline
相等或更晚时进入 timeout containment。之后到期时，caller 会先尝试移除精确的
`DeviceCompletionIdentity` admission。若 native completion 已经抢先完成 Ready publication，则只
释放它的精确 resident。若 completion 已经消费 rejected/stale admission，则唯一 native callback
继续负责 terminal fence publication 及其保留的 resource lease。该 containment 不声称能够同步取消
已经 commit 的 native command，late callback 也不能重新获得已丢弃的 residency publication
authority。

Payload capture、全部 accepted settlement、Value release、history cut 与最终 execution snapshot
完成后，会封闭一个完整且不含 Value 的 input。随后恰有一个可恢复的 `std::launch::async`
evaluator 可以与下一次 baseline preparation 重叠。在一个有效的唯一 future 安装完成前，它不能
消费 input；launch 失败时，会在 caller 线程同步评估仍可恢复的 input，并继续传播 launch error。
Runner 会在下一个固定 pre-admission handoff 前收集该 future，绝不移动或回填 origin。它最多
保留一个 evaluator，以及预留存储中的 111 条完整 row。成功路径中的 JSON 构造、NDJSON write
与 flush、progress log、replicate evaluation、summary persistence 和 row compaction 只会在固定
terminal boundary 发生。Abort 会在存在时 join 唯一 evaluator，并严格按 slot 顺序只 flush 完整
row；cursor 只有在 encode、write、flush 与 stream check 全部成功后才前进，raw row 在
serialization 之前绝不 compact。

Failed 或 invalid admission 会在构造 diagnostic 前 claim 一个单调且不分配内存的 persistence
gate。它的唯一 finalizer 会关闭 Graph、捕获 history cut、消费每个有效的 accepted
settlement、不经 digest/acquisition traversal 地释放 unfrozen Value、捕获 closed state、评估一条
source-faithful 且 fixed-width 的全 Invalid row、flush 全部早期 row 与当前 row，最后才尝试写入
additive outer failure artifact。未触及的 suffix edit 保持显式，后续 slot 不得提交。Claim 后会
抑制 generic inner/outer failure handling，因此两个 artifact 都不会通过 compatibility fallback
重试。

手工
`i2_progressive_benchmark` target 为 `EXCLUDE_FROM_ALL`，不属于 CTest，并且只向调用者选择的
目录写入 `execution-profile-i2-inner-row-v1` evidence。该 inner record 不是 ADR 0010
canonical 15-field outer row、bundle 或 reference comparison。因此，仅构建它或通过
deterministic test 不构成 I2 机器符合性声明；任何此类声明都必须显式运行并保留完整精确的
111-slot workload 证据。

Issue #95 现在已经提供源码私有的 B1 composition，且不改变任何 installed surface。
`B1Host::compute_b1_image` 会把精确 Throughput QoS、weight one、所选 cap 与只读
observation sink 贯穿到普通同步 compute 所使用的同一 embedded Host、Kernel、provider、
ledger 与 `ExecutionService` path。该私有 view 还会暴露唯一真实进程
`ComputeIoExecutor` 与不带 authority 的 execution snapshot；它不会创建第二个
scheduler、worker pool、ledger、Graph authority 或 public request。

`B1OutputStore` 是 B1 manual/release output owner，而不是 ADR 0009 中仍属目标的通用
产品 `OutputStore`。它会在一个预先选择的 canonical root 下保留 no-follow root descriptor，
在整个生命周期内持有 nonblocking advisory exclusive lock，并创建 mode-`0700` private
staging anchor/slot。协作进程与线程必须遵守该 lock，并把 B1 staging/occurrence name 保留给
这个单一 owner。它会在 `openat` 前记录 named directory identity，且只有 held descriptor
精确匹配后才允许 artifact write；随后把精确
67,108,864-byte payload charge 与精确 manifest charge 作为两个有序 task 提交给进程
executor。两个 task 均结算后，store 以平台 no-replace 语义把完整 private slot 原子 rename
到不可变 public occurrence，并同步 source/destination namespace。每个 artifact mutation、
barrier 与 revalidation 都保持 descriptor-relative，因此 pathname 或 real-directory slot
replacement 不能重定向写入。在 public rename 之前，guard 会先结算 accepted work，再进行
private cleanup：两次检查已记录 leaf/directory identity，检查每次按 name 删除的结果与随后
absence，并同步 parent；若检测到 unowned residue 或 cleanup failure 则 fail-stop。POSIX 不会
原子绑定最终 identity 检查与 `unlinkat`/`rmdir`；因此该保证依赖协作式 exclusive-owner 前提，
且不对这段间隔中任意不协作 same-UID mutation 作出声明。Guard 建立前的 anchor handoff failure
会保留含义不确定的 residue，且不声称可重试。只有在该前提内完成 checked private removal 并
观察到 absence 后，相同 commit identity 才能从空 namespace 重试。Atomic public rename 会把
guard 转为 public-pending，并撤销 deletion authority。后续 barrier、最终 validation 或 receipt
failure 会保留 occurrence 与空 anchor；same-commit retry 会相对 descriptor 验证精确 payload/
manifest byte 与 identity，完成缺失 barrier，并且不产生新 output work 或 rewrite，就返回相同
receipt。非 directory，或完全没有 transaction-looking leaf 的 real directory（空目录或仅含未知
marker），属于 plainly foreign collision，会保持原状并返回 `SlotExists`。一旦出现 payload、
manifest 或 private-manifest residue，不完整/额外 entry set 或 byte/identity 漂移就会保持原状并
返回 `RevalidationFailed`。Reconciled receipt 的 `io_observations` 为空，因为没有运行新 task；
它不能伪造当前 B1 FSM，因此 evaluation 必须取得早先保留的 new-work stream，缺失时 fail closed。
Store 写入紧密 little-endian RGBA binary32
byte、同步并重验 payload 与 manifest、一次性发布、完成 leaf-to-root directory barrier，
然后才返回类型化 crash-durable receipt。该 receipt 没有公开的 field-based constructor；
只有 store 能在 revalidation 完成后签发其不可变类型化字段。Store 还可以保留一个由重复
descriptor 支撑的不透明 root-authority capability；该 descriptor 与原 descriptor 共享同一个
open-file description 和 advisory-lock 生命周期。因此，evaluated inner row 中的 capability
副本即使在原始 store 销毁后，仍会让 descriptor 与 exclusive ownership 保持存活。每次 offer 与
settlement 都保留完整 occurrence/task identity、executor 签发的精确 delta 与同锁 I/O
snapshot；capacity retry 保持 attempt zero 与相同 charge。通过 limit check 只产生 provisional
constructing reservation：factory 抛异常、返回空 callback 或 task/queue-entry allocation 失败时
会回滚，且不签发 Accepted event。Construction 成功后，Accepted 要么随 queue ownership 一起
发布，要么在外部 shutdown 已获胜时与其精确关联的 Cancelled settlement 原子发布。每个 active
snapshot task 必须精确位于一个
constructing/queued/running phase；retained global event sequence 可以因省略无关 work 而存在数值
gap，但 task-local transition 不能缺失。Planned byte 与单 task event 只对 Compute I/O
admission、high-water 与精确 task settlement 具有权威性，不能证明 physical
memory ownership、durability、RSS 或 ledger/device evidence。

Source-private B1 profile、environment validator 与 evidence evaluator 还实现不可变
34-seed logical/raw golden table、canonical semantic trace、精确 21/24/4-field environment
schema、raw backend/mount/performance proof mapping、eligibility/root-containment/
compatibility，以及四项相互独立的 inner verdict。适用 evidence 与 JSON 会保留 raw
storage proof，形式是唯一封闭的 canonical 六 field expected document，其中包含全部 21 个 raw
field observation、mount input、两次 performance cut、transaction/receipt event 与 root/
destination observation；不会保留任何 derived proof boolean。每一侧 compatibility 都会
重新解析这些 byte、重跑全部 mapping，并从自身 canonical storage byte 复算 eligibility，
再与 retained claim 精确匹配。随后，它会把该 expected claim 独立绑定到不透明的源码私有
actual capability。只有 live held-root descriptor capability、store 签发的不可变 typed receipt
与可信 live probe adapter 才能签发其输入；public aggregate、复制值与 retained proof byte 均
不能做到这一点。每次 validation call 都会从该 live observer 取得新的 root/receipt/probe
snapshot。完整 raw probe 是 observation result，而不是 minting authority；
`B1InnerRowInput`/`B1InnerRow` 中的副本会共享 observer 并延长其 source 生命周期。JSON 只增加
可读解码与 diagnostic initial-snapshot digest，不引入另一套 proof grammar，也不提供可复用
authority。任一 external storage declaration 缺少可信 observation，
都会使该侧 machine-ineligible；禁止把 retained proof 复制到 actual-observation path。
`b1_immutable_benchmark` 为
`EXCLUDE_FROM_ALL`，不属于 CTest，会在 caller 选择的 root 下执行一条精确 34-job inner row。
它的四个 environment file 是 expected input，而非 observation authority。当前 portable
Darwin/Linux path 能观察 held root 与真实 receipt，但不能独立验证全部 mount、performance、
hardware-cache、power-loss-protection 与 transaction-event fact；因此在取得可信完整 probe 前，
它会输出 Invalid，而不是 machine-conformance claim。构建该 target、显示 help 或通过
deterministic test 都不构成 B1 机器符合性结果；该 target 本身既不声明已完成精确三
replicate 机器运行，也不声明 #96 outer row/bundle/reference composition。

Source-private M1 profile 会组合精确 shared I1/B1 cadence 与五个独立 inner axis，但不会新增
installed policy 或 lifecycle control surface。其 canonical nested record 是封闭且可逆的
`execution-profile-m1-inner-row-v2`；workload 与 outer 15-field row/five-field bundle 保持
version one。v2 manifest 具有精确 20 个有序 field：`interactive_sources` 会为 48 个
phase/ordinal/origin occurrence 各自保留完整的 post-freeze Issue #93 input；
`batch_sources` 则为每个 protocol offer 保留精确一个 Issue #95 physical/output/golden/
semantic/I/O observation source。Receipt field 只作为 observation 被复制；解析绝不铸造
`B1OutputCommitReceipt` 或 live storage capability。三十个 retained progress duration 必须
各自精确等于一秒。独立 corpus reader 会精确 join source identity 与顺序，重放 I1
latency/service/四 verdict projection 与 B1 verified-endpoint/waste projection；并使用与 runner
相同的 checked 规则，从 source 推导并精确匹配全部三十个 progress window、全部三十个 Graph
A/B service/demand window、全部 480 个 measured headroom outcome 及其 attempted/classified/
failure aggregate。该 source gate 还会推导并精确匹配 first measured admission/current hold，
并在 protocol 提前返回前运行。随后它精确校验其余 mixed observation，复用 production protocol/fairness/
waste/memory/B1-I/O evaluator，重新计算全部五个轴与 overall，精确匹配六个 retained verdict，并要求逐 byte 相同
的重新物化。即使 row 已经为 `Invalid`，source closure 仍是独立的 materialization gate。
因此，未知、重复、缺失、重排、截断、非规范、被篡改、source/projection 不匹配或 verdict
过期的 nested evidence，即使 outer 层重新 hash 也会 fail closed。v2 record 省略重复的完整
I1/B1 diagnostic JSON；它与仅具有 denominator 权威的 pair pack 都不能铸造 portable output
receipt、live storage authority 或 machine conformance。`m1_shared_benchmark` 继续是
CTest/CI 之外的手工 `EXCLUDE_FROM_ALL` target，本文不声明已完成精确 timed
three-replicate M1 corpus。

对于 current-hold replay，同一个 M1 observer coordinate 横跨 measured-current publication
与被替换 warmup cancellation。Current `(B,n)` 后接 cancellation `(B,n+1)` 属于普通产品
supersession，不是 boundary-only cancellation；严格早于 B 的 cancellation，或在 B 但
sequence 不晚于 `n` 的 cancellation，会 fail closed。Accepted-row sequence 继续属于独立
domain。这项 projection 结果也独立于 Issue #93 Run validity；后者仍拒绝成功 visible
publication 与 accepted cancellation 同时存在的 Run。

Mixed observer 会在一个有界 lock-free atomic gate 内采样 steady time 并分配下一个 causal
sequence，因此 sequence 顺序蕴含 time 非递减。它接受 task-semantic start/terminal record
中从零开始的 task zero。M1 memory replay 还独立要求每个 Host component 与稳定 device
identity 满足 `reserved <= lifetime_high_water <= limit`，且 lifetime high-water 在 temporal
cut 之间非递减。Nested observation snapshot 仍为十个 field，v2 manifest 仍精确为二十个
field；这些属于语义修正，不是 schema-version 扩展。

## 实现与验证入口

`ExecutionService` 保留单一 class/ABI 边界，但实现不再集中于一个翻译单元。配置与 policy
binding 保留在 `execution_service.cpp`；Graph shutdown、admission、submission/fence、device
residency 与 worker execution 分别由对应的
`execution_service_{lifecycle,admission,submission,device,worker}.cpp` 编译。
`execution_service_state.cpp` 负责 retained value lifetime；源码私有的 run-state、ready-store 与
pool header 共享完全相同的嵌套类型，但不会形成 forwarding contract 或 installed contract。

- `include/photospider/plugin/op_contract.hpp`
- `src/lib/core/ps_types.hpp` 与 `.cpp`
- `src/lib/compute/dispatch/task_graph_planning.hpp` 与 `.cpp`
- `src/lib/compute/dispatch/compute_task_submission.hpp` 与 `.cpp`
- `include/photospider/policy/policy_plugin_api.h`
- `src/lib/policy/policy_registry.hpp` 和 `.cpp`
- `src/lib/compute/execution/execution_service.hpp` 与
  `execution_service*.cpp`
- `src/lib/compute/execution/run_lifecycle_registry.hpp` 和 `.cpp`
- `src/lib/compute/execution/execution_lifecycle_telemetry.hpp` 和 `.cpp`
- `src/lib/benchmark/i1/i1_host.hpp`
- `src/lib/benchmark/i1/i1_profile.*`
- `src/lib/benchmark/i1/i1_evidence.*`
- `src/lib/benchmark/i2/i2_host.hpp`
- `src/lib/benchmark/i2/i2_profile.*`
- `src/lib/benchmark/i2/i2_evidence.*`
- `src/lib/benchmark/b1/b1_host.hpp`
- `src/lib/benchmark/b1/b1_profile.*`
- `src/lib/benchmark/b1/b1_environment.*`
- `src/lib/benchmark/b1/b1_output_store.*`
- `src/lib/benchmark/b1/b1_evidence.*`
- `src/lib/benchmark/m1/m1_profile.*`
- `src/lib/benchmark/m1/m1_evidence.*`
- `src/lib/benchmark/m1/m1_canonical.*`
- `src/lib/benchmark/common/evidence_envelope.*`
- `src/lib/compute/execution/progressive_compute.*`
- `src/lib/core/exact_box_downsample.cpp`
- `src/lib/runtime/resource_ledger.*`
- `src/lib/execution/device/compute_io_executor.*`
- `src/lib/adapters/openexr/openexr_deep_scanline_adapter.*`
- `src/lib/execution/execution_task_runtime.hpp`
- `src/lib/execution/device/device_executor_registry.*`
- `src/lib/execution/device/metal_device_executor.{mm,stub.cpp}`
- `include/photospider/memory/ready_fence.hpp`
- `src/lib/execution/transfer/value_transfer_task.*`
- `src/lib/runtime/graph_runtime.hpp` 和 `.cpp`
- `src/lib/runtime/kernel_execution_facade.cpp`
- `src/lib/graph/graph_cache_service.*`
- `src/lib/ipc/output_store.*`
- `include/photospider/host/host.hpp`
- `src/lib/host/embedded_host.cpp`
- `src/lib/ipc/{codec,client,host,request_router}.cpp`
- `tests/unit/test_policy_registry.cpp`
- `tests/unit/test_compute_io_executor.cpp`
- `tests/integration/test_openexr_deep_scanline_provider.cpp`
- `tests/unit/test_compute_run.cpp`
- `tests/unit/test_progressive_compute.cpp`
- `tests/unit/test_i2_profile.cpp`
- `tests/unit/test_i2_evidence.cpp`
- `tests/integration/test_i2_product_path.cpp`
- `tests/verification/i2_progressive_benchmark.cpp`
- `tests/unit/test_i1_profile.cpp`
- `tests/unit/test_i1_evidence.cpp`
- `tests/integration/test_i1_product_path.cpp`
- `tests/verification/i1_edit_storm_benchmark.cpp`
- `tests/unit/test_b1_profile.cpp`
- `tests/unit/test_b1_environment.cpp`
- `tests/unit/test_b1_output_store.cpp`
- `tests/unit/test_b1_evidence.cpp`
- `tests/integration/test_b1_product_path.cpp`
- `tests/verification/b1_immutable_benchmark.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_metal_device_executor.cpp`
- `tests/integration/test_ipc_daemon.cpp`
- `tests/integration/dependency_disabled_install_smoke.py`
- `tests/integration/static_product_consumer_smoke.py`

另请参阅[计算流程](Compute-Flow.zh.md)、
[计算边界](Compute-Boundaries.zh.md)、[插件 ABI](Plugin-ABI.zh.md)和
[Graph 生命周期](Graph-Lifecycle.zh.md)。
