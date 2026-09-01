# Operation 与 Data-Definition ABI

Photospider 安装两份 narrow same-trust extension header：

- operation ABI v2：copied semantic trait、closed typed parameter schema、
  plan-derived input demand 与一个 synchronous Value callback；
- data-provider ABI v1：copied schema key、element type 与 maximum rank。

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
dense whole-Region Value。DSO input view 精确覆盖其 logical contiguous bytes；trailing
backing bytes 会被拒绝，不能成为不可见的 plugin state。

Synchronous callback 保持 `int` signature，但返回一个闭合的 version-two result：success、
ordinary failure、cancellation 或 backend unavailable。backend unavailable 与 ordinary
failure 不同，并且只有 copied trait 允许时才能从 GPU attempt 请求 CPU fallback。unknown
nonzero integer 是 ordinary `OperationFailed` result。报告 backend unavailable 的 callback
不发布 output，因此 fallback 不会复用 partial GPU Value。

## Validation

Loading 验证 exact ABI version/structure size、pointer/array alignment、pointer/count pair、
bounded key/count/rank/parameter value、严格 UTF-8 operation/parameter-schema/
provider-schema key、duplicate parameter declaration、closed enum/flag/type combination、
required callback、output element/shape/byte count、facet array/key/version/payload、
arithmetic overflow 与 exactly-once destroy ownership。Key validation 会在 publication
前拒绝 invalid continuation byte、truncation、overlong encoding、UTF-16 surrogate、
大于 U+10FFFF 的值、embedded null 与 ASCII control，但不执行 Unicode normalization。
普通 facet payload 与 Value byte 仍是 opaque binary data。该 ABI version 拒绝 trailing
structure bytes，也不发布 v1 compatibility entry point。

Malformed registration 不发布任何内容。Multi-record registry publication 使用
copy-then-swap，allocation failure 不能暴露 prefix。Operation exception 被隔离；output 在
callback 返回前复制。Plugin-owned descriptor table 在 library unload 前 destroy。

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

Path 只来自 embedding-process startup configuration。Registry 在 compiler/executor 使用前
完成 assembly 并 freeze。DSO 与 host 在同一 trust domain 内执行。ABI check 是 correctness
validation，不是 sandbox、signature、certificate、package admission 或 process isolation。

不存在 policy ABI/SDK/DSO、external scheduling plugin 或 IPC plugin path。Data-definition
ABI 不构造 Value，也不提供 storage。
