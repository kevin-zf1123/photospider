# ADR 0012：Operation Plugin 使用独立版本化 Pure-C ABI

## 状态

本 ADR 于 2026-08-11 被接受为 GitHub Issue #101 / S-10 的目标 contract。它裁定替代
边界，但不表示新 header、loader、SDK、plugin 或 test 已经存在。在后续一次 breaking
migration 实现并验证 v1、且完整删除 v2 之前，operation ABI v2 仍是当前 installed
interface。

英文架构与 OpenSpec 文档是权威来源。实时交付与远端门禁状态继续由 Issue、Project、
active OpenSpec change 与 `development_tracking.md` 记录。

## 背景

当前 operation DSO 导出 C-linkage symbol `register_photospider_ops_v2`，但 registrar 与
callback 会交换 `std::function`、standard-library value、public C++ object、ownership、
allocator、RTTI 与 exception。Symbol 能检测一个期望 generation，却不能使 data boundary
兼容 C 或在 C++ toolchain 之间可移植。

Photospider 另有两个独立 pure-C plugin family：

- data-definition provider ABI v3 发布 Schema/Facet/Layout definition 与 bounded
  semantic definition callback；
- policy plugin ABI v1 对 immutable Host-admissible scheduler candidate 排序。

两者都不拥有 operation configuration、inference、dirty/forward Region propagation、
dependency construction 或 execution。把替代项称为“operation provider v3”会静默地
把这些权限附到错误 family，并造成无关版本号看似兼容。

当前 operation loader 已有重要 lifetime behavior：staged registration、atomic
publication、per-slot revision/predecessor identity、middle-generation splice、reverse
unload 与 in-flight callback DSO lease。替代边界必须保留这些性质，而不是保留 C++ ABI。

ADR 0011 建立了独立 security-domain 方向。Operator-trusted DSO 可以在 Host process
执行；tenant-supplied CPU code 最终必须在 isolated plugin runtime 执行。Pure C 是显式
可验证 record 的必要条件，却不是 process isolation。Issues #102、#103、#104 已分别
拥有 wire protocol、supervision、trust/resource policy。

## 决策

### Family、discovery 与受支持 profile

替代项是独立的 **operation-plugin ABI v1**。其未来 self-contained C11/C++17 header
为 `photospider/plugin/operation_plugin_api.h`，ABI 值为 1，并且只有两个 discovery
export：

```c
uint32_t ps_operation_plugin_get_abi_version(void);
ps_operation_status_v1 ps_operation_plugin_get_api_v1(
    ps_operation_plugin_api_v1 *api);
```

C++ 中使用 `extern "C"` 与 `noexcept`。所有 ABI callback 使用 `PS_OPERATION_CALL`：
platform C calling convention，Windows 为 `__cdecl`。Host 只完成 numeric handshake
后才请求 root API。

V1 支持 8-bit byte、4-byte `uint32_t`、8-byte `uint64_t`、8-byte data/function
pointer、natural 8-byte pointer/`uint64_t` alignment、Host-process endianness 和匹配
platform C convention。Packed、over-aligned、32-bit、foreign-endian 或不同 convention
不兼容。

### Exact record 与独立版本化 suite

只有 Diagnostic 至 Tile 这 20 个 versioned semantic record 以四个 `uint32_t` 开头：
`struct_size`、`struct_kind`、`struct_version`、`flags`。Plain fixed identity/handle、byte-
view、digest、array-reference、configuration-value、axis-range helper 不携带 record header。
Root API 与 suite table 使用各自的 root/suite prefix。每个 suite table 以 `struct_size`、
`suite_version`、`flags`、`reserved0` 开头。V1 要求 exact size 而不是 minimum prefix：unknown kind/version/flag、非零
reserved、short/long record、unknown tail、wrong stride、misalignment、arithmetic/range
overflow 都 fail closed。新增字段需要新 owning-suite version 或 operation ABI v2。

Root API 为 96/8 byte/alignment，依次以 `struct_size`、`abi_version`、`flags`、
`reserved0` 开头，随后包含 permanent 128-bit plugin identity、bounded implementation
version、opaque plugin context、`query_suite`、`destroy_plugin`
与 zero-required reserved pointer word。成功返回 null context 仍有效，且仍必须精确尝试
一次 destroy。

每个 v1 suite table 为 64/8；冻结 ID 与 callback inventory 如下：

| ID | Suite | 要求 | 有序 callback |
| ---: | --- | --- | --- |
| 1 | Definition | Required | operation count/get；implementation count/get |
| 2 | Configuration | Required | validate；create context；destroy context |
| 3 | Inference | Required | infer output plan |
| 4 | Region | Required | backward dirty；forward active-edge propagation |
| 5 | Dependency | 任一 implementation 声明 data dependence 时 required | build dependency record |
| 6 | Execution | Required | synchronous monolithic；synchronous tiled |

Required callback 不能为 null。仅在没有 copied implementation 声明某 execution shape
时，对应 callback 才可为 null。Unknown suite 返回 `UNSUPPORTED`；缺失或 malformed
required/declared suite 会在 publication 前拒绝整个 candidate。

Host 预清零每个 Host-prepared output 并设置其 exact prefix；plugin 保留 prefix，只填写
declared remaining field。Sink record 携带 plugin-authored complete prefix，由 Host 首先验证。

全部 suite/record version 都是 1。封闭 numeric domain 包括：record kind 1 至 20，依次为
Diagnostic、OutputSink、ConfigurationNode、ConfigurationView、OperationDescriptor、
ImplementationDescriptor、PortDescriptor、ValueDescriptor、FacetView、BufferView、
ValueView、InputBinding、OutputPlan、MutableOutputBinding、Invocation、RegionAtom、
RegionSetView、RegionBinding、DependencyRecord、Tile；configuration kind 1 Null 至
8 Object；direction 1 Input/2 Output；intent bit 1 HP/2 RT；shape bit 1 Monolithic/
2 Tiled；device 1 CPU；access bit 1 Read/2 Write；behavior bit 1 SideEffect/
2 DataDependent；Region outcome 1 Exact 至 4 Unknown；Region atom 1 Whole 至
4 TensorSlice；sink channel 1 Diagnostic 至 4 DependencyRecord。零值 invalid/absent，
unknown value/bit 失败，boolean 为 0 或 1。ValueView flag bit 1 是 PayloadAvailable；其他
semantic-record flag 与所有 root/suite flag 在 v1 中均为零。

Callback parameter order 同样封闭：plugin context 位于首位，之后为精确 operation/
implementation/invocation/configuration identity/view，再之后为 pointer/count/exact-stride
input、demand、output 或 tile argument，Host sink 最后。即使 configured context 为 null，
Configuration destroy 仍接收 operation 与 implementation identity。Sink 为
`emit(host_context,channel,records,count,stride)`。Host 不公开 cancellation callback：它在
entry 前、sink call 时、return 后检查，可以 normalize 为 `CANCELLED` 并丢弃 late result。

V1 中不存在 allocator、registry、Host service、Graph、Run、scheduler、cache、executor、
resource ledger/token、device service、filesystem、artifact、credential、logging、thread
或 dynamic-symbol callback。

### Opaque identity、handle 与 context

Plugin、operation、implementation、port、Schema、Facet、Layout identity 是 publisher-
assigned nonzero permanent 128-bit definition identity，不能复用于不同语义或 layout。
Value、edge、allocation、binding、site、Region identity 使用相同 16-byte carrier，但表示
Host-minted process-local runtime identity；分别随 logical value、Graph revision、allocation、
binding/write grant 或 invocation-snapshot owner 失效，不是 durable/wire identity。

Host 以不同 plain helper type 生成 nonzero、unpredictable 128-bit generation/invocation
handle。它们只是 process-local correlation handle，不是 semantic identity、pointer、lookup
API、capability、durable artifact identity、resource token 或 wire value。Invocation-scoped
callback 同时接收两者；definition、configuration-lifetime、root-query、destroy callback 由
精确 DSO generation lease 与适用的显式 identity/context 绑定，没有虚构的 invocation
handle。Stale/mismatched output 为 `INVALID_DESCRIPTOR`，且不发布任何内容。

Plugin context 与 configured-operation context 是 plugin-owned opaque `void *` round-trip
value。Host 绝不 dereference/free。Create 成功时可返回 null，仍获得一次 destroy
obligation；create 失败不转移 obligation。Context 不跨 generation、operation 或
implementation 移动。

Sink `host_context` 是独立 Host-owned callback-local round-trip token。Plugin code 只能把
它传回 `emit`；不得 dereference、free、retain 或把它当作 semantic identity。

### Descriptor catalogue、bound 与 ownership

OpenSpec design 为未来 header 冻结 ordered field group 与以下 natural size/alignment：

| Layout category | Size/alignment |
| --- | --- |
| record header | 16/4 |
| identity、generation/invocation handle、immutable/mutable bytes、exact-stride array reference、configuration value、axis range | 16/8 |
| SHA-256 digest | 32/8 |
| diagnostic、output sink、configuration view、Region-set view | 48/8 |
| configuration node、facet view、tile | 64/8 |
| buffer view、input binding、Region binding | 80/8 |
| output plan、mutable output binding、invocation、Region atom、dependency record | 96/8 |
| port descriptor | 112/8 |
| operation descriptor、value view | 128/8 |
| value descriptor | 192/8 |
| implementation descriptor | 192/8 |
| root API / 每个 suite table | 96/8 与 64/8 |

Header 必须在 C11/C++17 中 assert 这些值。它可以添加规定的
`ps_operation_*_v1` spelling prefix，但不能在不修订本决策时改变冻结 numeric、parameter/
field order、size、alignment、ownership 或 meaning。OpenSpec design 冻结每个 helper 与
semantic-record 的 field type/byte offset，以及 96-byte root 与每个 64-byte suite slot。
特别地，128-byte operation descriptor 在 offset 96/112 各保存一个 16-byte input/output
port pointer/count/stride helper，不在其他位置重复 count。因此所有 size 都能在冻结 64-bit
profile 下机械实现，不是仅凭 field-group 估算的目标。

所有 pointer/count/stride value 只在一个同步 callback 或 sink emission 中借用。Null
精确对应 zero count；stride 等于 exact element size。Receiver 在 dereference 前验证
alignment、multiplication、base/offset、subrange、aggregate bound 与 write overlap。Host
在 return 前 deep-copy accepted metadata/result，不保留 plugin pointer。

Operation descriptor 包含 permanent identity、canonical type/subtype/display、borrowed
exact-stride port array、flag、configuration-schema identity。Type/subtype 是不含 NUL 或
`:` 的 nonempty UTF-8，并组成唯一 `type:subtype` key。Port/implementation name 是不含
NUL 的 nonempty UTF-8；display/exclusive key 可为空，非空时是不含 NUL 的 UTF-8。Name
不 normalize、case-fold 或截断。Implementation descriptor 包含 parent/permanent identity、
HP/RT intent、monolithic/tiled shape、CPU device profile、tile/access/side-effect/data-
dependence fact、reentrancy、maximum parallelism、retained/scratch byte、cost、optional
exclusive key。Host 验证并把 callback、metadata、identity、source generation、revision
作为一个 slot 发布。

Configuration 是 Host-owned immutable tree，node 为 null、boolean、signed 64-bit
integer、binary64、UTF-8 string、bytes、array、object；不是 YAML、`ParameterMap` 或 C++
variant。Object key 唯一并按 unsigned-byte lexicographic order 排列。

Value record 区分 Schema/Facet/Layout identity、structural version、logical revision、
allocation/binding identity 与 descriptor/content/layout digest。Inference、Region、
dependency 接收 payload pointer 已清空的 descriptor-only view。只有 execution 接收
payload，只有显式 Host mutable-output grant 允许写入。

V1 structural maximum 为：

- 每个 canonical name/exclusive key 128 byte；
- 每个 implementation version/diagnostic 4 KiB；
- 每 plugin 4,096 operation，每 operation 256 implementation；
- input/output port 各 256；
- rank 16，每 value 64 facet 与 64 buffer；
- 4,096 configuration node、depth 64、1 MiB configuration byte；
- 每 result 64 Region atom；
- 每 invocation 4,096 dependency record。

Structural excess 为 `TOO_COMPLEX`，available-capacity failure 为 `OUT_OF_MEMORY`，
任何内容都不得静默截断。

### Planning 与 execution authority

Inference 在 Host allocation 前为每个 declared output port 生成 complete immutable
output plan。Plan 固定 Schema/Facet/Layout identity、rank/extent、buffer、size、access；
execution 不得替换或扩大它。

Region v1 明确提供 backward dirty 与 forward active-edge propagation。Outcome 为 Exact、
Whole、Empty、Unknown；atom 为 Whole、Empty、ImageRect、TensorSlice，coordinate 半开且
受检查。Loadable plugin 不能依赖 identity fallback。

声明 data dependence 的 implementation 必须提供 Dependency v1。Bounded record 把 output
port/site/region fact 映射到 input edge/region fact。Host 在 cache 前验证并复制所有
identity、rank、extent、generation、invocation。

Execution 同步且只支持 CPU-addressable binding。Monolithic callback 接收完整 immutable
input 与 Host-owned mutable output；tiled callback 还接收 checked tile。Plugin 只能写
granted range，不得改变 descriptor、binding、extent、readiness、identity、access 或
ownership fact。

V1 没有 native device handle、device-resident binding、fence、asynchronous completion、
retained invocation owner 或 delayed sink。Private device work 只有在 return 前同步 stage
到 Host CPU binding 时有效。仓库 Metal 示例必须使用该模式或在 v2 删除前移到 Host-
private adapter 后面。Native/async execution 需要未来独立版本化 suite/ABI。

### Host-owned output 与精确 lifetime

V1 没有 allocator callback。Definition string 在 return 前复制；execution 写 Host-owned
buffer；inference、Region、dependency、diagnostic 使用一个 48-byte callback-local Host
output sink。其单一 emit function 只接受适合当前 callback 的封闭 channel 与 exact-stride
record。Host 同步验证/复制。即使 plugin 忽略并返回 success，第一次 sink failure 仍 sticky。

Host destroy Host memory，plugin destroy plugin memory。每个成功创建的 root/configured
context 在 dependent call 完成后、精确 generation/DSO lease 下得到一次 destroy attempt。
Failure 只记录、不 retry。

Status type 为 `uint32_t`，精确值：

| 值 | Status |
| ---: | --- |
| 0 | `OK` |
| 1 | `INVALID_ARGUMENT` |
| 2 | `OUT_OF_MEMORY` |
| 3 | `UNSUPPORTED` |
| 4 | `INVALID_DESCRIPTOR` |
| 5 | `TOO_COMPLEX` |
| 6 | `CANCELLED` |
| 7 | `FAILED_PRECONDITION` |
| 8 | `INTERNAL_ERROR` |

Unknown status 是 ABI fault 并映射为 Host internal failure。一个 non-OK callback 可以
emit 一条最多 4 KiB、被复制的 UTF-8 diagnostic；text 不改变 status/authority。Exception、
unwind、`longjmp`、signal-recovery object 或 language-runtime value 都不跨 DSO。C++
wrapper 在 DSO 内分别把 `std::bad_alloc`、invalid caller input、其他可捕获 failure 映射
为 `OUT_OF_MEMORY`、`INVALID_ARGUMENT`、`INTERNAL_ERROR`。

### Atomic publication 与 generation-safe retirement

未来 loader 把 numeric、root、suite、descriptor、callback、bound、identity validation
全部执行到一个 shadow generation，完整验证后分配 Host generation 并原子发布。Plugin
绝不直接 mutate `OpRegistry`。

每个 executable slot 拥有完整 callback set、copied descriptor、identity、source
generation、revision、predecessor。Direct Host mutation 与 publication 串行化。Middle-
generation retirement 只 splice 自己拥有的 predecessor，reverse unload 不恢复 unmapped
code。

每个 callback/context 都持有精确 DSO generation lease，直到 sink validation、status
normalization、destroy 完成。Retirement 先移除 publication，再等待 lease，按 reverse
creation/load order destroy，最后 unmap。Plugin code 执行期间不持有 registry、
publication、scheduler、execution lock。

若进程内 callback 永不返回，其 invocation、write grant、context、generation、DSO 可能
永久存活。Cancellation 可以拒绝其 result，但不能虚构 return、destroy、quiescence 或
safe unload。

### Trust boundary 与后续 ownership

Operation-plugin ABI v1 是 operator-trusted in-process compatibility/validation
boundary。它不能阻止 memory corruption、syscall、secret access、thread、crash、hang、
OOM、forged callback 或 Host corruption。

Tenant-untrusted CPU code 不把 pointer-bearing ABI record 用作 IPC：

- Issue #102 负责 pointer-free shared-memory/FD invocation wire record、serialization
  与精确 offset/range/ownership validation；
- Issue #103 负责 authenticated `PluginRuntimeSupervisor` lifecycle、heartbeat/
  deadline、crash/hang/OOM/bad-output containment、restart、reap；
- Issue #104 负责 package allowlist/signature、sandbox/capability 与 enforceable
  resource policy。

本 ADR 不预选其 frame、handle、authentication 或 policy format。Cross-process GPU/
native-handle 工作仍是后续决策。

### 一次完整 breaking migration

Issue #101 只修改文档与 specification。后续实现必须新增 v1 header/SDK/loader/
conformance test、迁移全部仓库 operation 与 installed consumer，然后在同一 migration
release 删除 operation v2，包括 `register_photospider_ops_v2`、
`OperationPluginRegistrar`、public C++ callback contract、v2-only SDK/runtime package
surface、fixture、package assertion 与旧文档。

不保留 wrapper、alias、dual loader、forwarding header、v2-to-v1 adapter、missing-tail
interpretation 或 runtime fallback。独立 C11/C++17 consumer 以及 suite、rejection、
ownership、replacement、middle-unload、in-flight-lease test 是删除门禁。Rollback 是回退
完整 migration，不是 v2 mode。

该后续迁移不是 Issue #101 关闭或 decision 归档门禁。中英文 artifact 通过本地验证、fresh
独立 diff 审核、经授权 exact-head PR Integration、finding 已裁定的 fresh Codex exact-head
review、zero unresolved review thread 与 Issue/Project 行政门禁后，可以归档 decision
change 并关闭 Issue #101；此时 v2 仍可为 current、v1 仍可只是 target。

## 后果

- C/C++ extension 作者获得一个明确 operation contract，不再要求共享 C++ allocator/
  ABI/runtime。
- Exact record 与 Host sink 使 validation/ownership 机械化，代价是复制和显式 version
  bump。
- 首代 ABI 有意排除 native/async operation execution，因此不留下 hidden fence、
  completion 或 device-owner contract。
- 当前 strong loader/lifetime behavior 仍是 required property，但 compatibility wrapper
  与 dual registry 不是。
- Pure C 绝不作为 in-process tenant code 安全的证据。

## 被拒绝的替代方案

- **把目标改名为“provider v3”。** Provider v3 已表示 implemented definition-only family。
- **原地扩展 operation v2。** C++ value/ownership 无法通过 symbol-compatible tail 变成
  pure C。
- **复用 policy v1 或 data-provider record。** 它们的 authority、lifecycle、versioning
  有意更窄。
- **接受 minimum-size prefix 与 unknown tail。** 这会允许静默 layout/semantic 分歧。
- **增加 allocator callback。** Host sink/output grant 完全避免 cross-DSO allocation
  ownership。
- **在 v1 加入 native GPU 或 async completion。** 它们的 handle、fence、completion
  owner、cancellation、retirement 需要独立决策。
- **把 pure C 当作 isolation。** 同进程 native code 仍拥有环境进程权限。
- **让 v1 与 v2 长期并存。** Dual publication/restoration 会保留歧义和迁移残留。

## 参考

- [Plugin ABI](../../kernel-architecture/zh/Plugin-ABI.zh.md)
- [Compute Boundaries](../../kernel-architecture/zh/Compute-Boundaries.zh.md)
- [Kernel Evolution](../../roadmap/zh/Kernel-Evolution.zh.md)
- [ADR 0003](0003-process-owned-execution-resources.zh.md)
- [ADR 0008](0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.zh.md)
- [ADR 0011](0011-server-control-plane-workers-and-plugin-runtimes-are-separate-security-domains.zh.md)
- [GitHub Issue #101](https://github.com/kevin-zf1123/photospider/issues/101)
