# ADR 0012：Operation 与 Data Definition 使用精确的进程内 C ABI

- 状态：已接受，由 ADR 0015 收窄
- 日期：2026-09-01 边界修订

## 背景

Kernel 需要 operation extension point 和与其直接相关的 data-definition extension point，
但不能暴露 compiler/runtime implementation object。必须保留强 correctness validation，
同时不能声称 C ABI 会让 native code 安全或隔离。

## 决策

Operation ABI 与 data-provider ABI 是相互独立的 version-one C contract。其 C++ helper 不
增加第二套 binary contract。

### Operation ABI

Operation descriptor 包含一个 length-framed key、exact input count、flag、estimated bytes、
output element type、一个 closed shape rule、一个 closed Region rule、optional halo radius、
cacheability、一个 synchronous execute callback 与 descriptor-owned opaque state。

Host 将这些 value 复制到 `OperationTraits` 和 semantic IR。Callback、DSO 或 opaque-state
identity 不会复制进 IR/digest。Callback 接收 bounded dense whole-Region input
view、bounded facet record、selected local backend、cooperative cancellation observation 与
host-owned single-publication output sink。Host 在 callback 返回前复制 output
facet/bytes，并重建 validated Value。

### Data-definition ABI

Data provider 只发布 bounded schema record：key、element type 与 maximum rank。Registry 会
复制并 freeze 这些 record。该 ABI 直接服务 operation/Value vocabulary；它不读文件、不
创建 runtime Value，也不拥有 persistence。

### 精确 validation 与 lifetime

Load/registration 在 publication 前验证：

- exact ABI version 与 exact structure size；
- 自然 pointer/array alignment；
- pointer/count pair、maximum record/key/rank bound 与 checked arithmetic；
- 不含 embedded NUL/control byte 的 length-framed key；
- closed enum/flag/rule vocabulary 与合法 trait combination；
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
