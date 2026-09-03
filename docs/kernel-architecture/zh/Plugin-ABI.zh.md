# Operation 与 Data-Definition ABI

Photospider 安装两份 narrow same-trust extension header：

- operation ABI v2：copied semantic trait、closed typed parameter schema、
  plan-derived input demand 与一个 synchronous Value callback；
- data-provider ABI v1：copied schema key、element type 与 maximum rank。

已安装的 C++ convenience wrapper `operation_plugin.hpp` 是可直接包含且 self-contained
的 header：它在暴露 `element_type_value` 前自行包含 `<cstdint>` dependency 与 operation
C ABI，不依赖 consumer 先包含另一份 Photospider header。导出的
`Photospider::operation_sdk` interface target 会传播 `cxx_std_17`，因此 C++ consumer
可以从 package 获得 wrapper 的实际语言要求，无需自行重复声明。Maintained consumer
通过与 compiler 相符的 dialect assertion 证明这项传播：MSVC-compatible frontend 定义
`_MSVC_LANG` 时使用该宏，否则使用 `__cplusplus`。它不私自添加 standard flag，也不
要求 `/Zc:__cplusplus`。

Provider contract 继续保持纯 C。`Photospider::data_provider_sdk` 只传播 include
directory，不传播 C++ compile feature，所以 C11 translation unit 可以消费它而不会
获得 C++ 语言要求。Package 不发布独立的 `data_definition_sdk` alias。

## Operation record

Operation descriptor 包含 length-framed key、input count、flag、estimated bytes、output
element type、closed scalar/preserve/match/fixed shape 与 Whole/Elementwise/Halo Region
rule、halo radius、cacheability、bounded parameter-schema pointer/count、callback 与
opaque plugin state。Parameter record 发布 unique key、精确 Int64/Float64/Bool/String
type 与 required presence。Compiler 在 semantic IR 前拒绝 unknown、missing、wrong-type
与 conflicting parameter；callback 只接收 validated canonical value，不存在隐藏 default
fallback。

Callback 接收 bounded dense whole-Region input view、每个 input 的 planned demand
offset/extent、bounded facet array、backend enum、cooperative cancellation callback 与
host-owned output sink。它最多发布一个带 bounded facet 的 output；host 复制并验证为
dense whole-Region Value。第一次 sink 调用即占用 publication，即使 validation 拒绝也不
例外。任何第二次调用都会设置 invocation-local sticky violation，且不分配、不抛异常、
不替换第一次 accepted/rejected `Result`；callback 返回后，adapter 会报告稳定的 terminal
`OperationFailed` diagnostic：
`operation plugin violated output sink at-most-once contract`。Null sink context 返回零，
且不改变 invocation state。DSO input view 精确覆盖其 logical contiguous bytes；trailing
backing bytes 会被拒绝，不能成为不可见的 plugin state。

Synchronous callback 保持 `int` signature，但返回一个闭合的 version-two result：success、
ordinary failure、cancellation 或 backend unavailable。backend unavailable 与 ordinary
failure 不同，并且只有 copied trait 允许时才能从 GPU attempt 请求 CPU fallback。unknown
nonzero integer 是 ordinary `OperationFailed` result。报告 backend unavailable 的 callback
不得调用 output sink。若已调用，accepted output 是 terminal `OperationFailed` contract
violation；rejected output 保留 sink 的精确 typed failure。两种路径都不暴露
`BackendUnavailable` 或触发 CPU fallback；host cancellation 继续是最高优先级 result。
在该 cancellation check 之后，duplicate sink violation 优先于 success、backend
unavailable、ordinary failure、callback-reported cancellation 与 unknown result；因此它
绝不会发布第一次 Value，也绝不会请求 CPU fallback。

在任何 C++ 或 DSO callback entry 之前，`OperationRegistry::invoke` 会验证 operation/
input/demand count，先检查每个 input 的 `Value::valid()` 再读取 descriptor，验证每个
demand 与 parameter，观察 host cancellation，拒绝 CPU/GPU 之外的 backend value，随后
检查 backend capability。已知但不支持的 backend 仍为 `BackendUnavailable`；未知数字
backend 返回 `InvalidArgument`，且 DSO adapter 绝不会把它转换为 GPU。完成这些更高
优先级检查后，registry 只计算一次预期的 Scalar/Fixed/Preserve/Match output descriptor。
Preserve 会拒绝与声明 output type 冲突的首个 input element type；Match 会拒绝任一合法
input 的 type 或 shape 不一致。两者都在 callback 前返回 `TypeMismatch`，因此即使 callback
带副作用或原本会失败，也不会进入 callback。Callback output validation 复用该预计算
descriptor；成功 callback 返回 default-invalid `Value` 时仍安全地得到 `TypeMismatch`。
Plan-derived demand coverage 继续由 `ExecutionRun` 负责，registry 不在此重新推导。

C++ `OperationTraits::Fixed` record 只描述 logical output descriptor。Registration 会
验证非零 rank-1..8 shape、闭合 element type/rule 与普通 trait combination，但不会计算
dense element/byte product。Callback 可返回任何通过普通 publication validation 的 Value
layout，包括在巨大 logical shape 上只占八字节的 zero-stride broadcast。
`estimated_bytes` 是独立的 modeled admission estimate。C DSO Fixed descriptor 更严格，
因为 ABI v2 不携带 output stride：loading 会独立要求 contiguous signed stride 与 uint64
byte count 可表示。对于 dense total bytes `B`，loader 还要求 `B > 0`、zero-based last
byte `B - 1 <= INT64_MAX`，以及 `B <= SIZE_MAX`。因此在 64-bit host 上，UInt8
`{INT64_MAX + 1}` descriptor 与 `{2, 2^62}` 可表示；任一边界再增加一个 element 都会被
拒绝。这个区别不增加 ABI field，也不改变 Preserve 或 Match inference。

## Validation

在任何 Windows、Linux 或 Darwin native-loader 调用前，operation/provider loading 会把
精确 `std::string` path 验证为非空、最多 4096 bytes 且不含 embedded NUL。Malformed path
返回 `InvalidArgument`；平台无法加载的合法精确 path 仍返回 `NotFound`。被拒 path 不会
打开 truncated prefix，不发布 registry key/schema，也不会启动 native-owner lifecycle。

Loading 验证 exact ABI version/structure size、pointer/array alignment、pointer/count pair、
bounded key/count/rank/parameter value、严格 UTF-8 operation/parameter-schema/
provider-schema key、duplicate parameter declaration、closed enum/flag/type combination、
required callback、logical C++ fixed descriptor、dense C DSO fixed stride/byte
representability（包括 signed last-byte 与 host allocation-size bound）、output
element/shape/byte count、facet array/key/version/payload、arithmetic overflow 与
exactly-once destroy ownership。Key validation 会在 publication
前拒绝 invalid continuation byte、truncation、overlong encoding、UTF-16 surrogate、
大于 U+10FFFF 的值、embedded null 与 ASCII control，但不执行 Unicode normalization。
普通 facet payload 与 Value byte 仍是 opaque binary data。该 ABI version 拒绝 trailing
structure bytes，也不发布 v1 compatibility entry point。

Malformed registration 不发布任何内容。Builtin/embedding definition 与每个 DSO
definition 都会在 publication 前完整构造，随后由 private immutable owning handle 保留。
Registry map、DSO transaction staging 与 invocation snapshot 只复制该 handle：registry
mutex 持有期间绝不复制或执行 embedding callable。Multi-record publication 对 handle map
执行 copy-then-swap，因此 allocation failure 不能暴露 prefix；被替换的 map 在 unlock 后
retire。Invocation 持有的 handle 会让 callback 及其捕获的 DSO lease 存活到 callback
完成。Embedding C++ operation callback 的 `std::bad_alloc` 会继续传播，使 caller 能够
保留 resource-exhaustion policy；其他每个 `std::exception` 都映射为
`OperationFailed`。若 `what()` 返回 null，则在不从 null 构造 string 的前提下规范化为
空 diagnostic。Nonstandard exception 获得稳定的通用 `OperationFailed` diagnostic。
Output 在 callback 返回前复制。Plugin-owned descriptor table 在 library unload 前 destroy。

Dense layout product 在 multiplication 前使用 checked uint64 division，随后验证 complete
byte range。Boundary fixture 会加载真实 DSO，并要求后续 descriptor 不可表示时进行
transactional rejection，同时 destroy 与 native close 各恰好一次。Compile-time-width
helper 即使在 64-bit test builder 上也会实例化 32-bit allocation-size path。

Native open 后，每个 operation/provider handle 都立即由 move-only stack owner 持有。
当 exact API structure prefix 可安全读取后，该 owner 也接管已经取得的 destroy callback。
因此 symbol、table、schema、heap-owner 或后续 staging failure 会对每个已取得的 destroy
callback 与 native close 各调用恰好一次。成功 loading 会把同一个 owner 显式 move 到
published heap lease。

Provider registry 在 copied schema record 之前声明 native lease，因此逆序析构会在
最终 provider destroy callback 与 native unload 前退役所有 registry-owned schema。
`find()` 结果自行拥有复制后的 key，也不包含 DSO pointer，因此可在 registry teardown
后继续存在而不借用 mapped provider memory。

## Lifecycle 与边界

Path 只来自 embedding-process startup configuration，并作为精确且 NUL-free 的 byte
sequence 消费。Registry 在 compiler/executor 使用前完成 assembly 并 freeze。DSO 与 host
在同一 trust domain 内执行。ABI check 是 correctness validation，不是 sandbox、signature、
certificate、package admission 或 process isolation。

不存在 policy ABI/SDK/DSO、external scheduling plugin 或 IPC plugin path。Data-definition
ABI 不构造 Value，也不提供 storage。
