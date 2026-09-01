# ADR 0014：Compiler Document、IR、Plan 与 Digest 具有独立 Identity

- 状态：已接受，由 ADR 0015 收窄
- 日期：2026-09-01 边界修订

## 背景

Typed compiler 需要可复现的中间 identity，同时不能混淆 source document、semantic
meaning、optimization result、physical plan、runtime allocation、cache entry 或
daemon object。

## 决策

Compiler pipeline 有四个显式 value domain：

1. `WorkflowDocument`，caller-owned source model；
2. `SemanticGraphIR`，已规范化并完成 type/shape/trait validation；
3. `OptimizedGraphIR`，语义等价的优化形式；
4. `ExecutionPlan`，面向一组本地 target capability 的 physical plan。

每个 stage 构造后不可变，并在下一 stage 开始前完成验证。Compiler diagnostic 带
source location 与 stage-local code；不修改 input document。

### Version axis

Document schema、operation-trait schema、semantic IR schema、optimizer rule set、
physical planner、public package/API 与 daemon IPC 是独立 version axis。一个 axis
的变化不会默示另一个 axis 兼容。0.x 开发期间，installed package/API change 可以
是 breaking，并且必须通过隔离 consumer 测试。

内部 semantic/optimized/plan representation 不是 wire format，daemon 不会序列化
它们。不承诺跨 release 的内部 IR reader compatibility。

每个 in-memory stage 携带产生它的 exact frozen operation registry 的 private weak
identity。即使 operation key 相同，optimizer、planner 和 executor 也会拒绝来自其他
registry 的 stage。该 runtime freshness identity 不进入 canonical digest 或 wire/package data。

### Digest 与 cache key

Compiler 可以暴露：

- 规范化 semantic content 的 `SemanticGraphDigest`；
- optimized form 加 optimizer identity 的 `OptimizedGraphDigest`；
- physical plan content 加 target capability 的 `ExecutionPlanDigest`；
- 用于派生 lookup 的 `PlanCacheKey`。

Canonical hashing 使用显式 field order、width、enum spelling，以及每个 copied Float64
parameter 中存在的精确 IEEE-754 binary64 bit。positive zero 与 negative zero 因而在
semantic、optimized、plan 与 cache-key stage 保持不同。Compiler 不会规范化 NaN payload
或 infinity，这条 identity 规则也不增加 finite-only validation；每个通过 schema validation
的 copied bit pattern 都按 fixed little-endian order 编码。Digest 排除 runtime allocation
id、address、timing、cancellation observation、queue state 与 daemon id。

这些 digest 是用于 reproducibility、diagnostic、benchmark comparison 和可丢弃
derived cache 的非安全 identity。它们不是 signature、certificate、attestation、
authorization token、durable object identity 或 receipt。Plan cache 总能删除，并
从 source、当前 operation trait 和 compiler 重建。

### Correctness gate

每个 stage 按需检查 duplicate node id、missing reference、cycle、operation
availability、operation 发布的 closed parameter vocabulary、required item、exact
parameter type、parameter bound、type/shape/`Region` rule、integer overflow、backend
capability 与 plan dependency order。unknown、missing、wrong-type 或 conflicting
parameter declaration 会在 semantic IR publication 前失败，builtin callback 不提供隐藏
default。Embedding-provided cache hit 使用前必须重新
验证。Malformed 或 stale entry 变成 miss，不能绕过 compiler validation。

Physical planning 接受 named workflow output 的 optional bounded demand。它把 demand
按 whole-input、elementwise-exact 或 overflow-safe clipped halo Region 反向传播，保守合并
多个 consumer，保存每个 step 的 output/input demand，并把这些值纳入 physical
plan/cache identity。Execution 在 transfer/callback entry 前验证每个 produced Value 覆盖
planned input demand。当前 materialization boundary 仍是 complete Value；demand contract
不宣称已有 dirty 或 incremental executor。

### Closed source vocabulary

当前 `WorkflowDocument` 没有 generic extension bag。其 closed field 与 parameter
variant 被直接验证。新 semantic vocabulary 需要显式 document/API version change 与
compiler handling；未知 field 不会被静默接受进 IR/digest。

## 边界

`WorkflowDocument` 是 compiler input，不是 storage service。Compiler 不拥有 durable
migration authority、recovery journal、daemon lifecycle 或 security provenance
角色。ADR 0015 取代过去附加到这些 version 与 digest axis 的所有更广泛含义。

## 后果

- Stage identity 可检查、可测试，而不成为一个全局 version number。
- 派生 cache 可安全丢弃。
- Daemon 与 package compatibility 可演进，而不暴露内部 IR。
- Reproducibility digest 不隐含 trust 或 persistence。
