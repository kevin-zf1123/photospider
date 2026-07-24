# 图生命周期与变更语义

本文描述图 session 当前的所有权、发布、变更和失败行为。文中会如实记录已实现行为及已知边界。
内存 GraphDefinition seam、注入的 YAML document/cache adapter，以及 dependency-disabled
unavailable persistence adapter 都是当前行为。

## 所有权

`Kernel` 拥有一张由 mutex 保护的 map，从 graph name 指向共享的 `GraphRuntime` lifetime
root。每个 runtime 拥有一个 `GraphModel`、一个 graph-state executor、一条私有
compute-request executor、一个 latest-wins request coordinator、复制的 HP/RT
execution-route binding、event/execution-trace state 与 platform runtime resource。Map
lookup 会在短临界区内复制一个 shared runtime owner；释放 map lock 后才执行 graph-state、
compute、IO、inspection 与 lane work。因此，并发 close 可以移除 name，但不会销毁已由准入内部
调用保留的 runtime。

```text
Kernel
  graph-registry mutex
    -> graph name -> shared GraphRuntime root
                  -> GraphStateExecutor
                  -> compute-request lane
                  -> ComputeRequestCoordinator
                  -> GraphModel
```

公共 `ps::Host` 返回复制的 `GraphSessionId` 值。Session id 是标签，不是 graph 或 runtime
handle。每个 live `GraphModel` 还拥有私有强类型、不可复用的 `GraphInstanceId` 与 checked
nonzero `GraphRevision`。Visible capture、mutation、commit validation 与 publication 会进入
graph-state lane。长时间 operation execution 在该 lane 之外使用 request-owned snapshot。
Coordinator 会发布 checked supersession generation，并对每个精确 key 合并一个 pending owner；
现有 compute-request lane worker 仍是唯一 logical active runner，同时该 lane 继续串行化
execution-route inspection/replacement。

## 新 Session 加载

`Kernel::load_graph()` 会在需要时配置 Host-lifetime 固定 worker 数量，在已发布 graph map 之外
以复制的 route binding 构造 runtime，校验完整 graph document，最后才插入 runtime。Source
选择会区分省略与显式请求：

- 空 `yaml_path` 会在 `<root>/<session>/content.yaml` 存在时使用该文件，否则有意创建
  empty session；
- 每个非空 `yaml_path` 都是显式路径。Kernel 要求它存在并能复制到 session path，除非它已经
  指向同一文件。显式 source failure 返回 `GraphErrc::Io`，绝不回退到旧 session content。

Worker 请求只有零到八有效；零解析为 `min(max(1, hardware_concurrency()), 8)`，显式正值保持
精确。首次选择固定 service 的 configuration 或 load 会冻结该进程数量；之后的 zero/equal 请求
保持它不变，冲突的正值请求会被拒绝。每个 runtime 为 HP 与 RT 各复制一个封闭词汇中的
`cpu`、`serial_debug` 或 `gpu_pipeline` route id，generation 初始为一。Graph load 不构造物理
executor、policy context、scheduler、plugin 或 ledger reservation。每个 Run 稍后会携带完整
resource vector，经过公共 ready/policy/reserved-start 边界准入。

Graph document loading 是“准备后发布”事务：

1. `GraphIOService` 让注入的 `GraphDocumentReader` 返回一份有序、脱离运行态的
   `GraphDefinition`。已配置的 `YamlGraphDocumentAdapter` 拥有 filesystem access、YAML parsing
   与 translation；静态 parameter 与可选 output parameter 都是深度拥有、没有 YAML alias 的
   `ParameterValue` tree。
2. YAML adapter 在生成 definition 时分类 parser、root-shape、parameter representation 与
   node-field failure。Parameter 非 map value、不支持的 tag、规范化 key collision 以及
   Int64/Double overflow 都属于 document failure。
3. `InMemoryGraphDocumentAdapter` 会在 stage 完整临时 node map 时拒绝重复 id 和空的必需
   parameter-edge name。
4. Adapter 精确调用一次 `GraphModel::replace_nodes()`。该调用会验证 dependency 与 cycle、
   构造 replacement adjacency、一起安装 node 与 topology、重置 graph runtime metadata，并推进
   topology generation 与 authoritative `GraphRevision`。
5. `Kernel` 获取 graph-registry mutex，重新检查 shutdown 与 duplicate-name state，注册
   lifetime anchor，并在一次最终 publication transaction 中插入预分配 map node。

因此，path、parser、definition conversion、topology、unexpected 或 resource failure 都不会发布
partial graph 或新 session。`std::bad_alloc` 原样传播；其他 failure 使用下文稳定矩阵。
Parse 前已经发生的 directory creation 与 file-copy side effect 不会回滚，但它们不是 graph-map
publication。发布前发生失败会销毁未发布 runtime 及其复制的 route binding；不存在需要归还的
Graph-owned execution reservation。已为 Host 配置的固定 worker pool 会继续供后续 session 使用。

分类与事务的依据由
[ADR 0005](../../adr/zh/0005-graph-document-ingestion-is-a-classified-transaction.zh.md)
固定。

## 现有 Session Reload

`Host::reload_graph()` 会先让 session 进入防并发 close 的 admission，随后区分 missing
session，并通过 `GraphStateExecutor` 提交 reload。即使 path 为空，missing 或 closing
session 也返回 `NotFound`；已准入 existing session 的空 path 返回 `InvalidParameter`。
Admission 发生在 session lookup 之前，并持续到 backend LastError 转换完成，因此 close 无法在
接受 reload 后擦除 runtime 或其 diagnostic state。每个非空 source 都与 initial load 使用相同的
IO、syntax/schema、topology、unexpected-failure 与 resource-exhaustion 分类。

`GraphIOService` 会构造脱离运行态的 definition，in-memory adapter 则像 initial load 一样 stage
并验证临时 replacement。在 adapter 的单次 `replace_nodes()` 调用成功前，visible node map、
topology、topology generation、authoritative revision、cache、timing、dirty/planning state、
runtime state 与 Graph/session identity 都保持不变。这项保证同时覆盖 handled failure 和传播的
`std::bad_alloc`。

成功 reload 会替换整个图、重置 model runtime state、保留 live Graph instance identity，并且即使
复用 node id 也会同时推进 topology generation 与 authoritative revision。`RealtimeProxyGraph`
等 runtime-owned mirror 会观察 topology-generation boundary 并丢弃陈旧的 per-node state；staged
compute 也会因 exact revision validation 失败，而不能发布到 replacement 中。Disk-cache
diagnostic reset 会经过 worker record 与 reader snapshot 使用的同一个私有 no-throw store，并且只在
成功 replacement publication 中发生；失败 reload 会与其余 runtime state 一起保留此前完整诊断。

## Execution Route Replacement

Execution-route inspection 与 replacement 由私有 per-Graph compute-request lane 与同图 compute
串行化。Embedded Host inspection 会先把 missing/closing session 保持分类为 `NotFound`，随后在
Kernel 的 optional route lookup 之前把无效 `ComputeIntent` 拒绝为 `InvalidParameter`。有效 intent
的 copied route 缺失时仍返回 `NotFound`；成功 inspection 只返回 copied route/statistics snapshot。

对于 replacement，Kernel 会先拒绝封闭的 `cpu`、`serial_debug`、`gpu_pipeline` 词汇之外的值。
在 lane 内，`GraphRuntime` 会先复制 candidate id，再锁定 route state，验证 intent 与 checked
generation，最后一起发布复制的 id 与下一个非零 generation。同名 replacement 也会推进 generation。

Replacement 不构造 worker、device owner、policy context、plugin 或 resource reservation，因此不需
transient process headroom。未知 route、无效 intent、generation exhaustion 或已处理的 lane-lifecycle
failure 会保留此前 binding 并返回 `InvalidParameter`；missing 或 closing session 仍为 `NotFound`。
Candidate copy 产生的 `std::bad_alloc` 会通过文档化 Host 边界传播，不会产生 partial publication。

## 现有 Session 保存

`Host::save_graph()` 会让 session 进入防并发 close 的 admission，要求 session map entry
存在，并通过 graph mutation 以及 compute capture/commit 使用的
`GraphStateExecutor` 串行化 visible node snapshot。`GraphIOService` 会先通过
in-memory adapter，按 node id 升序 capture 一份脱离运行态且
排除全部 runtime state 的 definition，再交给注入的 writer。已配置的 YAML adapter 会在
destination open 前发出完整 representation。Missing 或
closing session 返回 `GraphErrc::NotFound`，并在 destination access 前停止。对于 existing
session，可恢复的 definition capture、YAML emission，以及 destination
preparation/open/write/flush/close failure 会统一归类为 `GraphErrc::Io`。资源耗尽保持精确的
`std::bad_alloc` exception channel，不会转成 `Io` status。

Save 是 owner-state read transaction。成功、返回的 failure 与传播的资源耗尽都会让 graph
topology、topology generation、authoritative revision、cache/timing/dirty/planning/runtime
state、Graph instance identity 和 session identity 保持不变。调用方可在任何已报告的失败后
对同一个已准入 session 重试；IPC client 对每次 mutation 只发送一次，不会自动重试。

Destination 只有刻意收窄的保证。Save 会直接写入调用方提供的 path，不使用 temporary file 加
atomic replacement。Destination 成功 open 前发生的失败会保留 existing bytes；open 成功后，
write、flush、close 或之后的资源失败可能留下已创建、已截断或只写入部分内容的 destination。
因此，destination rollback 不属于 graph-owner transaction。

## Node Replacement 与结构编辑

`Host::set_node_yaml()` 会让 session 进入防并发 close 的 admission。Required node lookup、
通过注入 reader 完成的 candidate `NodeDefinition` parsing、强制 replacement-id assignment、
in-memory materialization 与 `GraphModel::replace_node()` 会在同一个 graph-state work item 内
执行，因此 clear 或 reload 无法插入 lookup 与 mutation 之间。`get_node_yaml()` 使用相反方向的
单节点 capture，并经过注入的 writer。Public method 名称因 ABI 稳定性保留；私有
Kernel/Interaction method 与 GraphIO 保持格式中立。`Node` 不暴露 YAML conversion method。Missing 或
closing session，以及 requested node 缺失，都会返回 `GraphErrc::NotFound`。对于 existing
target，parsing 与完整 candidate-topology validation failure 返回
`GraphErrc::InvalidYaml`。

Replacement 会复制当前 node map、验证完整 candidate topology、准备 topology/revision 的
successor generation，最后才通过 no-throw container swap 与 scalar store 发布 visible state。
因此，parse、missing-dependency、cycle、allocation 或 generation-overflow failure 都会保留之前的
node map、topology、generation、revision 与 runtime state。只有全部可能抛异常的 structural
preparation 完成后才开始 publication。

`add_node()`、`remove_node()` 和 input-rewire 方法采用同一种 candidate-map 模式。成功的结构编辑
会重建 adjacency index、推进 topology generation 与 authoritative revision，并清除 cached
full task graph。被拒绝的 candidate 不会推进这两个值。

显式 disk、memory、combined 与 transient-memory cache clear 使用不同的 failure boundary，因为
filesystem deletion 与多 node clearing 可能先部分成功，后续 operation 才抛出异常。Graph-state
work item 会先计算 checked successor revision，因此 overflow 会保持 Graph、cache 与 file 不变。
该纯准备成功后，work item 以 no-throw 方式发布 successor revision，随后才进入 cache side effect。
后续 clear failure 仍按现有 facade contract 返回，但 revision 绝不回滚：即使 cache root 已删除却
无法重建，或只释放了部分 memory cache，clear intent 前捕获的每个 Run 仍为 stale。这是 revision-safe
invalidation，与 Issue #73 的 cooperative cancellation 不同。Cache clear 不会请求 cancellation；
stale Run 与 cancelled Run 只在“都不得发布 request-owned staged output”这一规则上汇合。

## ROI 投影

`Host::project_roi()` 与 `Host::project_roi_backward()` 会让 session 进入防 close 的
admission，并在同一个 graph-state work item 内完成两个 endpoint 的 lookup 与 propagation。
Missing 或 closing session，以及缺失的 source/target node，都会返回
`GraphErrc::NotFound`。当两个 endpoint 均存在，但 ROI 为空、路径不可达或 propagation
无法产生有效 rectangle 时，Host 返回 `GraphErrc::InvalidParameter`。

Host request/result 以及私有 graph、propagation、dirty 与 planning state 都携带 `PixelRect`
和 `PixelSize`。OpenCV adapter 或 provider 只能在实际 matrix operation 处局部构造 library
geometry；该表示不会进入 graph-state work item 或其保留状态。

Existing-session propagation exception 仍会更新 Kernel 的 best-effort `LastError`
observation，但当前 Host result 直接来自同一个 required operation；它绝不会在 operation
结束后通过读取共享 diagnostic state 重建。每个精确 `GraphRuntime` 独占一个由 mutex
保护的 optional diagnostic slot；Kernel 不存在 process-global identity-to-error table。
public name lookup 会先保留当前 runtime，再只复制该 runtime 的 slot。由旧 runtime 保留
调用产生的迟到 store 或 clear translation 只能修改旧 slot；该 slot 随最后一个 shared
runtime owner 销毁，不能影响同名 replacement。

## 注入的持久化依赖生命周期

持久化依赖按 `Kernel` 组合一次。Embedded product root 提供共享 `ImageArtifactCodec`、共享
`YamlCacheMetadataCodec`，以及一个从 reader/writer contract 两个视角使用的共享
`YamlGraphDocumentAdapter`。`GraphCacheService` 保留两个 codec owner；`GraphIOService` 保留
reader 与 writer。Kernel、GraphCache 和 GraphIO 会在构造时拒绝空 owner，也不提供 configured
fallback。

这些依赖不是 graph state：reload、clear 与 close 不会替换它们。`Kernel::~Kernel()` 会在普通
member teardown 到达 cache、traversal、diagnostic、IO 或 ROI collaborator 前显式清空自有 runtime
map。每个 `GraphRuntime` 因此会在 graph-state 可用时停止并排空 compute-request
work，再在释放 Graph-local route value 与 platform state 前排空 graph-state；整个过程中，这些借用的 Kernel
service 与全部注入 owner 仍然存活，只有之后的 service destruction 才会释放它们。
Owning Host 必须在 Kernel destruction 前停止外部 Kernel-call admission，因为 private
graph map 不是 concurrent-destruction API。Codec/document `GraphError` 保留各自文档化的
category，`std::bad_alloc` 原样传播。

## Clear

`GraphModel::clear()` 执行 model reset，而不只是删除 node。它会清除：

- node 与 topology adjacency；
- timing 与 accumulated I/O state；
- dirty snapshot、generation 与 source commit state；
- compute-plan history 与 full-task-graph cache；
- disk-cache diagnostics 与 skip-save state。

Clear 会推进 topology generation 与 authoritative revision，并让 model 保持 quiet
mode。它不会关闭或销毁所属 `GraphRuntime`；session 仍处于 loaded 状态。它也不会
删除 disk-cache file、清除 runtime-owned event/trace ring、直接清除
`RealtimeProxyGraph`，或清除 runtime-owned `LastError` diagnostic slot。
`RealtimeProxyGraph` 会在下一次 synchronization 观察到已推进的 topology generation
时自行失效；较旧的 staged compute 会被 revision validation 拒绝。Diagnostic record、
snapshot、reset、clone 与 staged exchange 全部使用同一个封装的 no-throw mutex 契约，
因此 clear 可以与 worker diagnostic traffic 重叠，而不会无同步访问
optional/path/string。

## Close 与 Lifetime

一个完整构造的 `GraphRuntime` 拥有稳定 `GraphLifetimeAnchor` 与预分配的单调 close
coordinator。Kernel 只会在 load 已可发布后注册不复用的 `GraphInstanceId` 与 anchor。
Lifecycle candidate 从第一次 admission check 起就保留 anchor lease，直至原子安装
standalone/realtime bundle 或完成 rollback，因此 close 不会漏掉 marker 前已经开始的 planning。

Direct Kernel close 会在持有 graph-registry mutex 时解析 name、保留精确 shared runtime root，并
执行非等待 owner/joiner selection。因此，lookup 与 selection 是一个 transaction：caller 不会
观察旧 name 后错误加入 replacement runtime，而 owner 会在 map removal 后继续保持 retired
runtime 存活。如果 failed generation 仍欠旧 joiner 一个结果，selection 会返回显式 retry token，
而不是等待。caller 会释放 graph-registry mutex、等待该 token，再同时重验 retained shared
runtime 与 `GraphInstanceId`，然后重新 selection。之后每个 runtime operation 都采用相同的短
lookup-and-retain 规则；retry waiting、lifecycle settlement 或任何 Graph-owned lane 运行时都不
持有 graph-registry lock。

Embedded Host close 会先选择或加入唯一 close generation，并把 public session 标记为 closing。
新的 compute、execution、reload、required save、node-YAML replacement、ROI projection、
timing inspection 与 all-cache clearing admission 都会失败。在 `RunLifecycleRegistry` fence 下，
owner 会把精确 Graph row 从 `Open` 改为 `Closing`、拒绝 pending candidate、拒绝 visible commit，
并对每个已安装 Run 请求 `GraphClose` cancellation。等待前会释放 fence；candidate 要么在 marker
前完成安装，要么回滚，任何迟到 Run 都不能进入该 row。所有 live 并发 close caller 会加入同一个
generation result。

仅选择 owner 并不是 lifecycle 线性化点。如果该 owner 在 `Open -> Closing` 之前失败，
coordinator 会把精确的 `std::exception_ptr` 发布给所有已经加入该 generation 的 caller。
只要旧 joiner 仍可能观察该 failure，就不允许 fresh owner 启动；最后一个 joiner 消费 failure
后 coordinator 才返回 `Idle`，下一个 caller 会获得新的、不复用的 generation。这个按
generation 限定的 failure handoff 会防止 ABA 与 joiner hang，同时允许析构函数重试普通 close
路径。`Open -> Closing` 一旦线性化，就禁止 recoverable unwind，并沿用既有的 fail-stop cleanup
边界。

已接受的同步调用会完成 result/status translation；已接受的 daemon job 继续拥有
status/result/release；已 admission Run 只保留 settle 所需的 ready、execution、completion 与
graph-finalization path。Finalization 会等待 terminal publication、物理 callback quiescence、
commit/discard completion、root reservation 与 child grant 精确释放、Graph lifetime lease 释放及
registry unregistration。永不返回的 non-preemptible provider 因此可以如实阻塞 close；实现不会
伪造 recovery。

Graph index 与 candidate count 清空后，close 会执行以下精确且不可逆的尾序：

1. 移除空 lifecycle-registry row；
2. 停止 coordinator 与 compute-request admission，唤醒因容量阻塞的 producer，retire parked
   ticket，排空已接受 callback，并在 graph-state finalization 仍可用时 join request worker；
3. 停止、排空并 join graph-state lane；
4. 把 runtime 标记为 retired，并释放 route ownership；
5. 再次获取 graph-registry mutex，验证该 name 仍指向所保留 runtime，擦除 map entry，并在释放
   mutex 前发布 close-generation success。随后其余 shared runtime owner 可安全 retire，不会留下
   任何由 map 派生的 dangling pointer。

Marker 之后不存在 reopen 或 worker reconstruction。有效 direct Host close 会成功；不存在 Graph
且没有可加入 generation 时返回 `NotFound`。Graph close 不会停止 process-owned worker、route、
policy binding 或无关 Graph Run。

Embedded Host 未显式 close 就销毁时，会先停止外部 Host admission，并为每个 live session 调用
相同 Graph close path。所有 Graph settle 后，composition root 会恰好一次调用幂等的 process
execution shutdown。该 shutdown 把 service 改为 `Stopping`、把其余 row 改为 `Closing`，排空
每个已安装 Run，然后停止 ready 与 route admission、join 物理 executor、retire policy invocation
及 current/displaced binding，并且只在全部 lifecycle/resource counter 为零时发布
`ServiceStopped`。Kernel 会先在不发生 mutation 的前提下拒绝 same-service worker 或 policy-
callback caller；该 recoverable preflight 会让 publication gate 保持 open、telemetry 保持
`Accepting`，且 shutdown generation 仍为零。随后，一个短 graph-registry transaction 会关闭
单调 late-publication gate，并在 service transition 与 cancellation fanout 之前释放 mutex。
lifecycle fence 会把已经通过构造的
load 与 `ServiceStopping` 排序：load 要么先发布并由 shutdown 排空，要么输给已关闭 gate，且永
不注册 lifecycle row。fanout 阻塞期间，Graph listing 与无关 name lookup 仍可用。由于 publication
gate 关闭是不可逆边界：由于 publication 无法安全重开，gate 关闭后的任何意外 observer 或
service-transition failure 都会 fail-stop。直接拥有内部 Kernel 的
调用方同样必须在析构前停止并发调用。这些 joined boundary 也是每个 live 或 staged
`GraphModel` diagnostic store 的 lifetime fence；store 本身不拥有 thread 或 detached lifetime。

`photospiderd` 围绕该 embedded Host contract 拥有 daemon session identity、job admission、Host
serialization 与 shutdown drainage。其准确 mapping、lease、socket 与 shutdown 规则定义在
`../../codebase-structure/zh/IPC-Protocol-v2.zh.md`；它们不属于 graph-kernel ownership。

## 当前错误表面

| 操作 | 当前公共行为 |
| --- | --- |
| initial load，重复 session | `GraphErrc::InvalidParameter`；existing session 保持不变 |
| execution default，未知 route，或 worker 请求超过八/与固定 pool 冲突 | `GraphErrc::InvalidParameter`；未来默认值保持不变 |
| initial load，空 path | session-local `content.yaml` 存在时加载该文件；否则有意发布 empty session |
| initial load，显式 missing/unreadable/uncopyable source 或 session-path failure | `GraphErrc::Io`；不发布 session、不回退；已经创建的 filesystem scratch side effect 不回滚 |
| initial load，YAML syntax/representation、非 sequence root、duplicate id、parameter representation/overflow 或 node-schema failure | `GraphErrc::InvalidYaml`；不发布 session |
| initial load，missing dependency 或 cycle | 精确的 `GraphErrc::MissingDependency` 或 `GraphErrc::Cycle`；不发布 session |
| initial load，unexpected non-resource failure | `GraphErrc::Unknown`；不发布 session |
| initial load，resource exhaustion | 传播 `std::bad_alloc`；不发布 session |
| reload，missing 或 closing session | `GraphErrc::NotFound` |
| reload，已有 session 且 path 为空 | `GraphErrc::InvalidParameter`；先前 graph 与 runtime state 保持 visible |
| reload，missing/unreadable source | `GraphErrc::Io`；先前 graph 与 runtime state 保持 visible |
| reload，YAML syntax/representation、非 sequence root、duplicate id、parameter representation/overflow 或 node-schema failure | `GraphErrc::InvalidYaml`；先前 graph 与 runtime state 保持 visible |
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
| execution inspection，missing/closing session 或 route 缺失 | `GraphErrc::NotFound` |
| execution inspection，existing session 使用无效 intent | `GraphErrc::InvalidParameter` |
| execution replacement，未知 route、无效 intent、generation exhaustion 或已处理的 lane failure | `GraphErrc::InvalidParameter`；旧 route binding 保持发布 |
| execution replacement，missing 或 closing session | `GraphErrc::NotFound` |
| clear 或 close，missing session | `GraphErrc::NotFound` |

`OperationStatus` 暴露 error domain、signed code、stable name 与 diagnostic message。调用方必须按
domain 与 code 分支，而不是按 diagnostic text 分支。IPC 会序列化该精确 status，并在 Host load
失败时回滚其 reserved session name；它不会引入 transport-only graph-document taxonomy。

## 边界与原理

- Session identity 是复制的 label；graph 与 runtime ownership 绝不会跨过 Host boundary。
- Prepare-before-publish load 与 prepare-before-swap reload 会把不完整 topology 隔离在 graph map
  之外，并在分类失败时保留先前 graph。
- 脱离运行态的 definition 与 in-memory adapter 会把 format conversion、persistent value、
  model materialization 和 topology publication 保持为可独立测试的阶段，同时不改变 public
  path contract。
- Graph-owner transaction 与 destination-file side effect 保持分离：save 会保留 graph state，
  但不承诺 destination atomic replace。
- `GraphStateExecutor` 串行化可见 graph capture、mutation、exact commit validation 与
  publication。私有 compute-request lane 串行化同图 compute 与 execution-route access，而
  long-running operation execution 使用 graph-state 之外的 request-owned snapshot。
- Graph registry lock 只串行化 runtime-root lookup/publication、非等待 close-generation
  selection、最终 erase/success publication 与 shutdown publication rejection。
  failed-generation retry waiting 以及 process lifecycle transition/cancellation fanout 都会在释放
  它之后运行。需要嵌套时，顺序只能是 graph registry 在非等待 close-coordinator access 或
  lifecycle registration 之前；任何 lifecycle/lane wait 都不持有它，也不允许反向嵌套获取。
  仓库专用 focused test 可以用 lifecycle observer 重新编译该 seam；已安装 production archive
  既不包含 observer state，也不包含其 branch。
- Graph route binding 是复制的 resource-neutral 值；物理执行与 policy context 保持由 Host/进程拥有。

这些边界使 publication、mutation 与 resource ownership 可以独立测试，而无需把共享 diagnostic
state 当作 transaction log。
[ADR 0005](../../adr/zh/0005-graph-document-ingestion-is-a-classified-transaction.zh.md)约束当前
ingestion contract。`GraphDefinition`、`InMemoryGraphDocumentAdapter`、注入的
graph-document contract 与已配置的 YAML filesystem adapter 现在共同构成当前 persistence
boundary。Issue #61 已实现 filesystem adapter 注入与格式中立的私有 Host composition，同时保留
public YAML-named ABI；Issue #62 将共享 YAML value conversion 与 cache metadata 移到
adapter-owned contract 后方，因此 runtime、graph、compute、inspection 与 cache declaration
保持 YAML 中立。Issue #63 已完成 dependency-disabled product profile：不发现 yaml-cpp 时，
empty 与 in-memory session 仍可使用；显式 graph-document 或 cache-metadata representation
operation 会使用 unavailable adapter，并返回 `GraphErrc::Io`。

已接受的
[ADR 0007](../../adr/zh/0007-compute-runs-and-process-execution-have-separate-owners.zh.md)
现在约束已实现的强 Graph identity/revision、staged compute、精确 revision/generation commit
predicate、cooperative Run cancellation、latest-wins supersession、request-owned realtime
`RunGroup`，以及分离的 compute-request/graph-state lane。由 `ExecutionService` 拥有的
`RunLifecycleRegistry`、原子 Run-admission/Graph-close fence、lifecycle-driven close/shutdown
cancellation、精确 settlement 与 Issue #76 的 source-private telemetry 已是当前行为。Issue #75
的纯 C policy ABI、Host-authored frontier、reserved-start admission 与私有 execution-route
replacement 继续保持当前行为。Public cancellation 与 lifecycle telemetry 有意不通过 Host、CLI
或 IPC 暴露。

## 实现与验证入口

- `src/lib/core/image_artifact_codec.hpp`
- `src/lib/adapters/opencv/image_artifact_codec_opencv.*`
- `src/lib/providers/configured_image_artifact_codec.*`
- `src/lib/providers/configured_persistence_adapters.*`
- `src/lib/runtime/kernel.cpp`
- `src/lib/runtime/kernel_io_cache_facade.cpp`
- `src/lib/runtime/kernel_inspection_facade.cpp`
- `src/lib/runtime/kernel_dirty_roi_facade.cpp`
- `src/lib/graph/graph_definition.hpp`
- `src/lib/graph/graph_document_reader.hpp`
- `src/lib/graph/graph_document_writer.hpp`
- `src/lib/graph/in_memory_graph_document_adapter.*`
- `src/lib/adapters/yaml/graph_definition_yaml.*`
- `src/lib/adapters/yaml/yaml_graph_document_adapter.*`
- `src/lib/adapters/yaml/parameter_value_yaml.*`
- `src/lib/adapters/yaml/yaml_cache_metadata_codec.*`
- `src/lib/core/cache_metadata_codec.hpp`
- `src/lib/core/parameter_value_text.*`
- `src/lib/graph/graph_cache_service.*`
- `src/lib/graph/graph_io_service.cpp`
- `src/lib/graph/graph_revision.hpp`
- `src/lib/graph/graph_state_executor.cpp`
- `src/lib/graph/graph_model.cpp`
- `src/lib/compute/compute_commit_policy.hpp`
- `src/lib/compute/compute_service.*`
- `src/lib/compute/run_lifecycle_registry.*`
- `src/lib/compute/execution_lifecycle_telemetry.*`
- `src/lib/compute/realtime_proxy_graph.*`
- `src/lib/runtime/graph_runtime.*`
- `src/lib/host/embedded_host.cpp`
- `src/lib/runtime/kernel_compute.cpp`
- `tests/integration/test_host_adapter.cpp`
- `tests/integration/test_graph_document_errors.cpp`
- `tests/integration/test_graph_document_injection.cpp`
- `tests/integration/dependency_disabled_install_smoke.py`
- `tests/unit/test_graph_document_adapter.cpp`
- `tests/integration/test_ipc_daemon.cpp`
- `tests/unit/test_ipc_protocol.cpp`
- `tests/integration/test_disk_cache_diagnostic_concurrency.cpp`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/unit/test_compute_run.cpp`
