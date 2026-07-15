# 计算边界

本文说明当前计算子系统内部的软件行为和实现所有权。

## 范围

计算子系统接收一个通过验证的内部请求，为一个 HP domain 或协调后的 HP/RT sibling 派生工作，
执行 operation，并发布 intent-specific result。它不拥有图文档持久化、前端渲染、daemon
transport 或进程级 operation plugin
生命周期。

公共调用方只能通过 `ps::Host` 进入计算。Embedded adapter 把公共 `HostComputeRequest` 值
转换为内部 Kernel 和 `ComputeService` 请求。公共 API 不暴露 `ComputeService`、plan、任务图
或 scheduler pointer。

## 所有权图

```mermaid
flowchart TD
  HOST["ps::Host"] --> ADAPTER["embedded Host adapter"]
  ADAPTER --> KERNEL["Kernel"]
  KERNEL --> BUDGET["进程级 SchedulerWorkerBudget"]
  KERNEL --> GSE["GraphStateExecutor"]
  GSE --> SERVICE["ComputeService"]
  SERVICE --> PLAN["planning 与 pruning 协作者"]
  PLAN --> DISPATCH["ComputeTaskDispatcher"]
  DISPATCH --> RUNTIME["SchedulerTaskRuntime"]
  RUNTIME --> CALLBACK["ready TaskHandle 或 callback"]
  CALLBACK --> TEMP["临时结果"]
  TEMP --> COMMIT["通过验证的结果提交"]
  COMMIT --> GRAPH["GraphModel 或 RealtimeProxyGraph"]
```

`GraphStateExecutor` 拥有当前每图排他性。即使 ready callback 在 scheduler worker 上执行，
规划和派发仍属于 compute 职责。

当前排他机制是有界串行 FIFO lane。每个处于 accepting 状态的 `GraphStateExecutor` 恰好拥有一个
worker。其队列最多容纳 64 个等待 callback，不包括至多一个正在执行的 callback，因此每个 Graph
最多拥有 65 个已经 admission 的 graph-state callback。队列已满时，`submit()` 会阻塞 caller；它
不会创建额外 lane worker，也不会丢弃或绕过已经 admission 的 work。Admission 之前不保证 producer
fairness，但已经 admission 的 work 会按 FIFO 执行。

每次 submission 都会返回 packaged-task future，精确保留 callable 的 value、reference、`void`
completion 或 exception。销毁 future 不会等待或取消 task；executor lifetime 会保留已经 admission
的 work。Callback 不能向自己的 lane submit，也不能关闭自己的 lane：worker re-entry 会在等待队列
之前抛出 `std::logic_error`。唯一的 worker 拥有整个 callback，包括 scheduler submission、
completion wait 和 visible commit。

`close_and_drain()` 对并发调用与重复调用都保持幂等。它会停止 admission，让被满队列阻塞的
producer 以 `std::runtime_error` 被唤醒，按 FIFO 排空已有 work，并在返回前 join worker。每个
caller 都等待自己加入的持久 close generation；失败 stop 后的 restart 可以在延迟 caller 被唤醒前
重新开放后续 accepting generation，但不会困住该 caller，也不会创建第二个 worker。
`GraphRuntime` 会在 scheduler teardown 前完成该 join。如果显式 close 随后在 scheduler shutdown
中失败，Kernel 会先启动一个 replacement lane worker 再返回失败，使保留的 session 仍可重试。
不同 graph 具有独立的 worker 和队列。Scheduler-worker ledger 不计这些 lane worker；其 32-slot
上限只覆盖 scheduler planning 计费的 worker。

## 当前协作者

| 模块 | 当前职责 | 不拥有 |
| --- | --- | --- |
| `ComputeService` | 请求验证、intent 协调、协作者构造和最终结果选择 | 前端值、worker thread、图文档 |
| `ComputeCachePolicy` | HP cache eligibility 与缓存路径决定 | 磁盘 I/O 所有权或 operation 执行 |
| `NodeInputResolver` | runtime parameter 和 ready image input | 图遍历或输出提交 |
| `FullTaskGraphExpander` | 一个 graph generation/domain 的完整 node/tile task 形态 | 请求目标、cache pruning、dirty pruning |
| `NodeCacheTaskGraphPruner` | 目标/依赖锥和 cache-aware 请求 plan | 新 node 或 tile task 形态 |
| `ComputeDispatchPlanBuilder` | cache-pruned HP plan 和 inspection record | scheduler queue |
| `DirtyRegionPlanner` | 图级 dirty propagation snapshot | 计算依赖计数 |
| `DirtySnapshotTaskGraphPruner` | 从既有 plan 选择活动 dirty work | task expansion |
| `IntentUpdateCoordinator` | HP-only 或 HP/RT sibling 语义 | 物理优先级或 worker 所有权 |
| `ComputeTaskDispatcher` | Dependency counter、ready release、temporary result、completion、exception、full HP commit 与 dirty source-first submission helper | Graph topology derivation、dirty staged commit 或 scheduler policy |
| `TaskSubmissionPlan` | 请求级 task handle、dense index、依赖状态、variant 和结果槽 | 超出当前 dispatch contract 的生命周期 |
| `NodeExecutor` | 一致的 monolithic/tiled operation 调用 | 图变更策略 |
| `ComputeMetricsRecorder` | compute event、timing、benchmark event 和 debug metadata | scheduler trace 所有权 |
| `SchedulerFactory` | 在构造前解析 `0..8` worker 请求，并规划每个 scheduler 的保守 slot 计费 | 进程容量所有权或 graph-state access |
| `SchedulerWorkerBudget` | 在所有 embedded Host/Kernel 间串行化固定 32-slot 进程 admission ledger | worker 构造、scheduling policy、fairness 或整个进程 thread 计数 |
| `ReservationOwnedScheduler` | 让 move-only reservation 保持到 concrete scheduler shutdown 与销毁完成 | 容量 planning 或 task-graph correctness |

Compute collaborator 位于 `src/lib/compute/`；三个 admission/ownership collaborator 位于
`src/lib/scheduler/`。这些类都是私有实现模块，不构成可安装 API。

## 请求行为

1. `Kernel` 解析 session 并进入图状态访问边界。
2. `ComputeService` 验证 target、intent、dirty ROI、cache flag 和 execution strategy。
3. 在 extent、ROI 或 task-shape 决定使用连接参数之前，parameter producer 会稳定为一个
   request-local HP snapshot。
4. Planner 展开一个 domain 的完整 task 形态，再裁剪到请求目标和依赖锥。
5. Dirty request 从该 plan 选择活动 work set；dirty 状态不会创建新的 task 形态。
6. 顺序执行 inline 遍历同一请求语义；并行执行 materialize 具体 handle，只把 ready handle 或
   callback 提交给选定 scheduler runtime。
7. Worker 写入请求级临时或 staged output；只有相应 commit path 能修改可见图状态。
8. 结果、事件、计时和错误通过 Host value 边界复制返回。

## 规划不变量

- Full expansion 以 graph topology generation、compute intent 和 task-shape configuration 为键。
- 当当前 input/parameter 可能在拓扑不变时改变 output extent，force-recache 会使可复用 expansion
  失效。
- 请求目标、cache availability 和 dirty 状态裁剪既有 task 形态，不会重定义图拓扑。
- 只要仍有由 `ComputeTaskGraph` 派生的 scheduler-visible callback 可能执行，该图就不可变。
- HP 与 RT 是独立 compute domain；一个 plan 不创建跨 domain task 依赖。
- 在可行时，tiled input normalization 每次 node invocation 只执行一次，而不是每个 tile callback
  执行一次。

这些规则使规划保持确定性，并让 scheduler 独立于图语义。因此，规划成本遵循先 full
expansion、再 pruning；lazy task creation 不属于当前 planning contract。

## Dispatcher 与 Scheduler 边界

Dispatcher 拥有请求正确性：

- dependency counter 和 dependent map；
- source-first dirty task release；
- task reference accounting；
- 临时结果槽；
- exception normalization 和 completion aggregation；
- 空 plan 验证；
- 最终 target 选择与 full HP commit；dirty executor 在复用 source-first submission helper 后拥有
  自己的 staged commit。

Scheduler 拥有当前物理执行机制：

- worker lifecycle 和 ready queue；
- batch state 与 scheduler-local epoch filtering；
- 实现特定 task ordering；
- scheduler completion 和 exception publication；
- 通过 Host context 发布有界 trace。

Scheduler 不会收到 `GraphModel`、`ComputeTaskGraph`、`DirtyRegionSnapshot` 或 cache authority。
新就绪的 dependent work 由 dispatcher 释放，并作为另一条 ready handle 或 callback 推送。Threaded
scheduler resource 按 `GraphRuntime`、按 intent route 拥有；当前不存在 process-wide worker pool
或 cross-graph fairness authority，但存在 process-wide admission authority：graph load 原子预留
HP+RT 合计计费，replacement 则在旧 owner 保持存活时预留一个 candidate 计费。内置 serial
scheduler 计费为零；内置 CPU 与已注册 ABI v2 plugin 按解析后的一到八授权计费；内置
GPU/heterogeneous 还要计入潜在 device worker。

## OpenCV Operation 并发

仓库自有 CPU OpenCV operation 是可重入的 provider 工作。Builtin provider 不再具有进程范围的
operation mutex。其 monolithic `convolve`、`resize`、`crop`、`extract_channel`、
`gaussian_blur`、`add_weighted`、`abs_diff` 与 `multiply` callback，以及 tiled
`curve_transform`、`gaussian_blur`、`add_weighted`、`abs_diff` 与 `multiply`，可以跨 tile、
Graph 和 HP/RT intent route 并发运行。Callback input 不可变；可变 `cv::Mat` header、temporary
与 output region 由 callback 局部拥有或 task 独占。

Registry 边界遵循同一规则。Registry lock 会串行化 ownership mutation、发布、一致 snapshot
capture 与卸载，但会在 callback invocation 前释放。因此，每个 provider 都必须保证 callback
可重入，或自行同步其共享可变状态。共享 operation key、device、intent 或 callback owner 绝不
意味着单线程执行。

`register_builtin()` 会在发布 builtin callback 前恰好一次调用 `cv::setNumThreads(1)`。仓库自有
CPU provider 使用 `cv::Mat`；仓库代码不调用 `cv::ocl::setUseOpenCL(false)`，也不会在 callback
可能活跃时重新配置 OpenCV threading。因此，已准入的 scheduler worker grant 是仓库自有的外层
CPU parallelism，而 OpenCV 内部 CPU parallelism 保持禁用。

围绕真实 backend state 的同步仍由 provider 局部负责。Metal Perlin provider 保留一个
DSO-private mutex，保护其共享 Metal device、queue、pipeline 与 buffer；该 mutex 既不是 OpenCV
operation lock，也不是 scheduler exclusivity contract。仓库自有 provider 之外的 OpenCV 使用、
第三方内部 thread 与 platform runtime worker 仍不计入 scheduler worker accounting。

[ADR 0004](../../adr/zh/0004-opencv-cpu-operations-are-reentrant-provider-work.zh.md)记录本项决策。
长期 integration coverage 会证明 `1/2/4/8` grant 对应精确 callback overlap，以及单 worker 与
八 worker 输出按位相同；手工原生扩展性证据记录在
`../../development/zh/Testing-and-Validation.zh.md`。ADR 0002 仍把未来 OpenCV algorithm、codec、
exception translation 与 process state 放入可选 provider/adapter，而不是 kernel 语义。

## Intent 与提交边界

`GlobalHighPrecision` 和 `RealTimeUpdate` 描述业务语义，而不是资源策略。Real-time update
协调一个 RT proxy sibling 和一个 HP authoritative sibling；每个 sibling 都有自己的 domain plan、
dirty snapshot、staged output 和 scheduler selection。

`IntentUpdateCoordinator` 通过两个 asynchronous call 建立当前 sibling concurrency。选中的
scheduler 只执行每个 sibling 内部的 ready work；它不会创建 sibling relationship，也不会从 task
metadata 推导该关系。

当前普通 compute policy 会持有每图独占访问直到可见提交。Dirty path 已使用更窄的 staged buffer：

- `RealtimeProxyWriteBuffer` 只提交到 `RealtimeProxyGraph`；
- `HighPrecisionDirtyWriteBuffer` 在 sibling commit gate 打开后，把权威 HP output 提交到
  `GraphModel`。

该 staging 会防止尚未组装完成的 tile output 可见，但它还不是通用 cancellation 或 graph revision
策略。

## 故障与生命周期语义

- 非法 target、intent/ROI 组合、planning contract 和 operation failure 通过分类图错误和 Host
  status value 报告。
- 资源耗尽可以按已记录的非析构 Host 边界传播为 `std::bad_alloc`。
- 超过八的 worker 请求或未知 scheduler type 会在 worker 构造前作为 `InvalidParameter` 失败；graph
  load 或 replacement 的进程 ledger 耗尽会保留 `GraphErrc::ComputeError`。
- Scheduler reservation 在 teardown 期间比 concrete worker 活得更久：candidate rollback 只归还
  candidate 容量，成功的 graph close 或 Host 销毁恰好一次归还 retained capacity，close 失败则
  保留容量供重试；replacement 在失败时保留旧 scheduler，因此需要 transient headroom。
- 已 admission 的 scheduler batch 会在异常离开当前请求前 settle。
- Operation callback 可能已经产生外部副作用；staged graph output 不会回滚这些副作用。
- 当前 task handle 借用请求级 executor 状态，其生命周期在当前 completion wait 结束；因此不能
  原样移入进程级异步队列。

## 边界原理

把 planning、ready detection、physical execution 和 commit 分离，会得到四个独立正确性点：

1. 无需 worker pool 即可测试 Graph 与 ROI 语义。
2. Scheduler 可以改变 ordering，而不拥有 Graph 状态。
3. 临时输出可以在可见前验证。
4. 物理执行所有权与 dependency correctness 保持可分离。

ADR 0003 记录了供后续实现的另一项已接受 ownership decision。本文是当前 per-graph scheduler
ownership 及其有界进程 admission containment 的权威说明；该 ledger 不是目标 shared
`ExecutionService`。

## 实现与验证入口

- `src/lib/compute/compute_service.*`
- `src/lib/compute/task_graph_planning.*`
- `src/lib/compute/compute_dispatch_plan_builder.*`
- `src/lib/compute/compute_task_submission.*`
- `src/lib/compute/compute_task_dispatcher.*`
- `src/lib/compute/dirty_region_planner.*`
- `src/lib/compute/dirty_update_executor.*`
- `src/lib/compute/intent_update_coordinator.*`
- `src/lib/core/ops.cpp`
- `src/lib/scheduler/scheduler_factory.*`
- `src/lib/scheduler/scheduler_worker_budget.*`
- `src/lib/scheduler/scheduler_reservation_owner.*`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_scheduler.cpp`
- `tests/integration/test_scheduler_worker_budget.cpp`
- `tests/unit/test_scheduler_factory_plan.cpp`
- `tests/unit/test_scheduler_reservation_owner.cpp`
- `tests/unit/test_scheduler_worker_budget.cpp`
- `tests/unit/test_propagation_contracts.cpp`
