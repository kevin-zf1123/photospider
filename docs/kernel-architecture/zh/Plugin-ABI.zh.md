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

## Validation

Loading 验证 exact ABI version/structure size、pointer/array alignment、pointer/count pair、
bounded key/count/rank/parameter value、text byte、duplicate parameter declaration、closed
enum/flag/type combination、required callback、output element/shape/byte count、facet
array/key/version/payload、arithmetic overflow 与 exactly-once destroy ownership。该 ABI
version 拒绝 trailing structure bytes，也不发布 v1 compatibility entry point。

Malformed registration 不发布任何内容。Multi-record registry publication 使用
copy-then-swap，allocation failure 不能暴露 prefix。Operation exception 被隔离；output 在
callback 返回前复制。Plugin-owned descriptor table 在 library unload 前 destroy。

## Lifecycle 与边界

Path 只来自 embedding-process startup configuration。Registry 在 compiler/executor 使用前
完成 assembly 并 freeze。DSO 与 host 在同一 trust domain 内执行。ABI check 是 correctness
validation，不是 sandbox、signature、certificate、package admission 或 process isolation。

不存在 policy ABI/SDK/DSO、external scheduling plugin 或 IPC plugin path。Data-definition
ABI 不构造 Value，也不提供 storage。
