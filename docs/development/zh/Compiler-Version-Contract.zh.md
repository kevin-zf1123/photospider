# 编译器、文档与计划版本契约

## 状态与权威

本文是 [ADR 0014](../../adr/zh/0014-compiler-document-and-plan-versions-are-independent.zh.md)
为 [Issue #245](https://github.com/kevin-zf1123/photospider/issues/245)
接受的 K1 版本、canonical bytes、兼容性、迁移、digest、plan cache 与 extension 权威契约。
它约束后续编译器实现；不声称 typed compiler 已经存在。

当前源码树事实仍以
[`docs/kernel-architecture/`](../../kernel-architecture/zh/README.zh.md)为权威。当前树具有
独立的 `GraphDefinition`/YAML ingestion、request-local `ComputePlan`、
`FullTaskGraph` cache key、正式 Value/artifact cache 与 operation ABI v1；尚未实现
`WorkflowDocument`、`OperationSemanticTraits`、compiler IR、compiler digest、compiler
`ExecutionPlan` 或 typed plan cache。

路线顺序仍以
[K1 至 K5](../../roadmap/zh/Next-Stage-Execution-Plan.zh.md)为权威。#199 定义 trait
字段，#200 定义 source-document schema 与 legacy importer，#201/#202 实现 no-optimization
compiler path 与 identities，K4 只在差分等价性成立后删除 legacy planner。

## 权威与范围

| 问题 | 权威 |
| --- | --- |
| 版本/canonical/digest/cache 契约 owner | Photospider kernel |
| 当前 source input | `GraphDefinition` 加 configured YAML adapter |
| 未来持久 compiler source | `WorkflowDocument` |
| 未来 derived authorities | `SemanticGraphIR`、`OptimizedGraphIR`、`ExecutionPlan` |
| 未来 executable-plan commit authority | Kernel compiler/planner，接入现有 `ComputeRun` 与 execution owners |
| 未来 plan-cache persistence | Kernel-owned、可丢弃的 derived cache |
| Non-owner | `photospider-daemon`；IPC v2 不是 compiler-schema authority |

本 K1 切片只冻结 identity 与规则。它不定义 trait 字段、document/IR/plan 字段、compiler
API、runtime status 类型、resource limits、optimizer legality、release/signing policy、
protocol v3 或 daemon schema。Operation ABI v1 保持 exact-size C records 与 entry points。

## 版本 identity registry

Compiler `VersionIdentity` 是以下 tuple：

```text
(identity: ASCII [a-z0-9.-]+, major: uint32 > 0, minor: uint32)
```

Identity 与 version 都具有权威性。不同 identity 下相同的数字版本没有任何关联。

| Contract | Identity | K1 version | 接受的 producer -> consumer | Unknown/unsupported | Downgrade | Breaking-change effect |
| --- | --- | --- | --- | --- | --- | --- |
| Canonical bytes | `photospider.compiler-canonical-bytes` | `1.0` | 仅 `1.0 -> 1.0` | payload decode 前拒绝 | 永不写出 | 使所有 canonical object、digest、key 与 record 失效 |
| WorkflowDocument | `photospider.workflow-document` | `1.0` | 仅 `1.0 -> 1.0` | 字段解释前拒绝 | `UnsupportedDowngrade` | 命名单向 source migration；只保留当前 writer |
| OperationSemanticTraits | `photospider.operation-semantic-traits` | `1.0` | 仅 `1.0 -> 1.0` | fail closed 为 `Unknown`；不做乐观优化 | `UnsupportedDowngrade` | 重新发布/迁移 sidecar；重建 derived artifacts |
| SemanticGraphIR | `photospider.semantic-graph-ir` | `1.0` | 仅 `1.0 -> 1.0` | 拒绝 | 永不写出 | 丢弃并从当前 source 重新 lowering |
| OptimizedGraphIR | `photospider.optimized-graph-ir` | `1.0` | 仅 `1.0 -> 1.0` | 拒绝 | 永不写出 | 丢弃并重新优化 |
| Planner behavior | `photospider.planner-contract` | `1.0` | 仅 `1.0 -> 1.0` | 拒绝 | 永不写出 | 使 plan 与 plan-cache namespace 失效 |
| ExecutionPlan | `photospider.execution-plan` | `1.0` | 仅 `1.0 -> 1.0` | scheduling 前拒绝 | 永不写出 | 丢弃并重新 planning |
| SemanticGraphDigest | `photospider.semantic-graph-digest` | `1.0` | `1.0 -> 1.0`，SHA-256 | 拒绝 domain/version/algorithm | 永不写出 | 重新计算；使下游 identity 失效 |
| OptimizedGraphDigest | `photospider.optimized-graph-digest` | `1.0` | `1.0 -> 1.0`，SHA-256 | 拒绝 domain/version/algorithm | 永不写出 | 重新计算；使 plan identity 失效 |
| ExecutionPlanDigest | `photospider.execution-plan-digest` | `1.0` | `1.0 -> 1.0`，SHA-256 | 拒绝 domain/version/algorithm | 永不写出 | 重新计算；stale plan 不得执行 |
| PlanCacheKey | `photospider.plan-cache-key` | `1.0` | `1.0 -> 1.0`，SHA-256 | incompatible miss | 永不写出 | 选择新的 key namespace |
| PlanCacheRecord | `photospider.plan-cache-record` | `1.0` | 仅 `1.0 -> 1.0` | incompatible miss；record 失去资格；重建 | 永不写出 | 丢弃 record；不迁移 plan bytes |
| Compiler extension | `photospider.compiler-extension` | `1.0` | envelope `1.0 -> 1.0`；extension version 除非列出否则要求 exact | required extension 拒绝；diagnostic 可保持 opaque | `UnsupportedDowngrade` | required change 使相应 digest/key 失效 |

本表是完整的 K1 compatibility manifest。不推断 same-major 或 lower-minor 关系。未来的
compatible edge 必须与规范化 migration/admission 规则和 golden evidence 一起显式加入。

## 现有 identities 不是 aliases

| 现有 identity | 当前角色 | 为什么不是 compiler identity |
| --- | --- | --- |
| Unversioned GraphDefinition/YAML | 当前 graph-document adapter input | #200 拥有其单向导入 WorkflowDocument 的工作；它不是 WorkflowDocument v1 |
| `ComputePlan` | 当前 request-local runtime static plan | 它没有 compiler schema/digest contract，不是 ExecutionPlan v1 |
| `task-shape-v5` | 当前 full-task-graph shape token | 它是 legacy task-graph cache 的一个输入，不是 planner-contract v1 |
| topology generation | 当前 graph topology cache invalidation | 它是 process/runtime topology state，不是 semantic graph identity |
| registry task-shape generation | 当前 callback-shape invalidation | 它是 process registry state，不是 trait schema 或 planner identity |
| operation implementation identity | 当前 selected implementation identity | 它将成为 plan included input，不是 plan/schema version |
| graph-cache manifest v2 | 当前 named-Value disk transaction record | 它持久化 computed Values，不持久化 compiler plan |
| Descriptor/Content/Layout digests | 当前 Value/artifact identities | 它们回答 Value 问题，不回答 graph/plan identity 问题 |
| Photospider package 0.1.0 | Installed package compatibility | Package compatibility 不会接纳 compiler bytes |
| operation ABI v1 | Installed exact-size pure-C DSO contract | Traits 保持为独立 engine registry/sidecar contract |
| IPC protocol v2 | Daemon wire contract | 它不携带 internal document、IR、plan、digest 或 plan-cache schema |

## Compiler Canonical Encoding v1

所有 framing integer 都是 unsigned big-endian。Canonical object 使用以下精确 layout：

```text
"PSCC"
u16 profile_identity_size || profile_identity_ascii
u32 profile_major || u32 profile_minor
u16 contract_identity_size || contract_identity_ascii
u32 contract_major || u32 contract_minor
u64 payload_size || canonical_value_payload
```

Profile 是 `photospider.compiler-canonical-bytes@1.0`。Identity bytes 是匹配
`[a-z0-9.-]+` 的非空 ASCII。每个 declared length 必须与可用 bytes 精确相等，payload
只包含一个 value，任何 trailing bytes 都属于 malformed。

### Canonical value tags

| Tag | Value | 后续 bytes |
| --- | --- | --- |
| `00` | null | 无 |
| `01` | false | 无 |
| `02` | true | 无 |
| `10` | signed integer | 精确 two's-complement `i64` bits，big-endian |
| `11` | unsigned integer | 精确 `u64` bits，big-endian |
| `12` | binary32 | 精确 IEEE-754 32-bit bits，big-endian |
| `13` | binary64 | 精确 IEEE-754 64-bit bits，big-endian |
| `20` | UTF-8 string | `u64` byte length，随后是 well-formed UTF-8 bytes |
| `21` | byte string | `u64` byte length，随后是 exact bytes |
| `30` | sequence | `u64` item count，随后按 logical order 编码 values |
| `31` | map | `u64` pair count，随后是 string-key/value pairs |

Map key 是 canonical UTF-8 string，必须唯一，并按 raw UTF-8 key bytes 严格递增。
Contract field name 使用 ASCII。缺失 map key 与显式 present null 不同。Schema-specific
normalization 在编码前拒绝 unknown core fields 与 malformed values。Ordered data 保留
logical order；schema 定义的 unordered collection 按完整 canonical item bytes 排序。

本 encoding layer 保留精确 floating-point bits。后续 field schema 必须在编码前显式
normalize 任何 numeric equivalence；K1 不会静默决定属于后续 trait/document 工作的
`-0`、NaN、infinity、precision 或 promotion semantics。

Implementation 必须在分配完整 payload 前校验 magic、identities、versions、lengths、checked
arithmetic、UTF-8、tags、map order/uniqueness、schema，以及单独冻结的 per-contract
resource bound。`u64` framing range 不是 allocation allowance。

YAML/JSON spelling、whitespace、map insertion order、C++ memory、padding、host
endianness、pointer、handle 与 process identity 永远不是 canonical bytes。

## Digest framing 与 domain separation

每个 compiler digest 都对以下精确 preimage 计算 SHA-256：

```text
"PSDG"
u16 domain_identity_size || domain_identity_ascii
u32 domain_major || u32 domain_minor
u16 algorithm_size || "sha-256"
u64 canonical_object_size || canonical_object_bytes
```

External spelling 是：

```text
<domain>@<major>.<minor>:sha256:<64-lowercase-hex>
```

`SemanticGraphDigest`、`OptimizedGraphDigest` 与 `ExecutionPlanDigest` 使用各自独立的
registry row。`PlanCacheKey` 在自身 domain 下使用同样的 framed hash construction。
只比较 raw 32 digest bytes 是无效的，因为 domain、version、algorithm、canonical profile
与 owning contract 都是 typed identity 的组成部分。

### Golden framing fixture

`semantic-empty-map-envelope-v1` 是 encoding fixture，不声称 empty map 是有效
SemanticGraphIR。它的 payload 是 canonical empty map：

```text
31 0000000000000000
```

完整的 106-byte object 是：

```text
50534343002470686f746f7370696465722e636f6d70696c65722d63616e6f6e
6963616c2d62797465730000000100000000001d70686f746f7370696465722e
73656d616e7469632d67726170682d697200000001000000000000000000000009
310000000000000000
```

在四个 domain 下对同一个 object 计算 hash，得到：

| Domain | SHA-256 |
| --- | --- |
| `photospider.semantic-graph-digest@1.0` | `1f64d517d05dfe9f8b79aa5478d3dd28b41c565fa76b5959c6f5cab400f30abd` |
| `photospider.optimized-graph-digest@1.0` | `a4e91909ffcebae8e7571e390a64b9850dd730b214ebd7791dfbf3acb5849f73` |
| `photospider.execution-plan-digest@1.0` | `b6d04d19fd53db3e814d843ffb8859bc807e32bf21d621115fe1b9ee9cf58524` |
| `photospider.plan-cache-key@1.0` | `bc74be910cb5ff335d062ec7e1626b5363a1cabac5ccae27fdeb21be1e4dc988` |

不同输出构成 domain-separation evidence。它们不验证未来 graph schema。

## Checked identity chain

后续 schema 必须绑定以下精确 provenance chain：

```text
WorkflowDocument canonical bytes
  -> SemanticGraphIR(version, source version, resolved traits/extensions)
  -> SemanticGraphDigest
  -> OptimizedGraphIR(version, semantic digest, pass-pipeline identity)
  -> OptimizedGraphDigest
  -> ExecutionPlan(version, optimized digest, planner identity, target facts)
  -> ExecutionPlanDigest
```

每个 consumer 在使用前校验自身 identity 与所有 required upstream identities。Stale
semantic digest 不得进入优化，stale optimized digest 不得进入 planning，stale plan
不得执行。即使第一版 optimization pipeline 是 identity，这三个 digest 仍相互独立。

## Plan-cache key 契约

语义规则是：所有可能改变 plan bytes 的事实都要 included；所有 excluded facts 都必须不可能
改变 plan bytes。

| Included | Excluded |
| --- | --- |
| Canonical-profile、key、digest-domain、planner、trait、IR 与 plan schema versions | Semantic identity 建立后的 raw source spelling |
| `SemanticGraphDigest` 与 `OptimizedGraphDigest` | Comments、source locations、display names、diagnostic extensions |
| Effective trait snapshot identity | 单独的 Graph revision；request、Run、session、trace 与 progress ids |
| Selected operation/package/implementation identities | Timestamps、cancellation state、queue occupancy、worker selection |
| Required semantic/planning extension identities、codecs 与 payload digests | 已声明不影响 plan shape 的 dynamic payloads |
| Pass-pipeline identity 与影响 plan 的 compiler options | Cache path/root、LRU/eviction state、persistence receipts |
| 影响 lowering 的 static inputs | Daemon delivery/job state |
| 用于塑造 plan 的 target/backend/device capability identity | Native pointer、allocation、binding、fence、lease、residency |

如果 excluded fact 后来被证明会改变 `ExecutionPlan` bytes，则之前的分类错误。Owning
identity 必须变化，该事实必须成为 included versioned input，现有 records 必须失效，而且在
回归证据存在前必须禁止 reuse。

当前 `full_task_graph_cache_key()` 仍是独立 runtime cache key。它不是 K1 plan key，不能通过
rename 得到提升。

## Plan-cache record 与 invalidation

未来 plan-cache record 携带：

- `photospider.plan-cache-record@1.0`；
- 精确 typed `PlanCacheKey`；
- `photospider.execution-plan@1.0` canonical plan bytes；
- typed `ExecutionPlanDigest`；
- 独立验证所需的每个 included identity。

Lookup 在 plan decode 前验证 framing 与所有 identities。Exact match 可以 hit。任何 unknown
version、missing required extension、key/input mismatch、stale upstream digest、canonical-byte
failure 或 plan-digest mismatch 都属于 `IncompatiblePlanCacheRecord`：lookup 报告 miss、使完整
record 失去 reuse 资格，并从当前 source 重建。Physical eviction 可以 best effort。不存在
partial hit、migrated plan 或 stale runtime fallback。

## Directed compatibility 与 one-way migration

Compatibility 是 directed relation：

```text
(producer identity/version) -> (consumer identity/version)
```

只有列出的 edge 合法。K1 对 core contracts 只列出 exact `1.0 -> 1.0`。Same major、older
minor 或 newer minor 都不会自动兼容。

当前 writer 只写当前 version。所有 write downgrade 都以 `UnsupportedDowngrade` 失败；绝不
静默丢弃字段或 extensions。

Durable `WorkflowDocument` 与 external trait sidecar 以后可以具有命名、确定、有界的单向
migrator。它校验完整 old object，生成当前 canonical bytes，原子 commit 当前 authority，且不再
写出旧 version。Old reader 只存在于 bounded migrator 内，不是并行 durable API。

Compiler IR、plan、digest、key 与 cache record 都是 derived。它们只会被丢弃并重建，不会被
migration 或 reinterpretation。Unversioned GraphDefinition/YAML importer 由 #200 所有。
临时 legacy/new planner differential path 由 #201/#202 所有，并以 K4 单向 authority cut 结束。

Software rollback 不是 data downgrade。只有 older software 已经为可能遇到的每种 durable input
提供显式 compatible read edge 时，source revert 才安全；否则必须 fail closed。Parent #196
继续拥有更广泛的 operational rollback policy。

## Compiler extension identity

每个 extension entry 使用 `photospider.compiler-extension@1.0` 并携带：

```text
(owner, name, major, minor, effect, canonical_codec_identity,
 canonical_payload)
```

`owner` 与 `name` 是非空 ASCII tokens；pair 在一个 object 内唯一。`major` 非零。
`effect` 只能是 `semantic`、`planning` 或 `diagnostic`。

- Semantic 与 planning extensions 是 required。Identity、version、codec 与 canonical payload
  进入对应 canonical object、digest chain 与 plan key。Unknown 或 unsupported required
  extension 拒绝 compilation 或 cache reuse。
- Diagnostic extensions 可在 source-document boundary 作为 opaque canonical bytes 保留。
  它们不进入 semantic、optimized、plan 或 plan-cache identities，也不会被解释为 internal
  IR fields。
- Duplicate 或 conflicting extension identities 会拒绝完整 object。

Source document 是 opaque-preservation authority。Internal IR 与 daemon IPC 不是 extension
archive。未来 public explain/API view 必须定义独立 versioned projection，不得 serialize compiler
IR。

## Fail-closed reason taxonomy

K1 冻结稳定 reason names，而不是 public enum 或 numeric ABI：

| Reason | Boundary |
| --- | --- |
| `MalformedCanonicalBytes` | framing、length、UTF-8、tag、order、duplicate、arithmetic、trailing bytes |
| `UnknownContractIdentity` | 未识别 core contract 或 extension envelope |
| `UnknownVersion` | 已知 identity，但 version 不在 supported inventory |
| `UnsupportedVersion` | 识别 identity/version，但不存在 directed admission edge |
| `UnsupportedDowngrade` | writer 被要求写出旧 version |
| `ConflictingVersionIdentity` | envelope 与 required embedded provenance 不一致 |
| `UnknownRequiredExtension` | semantic/planning extension 或 codec 无法解释 |
| `StaleDerivedIdentity` | IR/plan provenance 不同于当前 validated upstream identity |
| `IncompatiblePlanCacheRecord` | 任意 key/record/plan/version/digest/cache mismatch |

后续 implementation 可把这些 reasons 映射到 engine-owned status type。不得把它们变成成功的
optimistic fallback，也不得要求 caller 解析 diagnostic text。

## 评审示例

| Example | Input | Required result |
| --- | --- | --- |
| Exact compatible | producer 与 consumer 都是 `SemanticGraphIR@1.0` | 接纳 version，随后验证 bytes 与 dependencies |
| Same-major unknown | reader 支持 `WorkflowDocument@1.0`，收到 `1.1` | `UnknownVersion`；不探测字段 |
| Breaking source migration | 列出的旧 WorkflowDocument -> 当前 | 校验完整 old，原子写出 current，不保留 dual writer |
| Derived break | planner contract 变化 | 丢弃 plan/digests/cache record 并重建 |
| Unsupported downgrade | 当前 writer 被要求写出旧 document/sidecar | `UnsupportedDowngrade`；不丢字段 |
| Malformed bytes | duplicate map key 或 trailing byte | digest/object publication 前返回 `MalformedCanonicalBytes` |
| Conflicting identity | plan envelope 与 embedded planner identity 不一致 | `ConflictingVersionIdentity`；两者都不选 |
| Domain separation | 相同 canonical bytes 进入三个 digest domains | 三个 typed、不可互换的 hashes |
| Exact cache hit | key/record/plan/digest/所有 included identities 匹配 | 可返回 validated plan |
| Cache invalidation | trait/implementation/extension/target/version 变化 | incompatible miss；record 失去 reuse 资格并重建 |
| Unknown semantic extension | required identity/version/codec 不可用 | `UnknownRequiredExtension`；不做 optimistic lowering |
| Unknown diagnostic extension | well-framed source-only diagnostic payload | opaque 保留；排除在 compiler identity 外 |

## K1 evidence 适用性

| Evidence category | K1 disposition |
| --- | --- |
| Accepted authority/version/schema | Applicable：本文、ADR 0014、英文 OpenSpec |
| Named fixture and independent oracle | Applicable：`semantic-empty-map-envelope-v1`；独立 SHA-256 复算 |
| Compatible/malformed/unknown/conflicting/stale | Applicable：以上 normative review fixtures |
| Canonical bytes/digest/identity/cache invalidation | Applicable：framing、golden vectors、key/record rules |
| One-way migration/no dual authority | Applicable 作为契约；implementation 属于 #200/#201/#202 |
| Conservative fallback/fail-closed | Applicable：traitless ABI-v1 operation 保持 `Unknown`；unknown required identity 拒绝 |
| Resource exhaustion | N/A runtime evidence：没有 decoder/allocation；implementing Issues 必须在 allocation 前冻结并测试 bounds |
| Cancellation/concurrency | N/A：compile/execute path 不变；#201/#202 拥有 runtime integration evidence |
| Runtime persistence/recovery | N/A：未创建 plan-cache files；后续 cache implementation 拥有 fault 与 recovery |
| Package consumer/platform matrix | N/A：未修改 installed/public/package boundary |
| Clean configure/full build/CTest | Not run：没有 runtime 或 source behavior change |
| Daemon downstream gate | N/A：未修改 installed package 或 IPC boundary |
| English/Chinese/OpenSpec/tracking | Applicable，在同一 change 中完成 |
| Live Issue/Project/PR/CI/review | 审计 Issue/Project state；remote mutation、PR、CI 与 review 是本地切片之外的 delivery gates |

## 后续 Issues 的 adoption constraints

- #199 只能在 `photospider.operation-semantic-traits@1.0` 内定义 trait 字段；不得改变
  operation ABI v1，也不得为 `Unknown` 推断 optimistic behavior。
- #200 可定义 `WorkflowDocument@1.0` 与单向 legacy importer。首版 document 保持一个
  function、一个 region、一个 block，且 acyclic。
- #201/#202 实现 canonical no-optimization path、identities、stale rejection、plan cache，
  并在真正引入 installed API 时提供 installed-consumer evidence 和 differential equivalence。
- K4 只删除 legacy planner 一次。不得保留永久 wrapper、alias、dual planner 或 dual
  document authority。
- 这些 kernel-owned objects 均不得成为 IPC v2 或 daemon schema。
