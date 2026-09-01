# ADR 0015：将产品边界重置为嵌入式内核与本地守护进程

- 状态：已接受
- 日期：2026-09-01
- 决策类型：0.x 破坏性产品边界重置
- 归档标签：`pre-breaking-scope-reset-2026-09-01`
- 已归档 kernel commit：`446e83687ecf49be8e9b66beac8b40c7b8b224de`
- 配套 daemon 决策：`photospider-daemon/docs/adr/0001`

## 背景

Photospider 曾在一个 kernel 中累积多种不同产品概念：嵌入式图执行、本地 daemon
路由、持久服务状态、进程 worker 监管、policy plugin、plugin 准入、artifact
权威和发布证据画像。这些概念造成所有权含混，也让本地 correctness 机制被描述成
service 或 security 产品。

本决策是有意进行的 0.x 破坏性切割。重置前源码只通过 Git 历史和上述 annotated
archive tag 保留。active tree 不保留关闭的 target、兼容 adapter、抛异常 stub、
forwarding header 或归档源码副本。

## 决策

### 所有权矩阵

| 能力 | Photospider kernel | Photospider daemon |
| --- | --- | --- |
| Workflow source model 与验证 | 拥有 `WorkflowDocument` 和 graph validation | 只通过已安装的公开 kernel API 接收 workflow |
| Compiler pipeline | 拥有 semantic IR、optimized IR、operation traits、优化和 physical planning | 不复制或序列化内部 IR |
| 本地执行 | 拥有 CPU 必需/GPU 可选执行、transfer、residency、本地资源核算、取消、fallback 和结果发布 | 通过公开 compile/execute facade 提交工作 |
| Runtime data | 拥有 `Value`、`Region`、layout、memory 和非持久结果 | 仅在 daemon Job 存活期间保留临时结果 |
| Graph 所有权 | 拥有独立 `GraphContext`/`ExecutionContext` 对象 | 拥有不透明 `SessionId` 逻辑命名空间 |
| 工作编排 | 不定义 Job identity、queue、status、registry 或 retry | 拥有临时 `JobId`、queue、status、取消、result 与 release |
| IPC | 无 | 拥有 local IPC v3 和 `photospiderd` 生命周期 |
| Operation 与 provider | 拥有 semantic traits、operation/provider ABI 与 registry，以及可信进程内 DSO 加载 | IPC 绝不接收 plugin 路径 |
| Benchmark | 拥有原始 timing、backend/transfer/resource diagnostics、plan digest 和 correctness oracle | 可报告普通 daemon 生命周期 timing，但不拥有 evidence authority |

依赖方向仅为单向：

```text
photospider-daemon
  -> 隔离安装的 Photospider package
  -> 公开 compile/execute/value contract
```

Kernel 绝不依赖 daemon。Daemon 绝不 include kernel 私有 header、链接 source-tree
target、复制 compiler/planner code 或把内部 IR 放上 wire。

### Kernel 边界

Kernel 是 session-agnostic、单机且可嵌入的。一个进程支持多个独立
`GraphContext` 和 `ExecutionContext`，也支持它们并发。Graph context 是 kernel
对象或 handle，不是 daemon Session，也不是 registry entry。

保留的 pipeline 为：

```text
WorkflowDocument
  -> SemanticGraphIR
  -> OptimizedGraphIR
  -> ExecutionPlan
  -> local ExecutionContext
  -> ExecutionResult
```

Document identity、semantic identity、optimized identity、physical plan
identity、runtime allocation identity 和 daemon identity 保持分离。
`SemanticGraphDigest`、`OptimizedGraphDigest`、`ExecutionPlanDigest` 与
`PlanCacheKey` 是非安全的 compiler/cache identity。派生 cache 可丢弃并重建。

### Daemon 边界

Daemon 是同一用户、本机、非持久 orchestration layer。其 Session 是一个进程和
一个 trust domain 内的逻辑命名空间，不是 tenant 或 isolation boundary。重启会
清空全部 Session、Job 和 result。

Job 状态机严格为：

```text
Queued -> Running -> Succeeded | Failed | Cancelled
```

不存在 attempt identity、自动 retry、checkpoint、recovery journal、持久
Job specification、持久 artifact identity、output commit、receipt 或按 tenant
quota。调用者 retry 是一次新 submit。Session close 会取消未完成 Job 并释放临时
result。Job cancel 映射到 kernel cooperative best-effort cancellation，并拒绝
stale publication。

Local IPC v3 只暴露：

1. `session.create`
2. `session.close`
3. `job.submit`
4. `job.status`
5. `job.cancel`
6. `job.result`
7. `job.release`
8. `daemon.info`
9. `daemon.shutdown`

受支持 POSIX 系统使用 Unix-domain socket；受支持 Windows build 可使用本机
named-pipe abstraction。不存在 TCP、HTTP、gRPC、TLS、remote endpoint、v2
adapter 或双协议。

### Operation 与 provider 边界

保留 operation ABI/SDK/registry 和 data-definition/provider ABI。Operation
semantic traits 是 compiler input。Operation/provider DSO 与 host 处于同一信任域
并在进程内运行。Operation set 在进程启动时配置，开始执行后只读。

ABI version、structure size、alignment、pointer/count pair、array bound、
overflow、type/shape/`Region`/layout/facet validation、exception fencing 和
exact cleanup 都是 correctness contract，而不是 sandbox 或 plugin security
product。

破坏性的 operation ABI 为 version two。每个 operation 发布带 required 标志的 closed
typed parameter schema；compiler 在 semantic IR 形成前拒绝 unknown、missing、
wrong-type、duplicate/conflicting 或其他非法参数。Operation callback 接收已验证参数值与
plan-derived input Region。因此 Whole、Elementwise 与 overflow-safe clipped Halo rule
会真实改变 physical input demand 和 plan identity，而不是只作为 digest metadata。

### 保留的 correctness validation

本次重置删除安全产品声明，而不是 defensive correctness。以下仍为必需：

- ABI version/size/alignment 与 pointer/count/array validation；
- integer 与 allocation overflow validation；
- graph、typed IR、type、shape、`Region`、layout、facet 与 plan validation；
- malformed local-IPC frame 与 result-shape validation；
- stale handle、stale completion 与取消后 publication rejection；
- exception fencing 和精确资源清理；
- 可选 GPU path 不可用或拒绝工作时的 CPU fallback；
- deterministic CPU 与 optional GPU FIFO 共享一个 ExecutionContext-wide waiting-callback
  bound，running callback 不计入该数量；
- 平台和 toolchain 支持时的普通负向、并发、ASAN、TSAN 与 fuzz coverage。

### 删除的产品领域

Active 产品不包含或宣传：

- execution-profile SLO identity 或 release evidence；
- verdict/envelope/attestation/receipt authority；
- network service、authentication、authorization、Principal、Tenant、role、
  capability、multi-tenant quota 或 control plane；
- durable Job、attempt、checkpoint、recovery、journal、backup/restore、deploy、
  rollback 或 operations-readiness authority；
- fresh worker process、heartbeat/lease supervision、TERM/KILL/reap ownership 或
  remote/distributed worker model；
- plugin process isolation、sandbox、cryptographic trust、signature、certificate、
  package admission 或 trust bundle；
- policy DSO、policy public ABI/SDK/loader/registry/fixture；
- durable artifact authority、durable Value identity、output commit、receipt 或
  manifest-last publication。

普通 bounded local concurrency 与 backpressure 仍保留。`ExecutionRun`、per-lane
deterministic FIFO、它们的 single shared waiting admission、CPU worker、可选 local GPU
lane、`ResourceLedger`、transfer/residency tracking、deterministic scheduling、
cancellation、fallback 和 stale-completion rejection 都是 kernel mechanism，不得改名为
daemon 或 service authority。

### Benchmark

维护的 benchmark 可报告原始 compile/plan/execute/operation timing、selected
backend、transfer count/bytes、peak live bytes、fallback/error reason、plan
digest、result digest 和 correctness oracle。提供 oracle 时必须给出 bounded canonical
`oracle_name`，并记录在每个 sample 与 report；没有 oracle 的 run 会显式标记为
`unchecked`。它们不生成 execution-profile
identity、applicability/startability verdict、evidence envelope、attestation、
release evidence 或 durable artifact reference。

## 精确非目标

- Remote 或 multi-user service operation。
- Authentication、authorization、tenant isolation 或 security control plane。
- Durable queueing、retry、recovery、checkpointing 或 artifact storage。
- Operation/provider DSO 的 process isolation 或 sandboxing。
- 内部 semantic/optimized/physical IR 的稳定序列化。
- 兼容 IPC v2、frozen four-cell gate、被删 package component 或被删 public
  header。
- Distributed execution 或 remote device。

这些内容已经删除或不在范围内；它们不是 deferred、optional、default-disabled
或未来 roadmap 项。

## 被取代的决策与权威

本 ADR 是最高 active 产品边界权威。它将 ADR 0001、0004、0009、0010、0011 和 0013 从
active ADR 集合退役，因为其精确的 graph-state scheduler、OpenCV operation、
service/evidence 或 dense-image facet 决策在 reset implementation 中不存在。它取代
ADR 0002、0003、0005、0007、0008、0012 与 0014 中的产品范围部分；这些 ADR
保留的本地 kernel contract 会直接收窄。它也取代所有把 Job、worker process、
policy、trust、isolation、durable artifact、evidence 或 network-service authority
分给 kernel，或者把 IPC v2 compatible maintenance 分给 daemon 的 active
roadmap、OpenSpec、architecture page、Issue 或 Project description。

历史 archive 仅是历史证据。Active index 不得把 archive 链接成当前权威，也不得
使用 archive 恢复已删除领域，除非先产生一个明确取代本决策的新破坏性产品决策。

## 后果

- 0.x 版本线立即发生 source 与 package compatibility break。
- Kernel 和 daemon release 必须从隔离安装的 package 构建并测试。
- 被删实现只能从 Git 历史和 annotated archive tag 恢复。
- 文档、Issue、Project、test、CI inventory 与 package export 必须描述同一边界。
- 未来任何重新引入已删领域的提案，必须先产生一个明确取代本决策的新
  product-boundary ADR。
