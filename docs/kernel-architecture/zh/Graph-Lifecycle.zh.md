# 图生命周期与变更语义

本文描述图 session 当前的所有权、发布、变更和失败行为。文中会如实记录已实现行为及已知边界；
拟议的持久化抽象属于内核演进路线图。

## 所有权

`Kernel` 拥有从图名称到 `GraphRuntime` 实例的映射。每个 runtime 拥有一个 `GraphModel`、
一个 `GraphStateExecutor`、事件和 scheduler 状态，以及平台运行时资源。

```text
Kernel
  graph name -> GraphRuntime
                  -> GraphStateExecutor
                  -> GraphModel
```

公共 `ps::Host` 返回复制的 `GraphSessionId` 值。Session id 是标签，不是 graph 或 runtime
handle。Graph-state mutation 与 visible compute 会进入同一个 per-graph exclusive access boundary。

## 新 Session 加载

`Kernel::load_graph()` 会在已发布 graph map 之外构造 runtime，启动配置的 scheduler，校验
完整 graph document，最后才插入 runtime。Source 选择会区分省略与显式请求：

- 空 `yaml_path` 会在 `<root>/<session>/content.yaml` 存在时使用该文件，否则有意创建
  empty session；
- 每个非空 `yaml_path` 都是显式路径。Kernel 要求它存在并能复制到 session path，除非它已经
  指向同一文件。显式 source failure 返回 `GraphErrc::Io`，绝不回退到旧 session content。

在构造该 runtime 前，Kernel 会规划两个已配置的 intent scheduler。Worker 请求只有零到八有效；
零解析为 `min(max(1, hardware_concurrency()), 8)`，显式正值保持精确。随后 Kernel 从进程级
32-slot `SchedulerWorkerBudget` 原子预留 HP+RT 合计计费。内置 `serial_debug` 计费为零，内置 CPU
与 ABI v2 plugin scheduler 按解析后的授权计费，内置 GPU/heterogeneous scheduler 还要计入已配置的
潜在 GPU worker。如果 planning 或合并准入失败，两个 scheduler 都不会被构造，也不会发布 session。

Graph document loading 是“准备后发布”事务：

1. `GraphIOService` 把全部 YAML sequence entry 解析到临时 node map。
2. 在 replacement 前分类 parser、root-shape、duplicate-id 与 node-schema failure。
3. 验证 dependency 与 cycle，并构造 replacement adjacency。
4. `GraphModel::replace_nodes()` 一起安装 node 与 topology，重置 graph runtime metadata，并推进
   topology generation。
5. 只有 load 完成后，`Kernel` 才把 runtime 插入 map。

因此，path、parser、node construction、topology、unexpected 或 resource failure 都不会发布
partial graph 或新 session。`std::bad_alloc` 原样传播；其他 failure 使用下文稳定矩阵。
Parse 前已经发生的 directory creation 与 file-copy side effect 不会回滚，但它们不是 graph-map
publication。Scheduler 容量更严格：发布前发生任何失败，都会销毁所有 candidate scheduler，
并把原子准入的两个 reservation 各自恰好归还一次。

分类与事务的依据由
[ADR 0005](../../adr/zh/0005-graph-document-ingestion-is-a-classified-transaction.zh.md)
固定。

## 现有 Session Reload

`Host::reload_graph()` 会先区分 missing session，随后通过 `GraphStateExecutor` 提交 reload。
即使 path 为空，missing session 也返回 `NotFound`；已有 session 的空 path 返回
`InvalidParameter`。每个非空 source 都与 initial load 使用相同的 IO、syntax/schema、topology、
unexpected-failure 与 resource-exhaustion 分类。

`GraphIOService` 与 initial load 一样构造并验证临时 replacement。在 `replace_nodes()` 成功前，
visible node map、topology、topology generation、cache、timing、dirty/planning state、runtime
state 与 session identity 都保持不变。这项保证同时覆盖 handled failure 和传播的
`std::bad_alloc`。

成功 reload 会替换整个图、重置 model runtime state，并且即使复用 node id 也会推进 topology
generation。`RealtimeProxyGraph` 等 runtime-owned mirror 会观察这一 generation boundary，
并丢弃陈旧的 per-node state。

## Scheduler Replacement

Scheduler replacement 通过 per-graph `GraphStateExecutor` 与 compute、inspection 和 close 串行化。
Kernel 会先验证并规划一个 candidate，再在旧 scheduler 与 reservation 保持存活时预留完整 candidate
计费。它在发布新 owner 前构造、attach 并 start candidate；只有发布后，被替换 owner 的销毁才会
归还其 slot。因此 replacement 需要 transient process headroom，绝不会推测性借用旧 reservation。

容量不可用时，replacement 会在 candidate 构造前返回 `GraphErrc::ComputeError`。Candidate 构造、
attach 或 start 失败时，只归还 candidate reservation，旧 scheduler 继续服务 compute。未知 type 与
无效 worker 请求仍为 `InvalidParameter`。成功 replacement 会把一个 move-only reservation 转移到
`ReservationOwnedScheduler`，其销毁顺序保证先拆除 concrete scheduler，再释放 slot。

## 现有 Session 保存

`Host::save_graph()` 会让 session 进入防并发 close 的 admission，要求 session map entry
存在，并通过 graph mutation 与 compute 共用的 `GraphStateExecutor` 串行化 visible node
snapshot。Missing 或 closing session 返回 `GraphErrc::NotFound`，并在 destination access 前
停止。对于 existing session，可恢复的 node serialization、YAML emission，以及 destination
preparation/open/write/flush/close failure 会统一归类为 `GraphErrc::Io`。资源耗尽保持精确的
`std::bad_alloc` exception channel，不会转成 `Io` status。

Save 是 owner-state read transaction。成功、返回的 failure 与传播的资源耗尽都会让 graph
topology、topology generation、cache/timing/dirty/planning/runtime state 和 session identity
保持不变。调用方可在任何已报告的失败后对同一个已准入 session 重试；IPC client 对每次 mutation
只发送一次，不会自动重试。

Destination 只有刻意收窄的保证。Save 会直接写入调用方提供的 path，不使用 temporary file 加
atomic replacement。Destination 成功 open 前发生的失败会保留 existing bytes；open 成功后，
write、flush、close 或之后的资源失败可能留下已创建、已截断或只写入部分内容的 destination。
因此，destination rollback 不属于 graph-owner transaction。

## Node Replacement 与结构编辑

`Host::set_node_yaml()` 会让 session 进入防并发 close 的 admission。Required node lookup、
candidate parsing、强制 replacement-id assignment 与 `GraphModel::replace_node()` 会在同一个
graph-state work item 内执行，因此 clear 或 reload 无法插入 lookup 与 mutation 之间。Missing
或 closing session，以及 requested node 缺失，都会返回 `GraphErrc::NotFound`。对于 existing
target，parsing 与完整 candidate-topology validation failure 返回
`GraphErrc::InvalidYaml`。

Replacement 会复制当前 node map、验证完整 candidate topology，最后才交换到 visible state。
Parse、missing-dependency 或 cycle validation failure 会保留之前的 node map 与 topology。当前
implementation 不承诺 all-exception strong guarantee：重建已验证 topology 时的 allocation
failure 可能发生在 candidate node map 已被 move 进 model 之后。

`add_node()`、`remove_node()` 和 input-rewire 方法采用同一种 candidate-map 模式。成功的结构编辑
会重建 adjacency index、推进 topology generation，并清除 cached full task graph。

## ROI 投影

`Host::project_roi()` 与 `Host::project_roi_backward()` 会让 session 进入防 close 的
admission，并在同一个 graph-state work item 内完成两个 endpoint 的 lookup 与 propagation。
Missing 或 closing session，以及缺失的 source/target node，都会返回
`GraphErrc::NotFound`。当两个 endpoint 均存在，但 ROI 为空、路径不可达或 propagation
无法产生有效 rectangle 时，Host 返回 `GraphErrc::InvalidParameter`。

Existing-session propagation exception 仍会更新 Kernel 的 best-effort `LastError` mirror，但当前
Host result 直接来自同一个 required operation；它绝不会在 operation 结束后通过读取共享
diagnostic state 重建。

## Clear

`GraphModel::clear()` 执行 model reset，而不只是删除 node。它会清除：

- node 与 topology adjacency；
- timing 与 accumulated I/O state；
- dirty snapshot、generation 与 source commit state；
- compute-plan history 与 full-task-graph cache；
- disk-cache diagnostics 与 skip-save state。

Clear 会推进 topology generation，并让 model 保持 quiet mode。它不会关闭或销毁所属
`GraphRuntime`；session 仍处于 loaded 状态。它也不会删除 disk-cache file、清除 runtime-owned
event/trace ring、直接清除 `RealtimeProxyGraph`，或清除 Kernel-owned `LastError`。
`RealtimeProxyGraph` 会在下一次 synchronization 观察到已推进的 topology generation 时自行失效。

## Close 与 Lifetime

Embedded Host close 会先把 session 标记为 closing。新的 compute、scheduler、required save、
node-YAML replacement 与 ROI projection admission 会失败。Host 会在 lane 仍 accepting 时等待该
marker 之前已准入的同步调用，让这些调用完成 graph-state submission。随后 Kernel 会停止这些调用
共用的 `GraphStateExecutor` admission。因 64-entry FIFO 已满而阻塞的 producer 无需等待队列空位就
会被唤醒并拒绝；只有此后 Host 才等待 async submission placeholder 与 caller-visible status
publication。已经 admission 的 callback 按 FIFO 顺序排空，Kernel 随后 join 唯一的 worker。只有
跨过该 joined boundary 之后才开始 scheduler stop；只有 stop 成功后才移除 map entry。

并发 close caller 通过 Host lifecycle gate 串行化。在 executor 内，每个 closer 会记录自己加入的
close generation，并等待该 generation 被持久发布为已经 join。Runtime stop failure 会保留
runtime、diagnostic state 与仍存活的 scheduler reservation。由于之前的 lane worker 已经 join，
Kernel 会在返回 stop failure 前创建一个 replacement worker；随后 Host 清除 closing marker，并
重新开放 admission。即使该 restart 早于上一 generation 的延迟 waiter 被唤醒，这些 waiter 仍会
返回，也不会创建第二个 worker。之后再次 close 时，会先排空并 join 该 replacement lane，再重试
scheduler stop。Close 成功时，concrete scheduler 会先 shutdown 并销毁，随后才归还 slot。
Embedded Host 未显式 close 就销毁时，也会走相同的同步 ownership chain：`GraphRuntime` 会在
scheduler teardown 之前排空并 join lane，并在 Host 析构完成前归还所有 graph reservation。只有
session 确实不存在时才返回 `NotFound`。

`photospiderd` 围绕该 embedded Host contract 拥有 daemon session identity、job admission、Host
serialization 与 shutdown drainage。其准确 mapping、lease、socket 与 shutdown 规则定义在
`../../codebase-structure/zh/IPC-Protocol-v1.zh.md`；它们不属于 graph-kernel ownership。

## 当前错误表面

| 操作 | 当前公共行为 |
| --- | --- |
| initial load，重复 session | `GraphErrc::InvalidParameter`；existing session 保持不变 |
| scheduler default 或直接 planning，worker 请求超过八 | `GraphErrc::InvalidParameter`；不构造 scheduler，未来默认值保持不变 |
| initial load，HP+RT 合并进程容量不可用 | 不发布 session 或 scheduler；精确返回 `GraphErrc::ComputeError` |
| initial load，空 path | session-local `content.yaml` 存在时加载该文件；否则有意发布 empty session |
| initial load，显式 missing/unreadable/uncopyable source 或 session-path failure | `GraphErrc::Io`；不发布 session、不回退；已经创建的 filesystem scratch side effect 不回滚 |
| initial load，YAML syntax/representation、非 sequence root、duplicate id 或 node-schema failure | `GraphErrc::InvalidYaml`；不发布 session |
| initial load，missing dependency 或 cycle | 精确的 `GraphErrc::MissingDependency` 或 `GraphErrc::Cycle`；不发布 session |
| initial load，unexpected non-resource failure | `GraphErrc::Unknown`；不发布 session |
| initial load，resource exhaustion | 传播 `std::bad_alloc`；不发布 session |
| reload，missing session | `GraphErrc::NotFound` |
| reload，已有 session 且 path 为空 | `GraphErrc::InvalidParameter`；先前 graph 与 runtime state 保持 visible |
| reload，missing/unreadable source | `GraphErrc::Io`；先前 graph 与 runtime state 保持 visible |
| reload，YAML syntax/representation、非 sequence root、duplicate id 或 node-schema failure | `GraphErrc::InvalidYaml`；先前 graph 与 runtime state 保持 visible |
| reload，missing dependency 或 cycle | 精确的 `GraphErrc::MissingDependency` 或 `GraphErrc::Cycle`；先前 graph 与 runtime state 保持 visible |
| reload，unexpected non-resource failure | `GraphErrc::Unknown`；先前 graph 与 runtime state 保持 visible |
| reload，resource exhaustion | 传播 `std::bad_alloc`；先前 graph 与 runtime state 保持 visible |
| save，missing 或 closing session | `GraphErrc::NotFound` |
| save，existing session 出现可恢复的 serialization、YAML emission 或 destination preparation/open/write/flush/close failure | `GraphErrc::Io`；graph/runtime/session-owner state 保持不变；成功 open 前的失败保留 existing destination bytes，post-open failure 可能留下已创建、已截断或只有部分内容的 output |
| save，existing session 出现资源耗尽 | 传播 `std::bad_alloc`；graph/runtime/session-owner state 保持不变；destination effect 遵循相同的 pre-open 与 post-open 边界 |
| node replacement，missing/closing session 或 requested node 缺失 | `GraphErrc::NotFound` |
| node replacement，existing target 的 malformed input、missing dependency 或 cycle | `GraphErrc::InvalidYaml`；previous graph state 保持 visible |
| forward/backward ROI projection，missing/closing session 或 endpoint 缺失 | `GraphErrc::NotFound` |
| forward/backward ROI projection，existing endpoint 无有效 projection | `GraphErrc::InvalidParameter` |
| scheduler replacement，未知 type 或无效请求 | `GraphErrc::InvalidParameter`；旧 scheduler 保持发布 |
| scheduler replacement，transient process capacity 不可用 | `GraphErrc::ComputeError`；不构造 candidate，旧 compute 行为仍可用 |
| clear 或 close，missing session | `GraphErrc::NotFound` |

`OperationStatus` 暴露 error domain、signed code、stable name 与 diagnostic message。调用方必须按
domain 与 code 分支，而不是按 diagnostic text 分支。IPC 会序列化该精确 status，并在 Host load
失败时回滚其 reserved session name；它不会引入 transport-only graph-document taxonomy。

## 实现与验证入口

- `src/lib/runtime/kernel.cpp`
- `src/lib/runtime/kernel_io_cache_facade.cpp`
- `src/lib/runtime/kernel_inspection_facade.cpp`
- `src/lib/runtime/kernel_dirty_roi_facade.cpp`
- `src/lib/graph/graph_io_service.cpp`
- `src/lib/graph/graph_state_executor.cpp`
- `src/lib/graph/graph_model.cpp`
- `src/lib/host/embedded_host.cpp`
- `tests/integration/test_host_adapter.cpp`
- `tests/integration/test_graph_document_errors.cpp`
- `tests/integration/test_ipc_daemon.cpp`
- `tests/unit/test_ipc_protocol.cpp`
- `tests/integration/test_kernel_contracts.cpp`
