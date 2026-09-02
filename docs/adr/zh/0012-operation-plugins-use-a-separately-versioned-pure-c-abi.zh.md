# ADR 0012：Operation 与 Data Definition 使用精确的进程内 C ABI

- 状态：已接受，由 ADR 0015 收窄
- 日期：2026-09-01 边界修订

## 背景

Kernel 需要 operation extension point 和与其直接相关的 data-definition extension point，
但不能暴露 compiler/runtime implementation object。必须保留强 correctness validation，
同时不能声称 C ABI 会让 native code 安全或隔离。

## 决策

Operation ABI 是精确的 version-two C contract；data-provider ABI 仍是独立的
version-one contract。其 C++ helper 不增加第二套 binary contract。不会保留 operation
ABI v1 adapter 或 decoder。

### Operation ABI

Operation descriptor 包含一个 length-framed key、exact input count、flag、estimated bytes、
output element type、一个 closed shape rule（包括显式有界 fixed shape）、一个 closed
Region rule、optional halo radius、cacheability、bounded parameter-schema pointer/count、
一个 synchronous execute callback 与 descriptor-owned opaque state。每个 parameter schema
record 发布 unique key、精确 closed type 与 required/optional bit。

Host 将这些 value 复制到 `OperationTraits` 和 semantic IR。Callback、DSO 或 opaque-state
identity 不会复制进 IR/digest。Callback 接收 bounded dense whole-Region input view、
plan-derived input-demand offset/extent、canonical 且已通过 schema validation 的 parameter
value array、bounded facet record、selected local backend、cooperative cancellation
observation 与 host-owned single-publication output sink。Host 在 callback 返回前复制 output
facet/bytes，并重建 validated Value。

C++ registry 把 Fixed rule 视为 logical descriptor contract：rank 为 1..8，每个 extent
非零，element type 与 rule 属于闭合 vocabulary。Trait publication 不会相乘 logical
extent。因此，即使对应 dense element/byte product 溢出，embedding callback 仍可发布合法
zero-stride broadcast Value。`estimated_bytes` 继续是 callback 独立的 modeled resource-
admission value，并复制进 physical plan。C DSO descriptor 则有意保持更窄，因为 ABI v2
不发布 stride：Fixed DSO output 必须具有可表示的 contiguous signed stride 与完整 uint64
byte count，否则 loader 会 transactional rejection。Preserve 与 Match semantics 不变。

保持不变的 `int` callback result 具有闭合的 version-two vocabulary：success、ordinary
failure、cooperative cancellation 与 backend unavailable。显式 backend-unavailable result
会映射为 `BackendUnavailable`；只有 copied trait 允许 CPU fallback 的 GPU attempt 才能在
CPU 上重试。ordinary failure 与所有 unknown nonzero integer 仍映射为 `OperationFailed`，
绝不触发 fallback。backend-unavailable callback 不得调用 output sink。若已经调用，
accepted output 会成为 terminal `OperationFailed` contract violation；rejected output
保留 sink 的精确 typed failure。两种路径都不暴露 `BackendUnavailable`，也不在 CPU 上
重试；host cancellation 继续拥有最高优先级。callback signature 与 descriptor layout
均保持不变。

### Data-definition ABI

Data provider 只发布 bounded schema record：key、element type 与 maximum rank。Registry 会
复制并 freeze 这些 record。该 ABI 直接服务 operation/Value vocabulary；它不读文件、不
创建 runtime Value，也不拥有 persistence。

### 精确 validation 与 lifetime

Load/registration 在 publication 前验证：

- exact ABI version 与 exact structure size；
- 自然 pointer/array alignment；
- pointer/count pair、maximum record/key/rank/parameter bound 与 checked arithmetic；
- 不含 embedded NUL/control byte 的 length-framed key；
- closed enum/flag/rule/parameter-type vocabulary、unique parameter key、required item
  presence、exact source type 与合法 trait combination；
- descriptor-only C++ fixed-shape validation，以及对不携带 stride 的 C DSO fixed
  descriptor 独立执行 dense stride/byte representability validation；
- required callback、single output publication、exact output element/shape、bounded facet
  array/key/version/payload 与 byte count；
- callback exception fence 与 exactly-once destroy ownership。

该版本有意拒绝 trailing structure bytes，不把它们视为 forward compatibility。Malformed
table 不会发布 partial registry entry；multi-record publication 使用 copy-then-swap，
allocation failure 也不发布 prefix。Library 只能来自显式 process-startup
configuration；registry 在 compiler/executor 使用前变成 read-only，并在 unload 前
destroy plugin-owned table。

Operation/provider DSO 与 host 在同一 trust domain 内执行。ABI validation 防止 malformed
interoperability；它不是 sandbox、signature、certificate、trust chain、package admission、
heartbeat 或 process supervisor。

## 边界

不存在 policy ABI/SDK/DSO、external scheduler、IPC plugin path、isolated plugin process
或 native-code security product。Daemon 永不选择 plugin path。

## 结果

- Installed C11/C++17 consumer 可编写同信任 operation/data-definition DSO。
- Compiler trait 保持 copied portable value。
- Exact validation、exception fence 与 destroy-before-unload 保证 correctness/cleanup。
- ABI version change 是 0.x package 中的显式 breaking change。
