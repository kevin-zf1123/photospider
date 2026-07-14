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

`Kernel::load_graph()` 会在已发布 graph map 之外构造 runtime，启动配置的 scheduler，随后在
`<root>/<session>/content.yaml` 存在时加载该文件。调用方显式提供的 source file 存在时，
会先被复制到该 session path。

Graph document loading 对普通 parse/validation failure 与 session publication 是事务性的：

1. `GraphIOService` 把全部 YAML sequence entry 解析到临时 node map。
2. 在 replacement 前验证重复 id、dependency 与 cycle。
3. `GraphModel::replace_nodes()` 一起安装 node 与 topology，重置 graph runtime metadata，并推进
   topology generation。
4. 只有 load 完成后，`Kernel` 才把 runtime 插入 map。

因此，parse、node construction 或 topology failure 不会发布 partial graph 或新 session。
资源耗尽会继续抛出；其他已处理的 load failure 返回失败的 Host result。Parse 前已经发生的
directory creation 与 file-copy side effect 不会回滚。

当前 source-path boundary 有三个重要限制：

- `GraphRuntime` 构造函数执行的 directory creation 位于 Kernel 的 best-effort copy block 之外。
  Filesystem failure 会作为 I/O load failure 向上传播，且不会发布 session；但构造期间已经创建的
  directory 不会回滚；
- 随后的 session-directory setup 与 source/config copy block 会在 document load 前抑制除
  `std::bad_alloc` 外的 failure；
- 调用方提供非空且不存在的 YAML path 时，它不会替换 session-local target。已有旧
  `content.yaml` 时 Kernel 会加载旧文件；目标也不存在时才打印 warning 并发布 empty session，
  而不是返回 I/O failure。

省略 YAML path 是另一种情况：session-local `content.yaml` 存在时 Kernel 会使用它，否则有意
发布 empty session。这些情况还没有由一个冻结的 load-error matrix 统一表达。

## 现有 Session Reload

`Host::reload_graph()` 会先区分 missing session，随后通过 `GraphStateExecutor` 提交 reload。
`GraphIOService` 与 initial load 一样构造并验证临时 replacement。在 `replace_nodes()` 成功前，
visible node map、topology、topology generation、cache、timing 以及 dirty/planning state 都保持不变。

成功 reload 会替换整个图、重置 model runtime state，并且即使复用 node id 也会推进 topology
generation。`RealtimeProxyGraph` 等 runtime-owned mirror 会观察这一 generation boundary，
并丢弃陈旧的 per-node state。

## 现有 Session 保存

`Host::save_graph()` 会让 session 进入防并发 close 的 admission，要求 session map entry
存在，并通过 graph mutation 与 compute 共用的 `GraphStateExecutor` 串行化 visible node
snapshot。Missing 或 closing session 返回 `GraphErrc::NotFound`。对于 existing session，
destination access、node serialization 和 YAML emission failure 会统一归类为
`GraphErrc::Io`；destination failure 明确包括 open、write、flush 与 close。Save 会直接写入
调用方提供的 path，不使用 temporary file 加 atomic replacement。因此，destination 一旦成功
open，失败的 save 可能留下已创建、已截断或只写入部分内容的文件。

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
node-YAML replacement 与 ROI projection admission 会失败；close 则等待已接受的同步调用和
caller-visible async status publication。随后 Kernel 通过同一个 `GraphStateExecutor` 停止
runtime，并移除 map entry。

并发 close caller 通过 Host lifecycle gate 串行化。Runtime stop failure 会保留 runtime 与
diagnostic state、清除 closing marker，并重新开放 admission。只有 session 确实不存在时才返回
`NotFound`。

`photospiderd` 围绕该 embedded Host contract 拥有 daemon session identity、job admission、Host
serialization 与 shutdown drainage。其准确 mapping、lease、socket 与 shutdown 规则定义在
`../../codebase-structure/zh/IPC-Protocol-v1.zh.md`；它们不属于 graph-kernel ownership。

## 当前错误表面

| 操作 | 当前公共行为 |
| --- | --- |
| initial load，重复 session | load result 失败；embedded Host 当前分类为 `InvalidParameter` |
| initial load，runtime directory creation failure | 不发布 session；报告为 `GraphErrc::Io`；已经发生的 filesystem side effect 不回滚 |
| initial load，document parse/topology failure | 不发布 session；详细 backend category 当前会折叠成同一种 load failure |
| initial load，显式 missing source | 已有 session-local `content.yaml` 时加载旧文件；否则 warning，并发布 empty session |
| reload，missing session | `GraphErrc::NotFound` |
| reload，unreadable source 或 YAML syntax parser failure | `GraphErrc::Io` |
| reload，非 sequence 或 duplicate-id document | `GraphErrc::InvalidYaml` |
| reload，dependency/cycle validation | 对应的 backend `GraphErrc` |
| reload，未分类的 YAML conversion exception | 通过 stored last-error path 返回 `GraphErrc::Unknown` |
| save，missing 或 closing session | `GraphErrc::NotFound` |
| save，serialization、YAML emission 或 destination open/write/flush/close failure | `GraphErrc::Io`；save 不是 atomic replacement，因此 post-open failure 可能留下已创建、已截断或只有部分内容的 destination |
| node replacement，missing/closing session 或 requested node 缺失 | `GraphErrc::NotFound` |
| node replacement，existing target 的 malformed input、missing dependency 或 cycle | `GraphErrc::InvalidYaml`；previous graph state 保持 visible |
| forward/backward ROI projection，missing/closing session 或 endpoint 缺失 | `GraphErrc::NotFound` |
| forward/backward ROI projection，existing endpoint 无有效 projection | `GraphErrc::InvalidParameter` |
| clear 或 close，missing session | `GraphErrc::NotFound` |

`OperationStatus` 暴露 error domain、signed code、stable name 与 diagnostic message。调用方必须按
domain 与 code 分支，而不是按 diagnostic text 分支。上述 initial-load 不一致是当前限制，不是通用
graph-document contract。

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
- `tests/integration/test_ipc_daemon.cpp`
- `tests/integration/test_kernel_contracts.cpp`
