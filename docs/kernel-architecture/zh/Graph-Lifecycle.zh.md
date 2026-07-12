# 图生命周期与变更语义

本文档定义图/运行时所有权，以及图加载、reload、编辑和 clear 操作的失败行为。

## 运行时所有权

`Kernel` 拥有从图名称到 `GraphRuntime` 实例的映射。每个 `GraphRuntime` 只拥有一个 `GraphModel`、graph-state executor、事件服务、调度器映射和平台 context。

```text
Kernel
  graph name -> GraphRuntime
                  -> GraphModel
```

图和运行时应被视为一对一的所有权单元。

对于 embedded Host concurrency，close admission gate 会在 backend removal 之前把一个 session
标记为 closing。新的 compute 与 scheduler admission 会失败；close 则等待已接受的同步调用和
caller-visible async status publication。随后 Kernel 会通过 compute、scheduler information/
replacement 共用的 per-graph `GraphStateExecutor` 提交 runtime stop，再删除 map entry。该顺序
保证每个已接受 operation 完成之前，runtime 及其 scheduler owner 都保持存活。
并发 close caller 会逐个独占 close marker：先前 attempt 完成后，每个 waiter 都重新检查，
再执行自己的存在性/关闭尝试。runtime stop failure 会保留 runtime 与 diagnostic state、清除
marker 并重新开放 admission，因此仍可 inspect 或稍后重试 close；只有 map entry 确实不存在
时才返回 `NotFound`。

## Daemon-Owned Session Identity

`photospiderd` 拥有一个 embedded `ps::Host`；client 从不拥有其 `GraphRuntime` lifetime。
Version 1 router 会在 `GraphLoadRequest.session` 中保留 caller 的 safe `session_name`，因此既有
`<root>/<session>` 与 cache-directory 语义不变。它返回另一个 128-bit opaque IPC session id，
并在 private bidirectional mapping 中保存 Host 返回的精确 `GraphSessionId`。

Load 在 registry 与 Host boundary 之间保持 transactional：先 reserve opaque id，再调用 Host，
仅在 Host success 后发布 opaque、精确 Host-id 与 display-name 三个 index。Host exception 会
删除 reservation；publication failure 会先删除 reservation，再 best-effort 补偿性关闭 Host
session。Host 契约要求 load 抛出异常时不留下新发布的 session。Embedded
Kernel/Host 通过在发布前预分配 result identity，再使用 `noexcept` move 提交，
来满足该契约。`graph.list` 会把 committed mapping 与 `Host::list_graphs()`
reconcile；发现差异时返回 invariant error，而不是暴露 untracked Host name。
Client disconnect 从不调用 `close_graph`；另一个 client 仍可 list/inspect
daemon-owned session。

`graph.close` 会使用 compute admission 共用的 daemon lifecycle gate。它会原子地把 mapping
标记为 closing，以 Graph `not_found` 拒绝每个新的 session-scoped Host/compute admission，并
等待该 session 已 admitted 的 Host call 与全部 queued/running job。已接受 job 的
status/result/release 仍可使用，因为它们是 job-scoped，且不进入 Host。只有这些计数归零后，
daemon 才获取 Host mutex 并调用 `Host::close_graph()`。Success 会移除 mapping；Host
`NotFound` 会在保留 failure 的同时移除 stale mapping；其他 Host failure 会原子地重新开放
admission 并保留 mapping。

Public Host contract 不承诺 thread safety，因此 daemon 使用一个 dedicated mutex 包围每个 Host
call，包括 read-only list 与 inspection。Protocol validation 以及 `daemon.ping`/
`daemon.version` 不获取该 mutex；socket IO 期间绝不持有它。Signal shutdown 会停止 accept、
停止 session/compute/snapshot admission 与新 output lease，关闭 listener，唤醒并 join client
worker，drain 已接受 job，并 join 唯一 compute worker。随后它会移除 terminal job ownership、
清空 stable collection snapshot、停止 output publication、等待 active delivery lease release 或
expire，并在尝试关闭 active Host session 之前 identity-clean/close OutputStore。只有 registry 与
Host cleanup 完成后，它才会在 persistent lifecycle lock 仍持有时移除精确 socket，释放该 lock，
最后 destroy Host state。Lock file 会保留以提供稳定的跨进程同步。完整 wire/socket contract 维护在
`docs/codebase-structure/zh/IPC-Protocol-v1.zh.md`。

## 新图加载

加载新图应创建新的 `GraphRuntime`，并且只有 YAML 验证成功后才通过 `Kernel` 暴露它。

如果 YAML 中任意节点非法、缺少必需字段或产生环，加载必须返回错误，并且不得暴露部分加载的图。

期望行为：

```text
parse YAML -> validate all nodes/topology -> rebuild adjacency -> create/commit runtime
                                      \-> on failure: return error, expose none
```

在失败节点之前部分保留有效节点不是期望行为。

## 现有图 Reload

Reload 现有图更敏感，因为它操作的图名称可能已经可见。目标方向是在 reload 失败时避免半清空或部分重建的模型状态。

选择的行为：失败的 reload 保留之前的图。Reload 会在提交到可见 `GraphModel` 之前验证替换模型并重建拓扑邻接。成功 replacement 即使复用 node id，也会推进 topology generation，使 `RealtimeProxyGraph` 等 runtime-owned mirror 在下次 compute 前重置陈旧的 per-node state。

## 节点 YAML 替换

节点 YAML 替换应在验证失败时保留旧节点和图。

至少，替换必须解析新节点，并在解析或字段验证失败时保留旧节点。拓扑验证也应在提交前发生，使替换不能引入环或断裂依赖。

替换会在提交前验证候选拓扑。如果解析、依赖验证或环验证失败，之前的节点和图保持可见。

## GraphModel Clear

`GraphModel::clear()` 应重置模型级运行时状态，而不只是删除 `nodes`。

Clear 应重置：

- 节点映射
- 拓扑邻接索引
- topology generation
- 计时结果
- 累计 IO 时间
- skip-save 状态
- 其他可能影响后续加载或计算的单次运行模型状态

这让 reload 和 clear 行为更容易推理，并避免陈旧元数据附着在空图上。按 node id keyed 的 runtime-owned state 必须把 generation change 视为 invalidation boundary，而不是为复用 id 保留 entry。

## 错误表面

图加载、reload 和编辑失败通过 public `ps::Host` status/error value 对 frontend 可见。在
embedded 模式下，Host adapter 会把内部 `Kernel` 与 `InteractionService` 的失败诊断映射到该
public surface。Frontend 既不会调用这些内部 facade，也不需要从部分变化的图中推断失败。

在 daemon mode 下，同一 `OperationStatus` 会保留完整 domain 的 `OperationErrorDomain`、
signed code、stable name 与 diagnostic message。Host failure 使用 `graph` domain 与显式
`GraphErrc` number/name pair。Framing、envelope 与 parameter error 保持在 `protocol` domain；
本地 socket failure 保持为 `OperationErrorDomain::Transport`。Diagnostic text 不是 branching
contract，transport failure 也不会被改写为 graph IO。
