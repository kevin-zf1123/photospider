# ADR 0014：Compiler、document 与 plan 版本相互独立

- 状态：已接受为目标契约；尚未实现
- 日期：2026-08-31
- 相关：ADR 0006、ADR 0008、ADR 0012、Issues #196、#199-#203、#245

## 背景

当前 kernel 通过 YAML adapter 使用 unversioned `GraphDefinition` value，并使用
request-local `ComputePlan`、full-task-graph cache key、独立 operation implementation
identities、Value/artifact digests 与 graph-cache manifest v2。`WorkflowDocument`、
`OperationSemanticTraits`、`SemanticGraphIR`、`OptimizedGraphIR`、compiler
`ExecutionPlan`、compiler digests 与 typed plan cache 尚不存在。

若把任何现有 token 当作共享 compiler version，就会耦合 source syntax、semantic
interpretation、optimization、planning、executable output、hashing 与 cache storage，也会让
package 或 IPC compatibility 静默接纳 internal compiler bytes。因此 typed-compiler 序列必须在
#199/#200 定义字段之前先决定 version 与 migration。

该决策必须保留已建立的边界：operation ABI v1 继续作为 exact-size pure-C contract，daemon
IPC v2 继续作为独立仓拥有的 wire protocol，当前架构文档继续只描述已实现 behavior，derived
compiler artifacts 可以重建，而不是跨 incompatible semantics 翻译。

## 决策

### 每个 compiler contract 都有自己的 identity

Canonical-byte profile、`WorkflowDocument`、`OperationSemanticTraits`、
`SemanticGraphIR`、`OptimizedGraphIR`、planner behavior、`ExecutionPlan`、三个 digest
domains、plan-cache key、plan-cache record 与 compiler-extension envelope 使用独立 identity，
并都从 `1.0` 开始。

Version compatibility 是 directed allowlist。K1 只接纳 exact `1.0 -> 1.0`。相同 major
version 不意味着 compatibility。Unknown 或 unsupported identity、version、canonical profile、
digest domain、algorithm 或 required extension 都 fail closed。Writer 只写当前 version，且不
downgrade。

完整 registry 与 compatibility matrix 持续维护在
[编译器版本契约](../../development/zh/Compiler-Version-Contract.zh.md)中。

### Canonical identity 使用 binary framing 与 domain separation

Compiler Canonical Encoding v1 使用确定性 big-endian `PSCC` envelope 与 typed value
tree。它不 hash YAML/JSON spelling、C++ memory、host order、padding、pointer 或 iteration
order。

`SemanticGraphDigest`、`OptimizedGraphDigest`、`ExecutionPlanDigest` 与
`PlanCacheKey` 使用 SHA-256 计算 length-framed `PSDG` preimage，并拥有独立 domain
identity/version。没有 typed domain 的 raw 32-byte hash 不是可互换 compiler identity。

### Migration 遵循 authority

Durable source document 与 external trait sidecar 可以通过显式、有界、确定的单向 migration
进入当前 version。当前 writer 成为唯一 durable authority；不保留 compatibility alias、parallel
writer 或 automatic downgrade。

Compiler IR、plan、digest、plan key 与 cache record 都是 derived。Incompatible version 会使其
失效，并从最早的 valid current source 重建。它们永远不会被 migration、reinterpretation 或以
stale 状态执行。

Legacy GraphDefinition/YAML importer 仍属于 #200。临时 legacy/new planner differential
属于 #201/#202，并必须以一次 K4 authority cut 结束。

### Plan-cache identity 包含每个 plan influence

Typed plan key 包含 canonical/digest/planner/trait/IR/plan versions、semantic 与 optimized
digests、effective traits、operation implementation 与 package identities、required
semantic/planning extensions、pass-pipeline 与影响 plan 的 options/static inputs，以及 target
capability facts。

它排除 source formatting 与 diagnostic metadata、单独的 Graph revision、request/Run/session/
time/trace/cancellation/queue observations、non-shaping dynamic payloads、cache paths/eviction、
persistence receipts、daemon state 与 runtime pointer/allocation/fence/lease。如果 excluded fact
后来被证明会改变 plan bytes，则在该事实被 versioned、included 且 cache namespace 改变前，
reuse 都是无效的。

### Extensions 按 effect 参与，且不跨 repository boundary

每个 compiler extension 携带 owner/name/version、effect、canonical codec 与 canonical
payload。Required semantic/planning extensions 进入对应 canonical object、digest 与 plan key；
unknown required extension 会拒绝。Optional diagnostic extensions 可以在 source document 保持
opaque，但不会成为 interpreted IR 或 plan identity。

Compiler objects 仍归 kernel 所有。Operation traits 使用未来 engine-owned registry 或独立
versioned sidecar，且不修改 operation ABI v1。Internal document、IR、plan、digest、cache
record 与 extension 不会成为 IPC v2 或 daemon schema。

## 后果

### 正面影响

- Source、semantic、optimizer、planner、hash、cache 或 extension 的变化只会使依赖它的
  contracts 失效。
- Unknown 与 stale inputs 在 interpretation、cache reuse、scheduling 或 execution 前失败。
- Canonical bytes 与 digest domains 可独立于 file adapter、C++ layout 与 machine
  architecture 复现。
- 单向 durable migration 与 derived rebuild 避免永久 dual document 或 planner authority。
- Traits 可以独立演进，同时 installed operation ABI v1 保持 byte-for-byte 不变。

### 负面影响与缓解

- Exact-only 初始 compatibility 会造成保守 rejection 与 cache miss。更宽的 edge 必须提供
  显式 reviewed evidence，不能由 same-major rule 推断。
- Canonical envelope 先于具体 schema。#199/#200/#201/#202 必须在不静默修改 envelope 的
  前提下冻结 field projection 与 bounds；encoding 变化必须 bump profile version。
- Plan-key 正确性依赖于对所有 plan influence 的分类。任何新发现的 influence 都是 contract
  bug，必须 invalidation 并增加 regression 后才能 reuse。
- Diagnostic extensions 不由 internal IR 保留。其 durable preservation owner 仍是 source
  document。

## 被拒绝的方案

### 使用一个 compiler-wide version

拒绝原因：document、traits、semantic IR、optimized IR、planner、execution plan、digest 与
cache record 的 compatibility 和 rebuild lifetime 不同。

### 推断 same-major compatibility

拒绝原因：这些是 pre-1.0 compiler contracts，旧 reader 无法知道新增 field 或 extension 是否
nonsemantic。

### Canonicalize YAML 或 JSON text

拒绝原因：adapter、number、duplicate-key、Unicode、whitespace 与 ordering behavior 会成为
identity authority。

### 跨 breaking version 迁移 cached plan

拒绝原因：从当前 source 重建，比跨已变化 semantic 或 planner contract 翻译 executable
decision 更安全。

### 把 traits 追加到 operation ABI v1

拒绝原因：ABI v1 已有 exact record sizes、suites、symbols、padding rules 与 entry points。
Compiler metadata 具有独立 version lifetime。

### 通过 daemon IPC 暴露 internal IR

拒绝原因：IPC v2 compatible-maintenance 与 compiler ownership 是独立 repository contracts。
未来 external view 需要自己的 projection schema。

## 与当前事实和交付状态的关系

持续维护的
[kernel 架构索引](../../kernel-architecture/zh/README.zh.md)记录所有 typed-compiler objects
仍未实现。当前 graph、planning、cache 与 plugin 事实继续位于
[Graph 生命周期](../../kernel-architecture/zh/Graph-Lifecycle.zh.md)、
[计算流程](../../kernel-architecture/zh/Compute-Flow.zh.md)、
[Cache 模型](../../kernel-architecture/zh/Cache-Model.zh.md)与
[Plugin ABI](../../kernel-architecture/zh/Plugin-ABI.zh.md)。

[路线图 v3](../../roadmap/zh/Next-Stage-Execution-Plan.zh.md)是交付顺序权威。实时 Issue 与
Project item 仍是状态权威。本 accepted ADR 冻结 target contract；不把
`WorkflowDocument`、compiler IR、digests、`ExecutionPlan` 或 plan cache 提升为当前 runtime
behavior。
