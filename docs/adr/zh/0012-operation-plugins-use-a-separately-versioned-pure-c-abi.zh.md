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
可验证 record 的必要条件，却不是 process isolation。Issue #102 现在已经实现一个独立
版本化、源码私有的 CPU wire/runtime 切片，但不更改当前 operation ABI v2，也不实现本 ADR
仍为目标态的 operation ABI v1。Issue #103 现在已经围绕该 transport 实现独立版本化、源码
私有的 supervision 组合；Issue #104 继续拥有 trust、sandbox 与可执行 resource policy。两个
切片都不会改变 ABI replacement 决策，也不会提供最终用户 operation loader。

## 决策

### Family、discovery 与受支持 profile

替代项是独立的 **operation-plugin ABI v1**。其未来 self-contained C11/C++17 header
为 `photospider/plugin/operation_plugin_api.h`，ABI 值为 1，并且只有两个 discovery
export。`PS_OPERATION_CALL` 在 Windows 上为 `__cdecl`，在其他平台为 platform C
calling convention；`PS_OPERATION_PLUGIN_EXPORT` 是平台 export/default-visibility
annotation；`PS_OPERATION_NOEXCEPT` 在 C++17 中为 `noexcept`，在 C11 中为空：

```c
#if defined(__cplusplus)
extern "C" {
#endif

PS_OPERATION_PLUGIN_EXPORT uint32_t PS_OPERATION_CALL
ps_operation_plugin_get_abi_version(void) PS_OPERATION_NOEXCEPT;

PS_OPERATION_PLUGIN_EXPORT ps_operation_status_v1 PS_OPERATION_CALL
ps_operation_plugin_get_api_v1(
    ps_operation_plugin_api_v1 *api_out) PS_OPERATION_NOEXCEPT;

#if defined(__cplusplus)
}
#endif
```

Export annotation 只适用于这两个具名 symbol。它们的 resolved function-pointer
type 与每个 ABI callback 都使用 `PS_OPERATION_CALL` 与 C++17 `noexcept`
function type。Host 只完成 numeric handshake 后才请求 root API。

V1 支持 8-bit byte、4-byte `uint32_t`、8-byte `uint64_t`、8-byte data/function
pointer 与每个具名 function-pointer type、natural 8-byte data/function-pointer 与
`uint64_t` alignment、Host-process endianness 和匹配 platform C convention。Packed、
over-aligned、32-bit、foreign-endian 或不同 convention 不兼容。Object pointer、
function pointer 与 integer slot 保持为不同 C type，绝不进行 representation conversion。

### Exact record 与独立版本化 suite

只有 Diagnostic 至 Tile 这 20 个 versioned semantic record 以四个 `uint32_t` 开头：
`struct_size`、`struct_kind`、`struct_version`、`flags`。Plain fixed identity/handle、byte-
view、digest、array-reference、configuration-value、axis-range helper 不携带 record header。
Root API 与 suite table 使用各自的 root/suite prefix。每个 suite table 以 `struct_size`、
`suite_id`、`suite_version`、`flags` 这四个 `uint32_t` 字段开头。V1 要求
exact size 而不是 minimum prefix：unknown kind/version/flag、非零
reserved、short/long record、unknown tail、wrong stride、misalignment、arithmetic/range
overflow 都 fail closed。新增字段需要新 owning-suite version 或 operation ABI v2。

Root API 为 96/8 byte/alignment，依次以 `struct_size`、`abi_version`、`flags`、
`reserved0` 开头，随后包含 permanent 128-bit plugin identity、bounded implementation
version、opaque plugin context、`query_suite`、`destroy_plugin`
与精确的 `uint64_t reserved[3]`。这些 root reserved word 是 integer field，不是
pointer slot。成功返回 null context 仍有效，且仍必须精确尝试
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

`query_suite` 前，Host 把一个具体 64-byte suite 的每个 field 初始化为其 C
semantic zero value——integer field 为零、pointer field 为 null——然后在其 nonnull
`ps_operation_suite_header_v1` 首成员中写入 size 64、requested suite ID、
requested version 与 zero flag。这不假定 byte-zero 就是 null pointer。Plugin 保留这
四个 prefix 字段。Required callback 不能为
null。仅在没有 copied implementation 声明某 execution shape 时，对应 callback
才可为 null。Unknown suite ID 或 version 返回 `UNSUPPORTED`。返回 `OK` 后，
returned size/ID/version/flag mismatch 是 `INVALID_DESCRIPTOR`；Host 在读取任何
callback 前拒绝它。缺失或 malformed required/declared suite 会在 publication 前
拒绝整个 candidate。

`get_api_v1` 前，Host 把具体 96-byte root 的每个 field 初始化为其 C semantic
zero value，并写入 size 96、ABI version 1、zero flag 与 zero `reserved0`；plugin
保留该 prefix。Host-prepared semantic record 与
suite 同样携带其完整精确的 size/kind/version/flag 或 size/ID/version/flag
prefix。Plugin 保留每个 Host-authored prefix，只填写 declared remaining field。Sink
record 携带 plugin-authored complete semantic-record prefix，由 Host 首先验证。

全部 suite/record version 都是 1。Suite ID 精确为 1 Definition、2 Configuration、
3 Inference、4 Region、5 Dependency、6 Execution。封闭 numeric domain 包括：record
kind 1 至 20，依次为
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

下列 typedef prototype 是 normative contract。每个 typedef 都是 function-pointer type，
不是 unprototyped function 或 pseudocode abbreviation。`PS_OPERATION_NOEXCEPT`
使 `noexcept` 在 C++17 中成为 function type 的一部分，在 C11 中展开为空：

```c
typedef uint32_t(PS_OPERATION_CALL *ps_operation_plugin_get_abi_version_fn_v1)(void) PS_OPERATION_NOEXCEPT;
typedef ps_operation_status_v1(PS_OPERATION_CALL *ps_operation_plugin_get_api_fn_v1)(ps_operation_plugin_api_v1 *api_out) PS_OPERATION_NOEXCEPT;
typedef ps_operation_status_v1(PS_OPERATION_CALL *ps_operation_emit_fn_v1)(void *host_context, uint32_t channel, const void *records, uint32_t count, uint32_t stride) PS_OPERATION_NOEXCEPT;
typedef ps_operation_status_v1(PS_OPERATION_CALL *ps_operation_query_suite_fn_v1)(void *plugin_context, uint32_t suite_id, uint32_t requested_version, ps_operation_suite_header_v1 *suite_out) PS_OPERATION_NOEXCEPT;
typedef ps_operation_status_v1(PS_OPERATION_CALL *ps_operation_destroy_plugin_fn_v1)(void *plugin_context, const ps_operation_output_sink_v1 *sink) PS_OPERATION_NOEXCEPT;
typedef ps_operation_status_v1(PS_OPERATION_CALL *ps_operation_get_operation_count_fn_v1)(void *plugin_context, uint32_t *operation_count_out, const ps_operation_output_sink_v1 *sink) PS_OPERATION_NOEXCEPT;
typedef ps_operation_status_v1(PS_OPERATION_CALL *ps_operation_get_operation_fn_v1)(void *plugin_context, uint32_t operation_index, ps_operation_descriptor_v1 *operation_out, const ps_operation_output_sink_v1 *sink) PS_OPERATION_NOEXCEPT;
typedef ps_operation_status_v1(PS_OPERATION_CALL *ps_operation_get_implementation_count_fn_v1)(void *plugin_context, const ps_operation_identity_v1 *operation_identity, uint32_t *implementation_count_out, const ps_operation_output_sink_v1 *sink) PS_OPERATION_NOEXCEPT;
typedef ps_operation_status_v1(PS_OPERATION_CALL *ps_operation_get_implementation_fn_v1)(void *plugin_context, const ps_operation_identity_v1 *operation_identity, uint32_t implementation_index, ps_operation_implementation_descriptor_v1 *implementation_out, const ps_operation_output_sink_v1 *sink) PS_OPERATION_NOEXCEPT;
typedef ps_operation_status_v1(PS_OPERATION_CALL *ps_operation_validate_configuration_fn_v1)(void *plugin_context, const ps_operation_identity_v1 *operation_identity, const ps_operation_configuration_view_v1 *configuration, const ps_operation_output_sink_v1 *sink) PS_OPERATION_NOEXCEPT;
typedef ps_operation_status_v1(PS_OPERATION_CALL *ps_operation_create_configured_context_fn_v1)(void *plugin_context, const ps_operation_identity_v1 *operation_identity, const ps_operation_identity_v1 *implementation_identity, const ps_operation_configuration_view_v1 *configuration, void **configured_context_out, const ps_operation_output_sink_v1 *sink) PS_OPERATION_NOEXCEPT;
typedef ps_operation_status_v1(PS_OPERATION_CALL *ps_operation_destroy_configured_context_fn_v1)(void *plugin_context, const ps_operation_identity_v1 *operation_identity, const ps_operation_identity_v1 *implementation_identity, void *configured_context, const ps_operation_output_sink_v1 *sink) PS_OPERATION_NOEXCEPT;
typedef ps_operation_status_v1(PS_OPERATION_CALL *ps_operation_infer_output_plans_fn_v1)(void *plugin_context, const ps_operation_invocation_v1 *invocation, const ps_operation_configuration_view_v1 *configuration, const ps_operation_array_ref_v1 *input_bindings, const ps_operation_output_sink_v1 *sink) PS_OPERATION_NOEXCEPT;
typedef ps_operation_status_v1(PS_OPERATION_CALL *ps_operation_propagate_region_backward_fn_v1)(void *plugin_context, const ps_operation_invocation_v1 *invocation, const ps_operation_configuration_view_v1 *configuration, const ps_operation_array_ref_v1 *input_bindings, const ps_operation_array_ref_v1 *demanded_output_region_bindings, const ps_operation_output_sink_v1 *sink) PS_OPERATION_NOEXCEPT;
typedef ps_operation_status_v1(PS_OPERATION_CALL *ps_operation_propagate_region_forward_fn_v1)(void *plugin_context, const ps_operation_invocation_v1 *invocation, const ps_operation_configuration_view_v1 *configuration, const ps_operation_array_ref_v1 *input_bindings, const ps_operation_identity_v1 *active_input_edge_identity, const ps_operation_region_set_view_v1 *changed_input_regions, const ps_operation_output_sink_v1 *sink) PS_OPERATION_NOEXCEPT;
typedef ps_operation_status_v1(PS_OPERATION_CALL *ps_operation_build_dependencies_fn_v1)(void *plugin_context, const ps_operation_invocation_v1 *invocation, const ps_operation_configuration_view_v1 *configuration, const ps_operation_array_ref_v1 *input_bindings, const ps_operation_array_ref_v1 *demanded_output_region_bindings, const ps_operation_output_sink_v1 *sink) PS_OPERATION_NOEXCEPT;
typedef ps_operation_status_v1(PS_OPERATION_CALL *ps_operation_execute_monolithic_fn_v1)(void *plugin_context, const ps_operation_invocation_v1 *invocation, const ps_operation_configuration_view_v1 *configuration, const ps_operation_array_ref_v1 *input_bindings, const ps_operation_array_ref_v1 *mutable_output_bindings, const ps_operation_output_sink_v1 *sink) PS_OPERATION_NOEXCEPT;
typedef ps_operation_status_v1(PS_OPERATION_CALL *ps_operation_execute_tiled_fn_v1)(void *plugin_context, const ps_operation_invocation_v1 *invocation, const ps_operation_configuration_view_v1 *configuration, const ps_operation_array_ref_v1 *input_bindings, const ps_operation_array_ref_v1 *mutable_output_bindings, const ps_operation_tile_v1 *tile, const ps_operation_output_sink_v1 *sink) PS_OPERATION_NOEXCEPT;
typedef void(PS_OPERATION_CALL *ps_operation_reserved_callback_fn_v1)(void) PS_OPERATION_NOEXCEPT;
```

48-byte output sink 在 16 存放 `void *host_context`，在 24 存放
`ps_operation_emit_fn_v1 emit`，在 32 存放 `uint64_t reserved[2]`。96-byte root
在 56 存放 `ps_operation_query_suite_fn_v1 query_suite`，在 64 存放
`ps_operation_destroy_plugin_fn_v1 destroy_plugin`，在 72 存放
`uint64_t reserved[3]`。Definition 在 16/24/32/40 存放四个 typed callback，在 48
存放 `ps_operation_reserved_callback_fn_v1 reserved[2]`；Configuration 在
16/24/32 存放三个 callback，在 40 存放 `reserved[3]`；Inference 在 16 存放
一个 callback，在 24 存放 `reserved[5]`；Region 与 Execution 都在 16/24 存放
两个 callback，在 32 存放 `reserved[4]`；Dependency 在 16 存放一个 callback，
在 24 存放 `reserved[5]`。每个 reserved callback 都为 null。任何 callback 或
reserved slot 都不得使用 `void *`、`uintptr_t`、`uint64_t` 或 unprototyped
function pointer 替代其 declared function-pointer type。

除 round-trip context value 外，每个 pointer parameter 都为 nonnull。Array-reference
pointer 在空 array 时仍为 nonnull；此时其 internal data pointer 为 null，count 为零。
Host 把 count、descriptor、suite、root 与 configured-context output 初始化为其 typed C
semantic zero value；create 失败
保持 `*configured_context_out` 为 null。`emit` call 使用 nonnull `records`、正的
bounded `uint32_t count` 与精确的 channel-specific `uint32_t stride`；不使用任何
call 表示空结果。

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
| record header / suite header | 16/4 与 16/4 |
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

29 个 fixed-layout payload type 精确由九个具名 plain helper 与 20 个具名 semantic
record 组成；record/suite header、root 与 suite table 是单独的 prefix/table type。
Header 必须在 C11/C++17 中 assert 它们全部，以及每个具名 function-pointer
type 的 size/alignment。它必须使用本 ADR 与 OpenSpec design 冻结的精确
`ps_operation_*_v1` type、field 与 typedef spelling；不能在不修订本决策时改变
numeric、parameter/field order、C type、size、alignment、ownership 或 meaning。OpenSpec
design 冻结每个 helper 与 semantic-record 的 field type/byte offset，以及 96-byte root
与每个 64-byte suite slot。
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

- Issue #102 实现源码私有的无指针 protocol-v1 request/response、基于 `SCM_RIGHTS` 的
  POSIX-shared-memory capability、canonical descriptor/content binding、严格
  offset/range/ownership validation，以及 one-shot process-local callback seam；
- Issue #103 实现 authenticated private-session `PluginRuntimeSupervisor` lifecycle、
  heartbeat/deadline、crash/hang/bad-output containment、基于事实的 signal 报告、
  fresh-process restart 与精确 reap；`SIGKILL` 只表示 memory-pressure-compatible，
  不能证明 OOM；
- Issue #104 负责 package allowlist/signature、sandbox/capability 与 enforceable
  resource policy。

本 ADR 被接受时并未预选其 frame、handle、authentication 或 policy format。Issue #102
随后为自身选择了独立版本化的 protocol-v1 frame 与 capability layout。该 protocol 不会
序列化 operation ABI v2、目标 operation ABI v1 或任何 ABI pointer record，也不引入
migration wrapper、shim、adapter 或 dual loader。Issue #103 随后选择了一种私有定长
lifecycle frame：它在专用 Unix datagram channel 上携带 OS 随机 nonce、完整 invocation
identity、严格 sequence 与 Host 选择的 heartbeat interval。该 session binding 不是 plugin
attestation 或 trust。Issue #104 仍拥有 package admission、sandbox/capability 与可执行
resource-policy format。当前没有 operation loader 会把 ABI v2 或仍为目标态的 ABI v1 映射到
supervised executor；最终用户选择仍属于完整 breaking ABI migration。Cross-process GPU/
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
