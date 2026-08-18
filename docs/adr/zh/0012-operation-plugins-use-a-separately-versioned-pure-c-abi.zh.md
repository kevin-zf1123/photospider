# ADR 0012：Operation Plugin 使用单独版本化的纯 C ABI

- 状态：已接受并实现
- 日期：2026-08-18
- 相关：ADR 0008、ADR 0011、ADR 0013、Issue #102、#103、#132

## 背景

Operation plugin 是独立构建的动态库。公共 C++ registration 边界会把 plugin binary 与 compiler
ABI、标准库布局、exception runtime、ownership type 和私有 registry object 耦合。DI-1 还要求
精确 DenseImage descriptor/facet/layout metadata；DI-2 要求不可变 output plan 与 Host-owned
allocation、grant、seal 和 publication authority。Untrusted CPU execution 必须跨越既有 isolated
runtime，同时不能序列化 process-local address。

先前的临时 registration generation 无法表达或执行这些要求。因此迁移必须是一次 breaking
package cut：一套新 operation ABI、一条 loader path、完整的仓库/provider 迁移，并通过删除而不是
compatibility adaptation 收口。

## 决策

### 已安装 operation contract 是纯 C ABI v1

Photospider 安装 `operation_plugin_api.h` 这一 self-contained C11/C++17 header，以及
`operation_plugin.hpp` 这一 header-only C++17 authoring helper。Helper 内部可以使用 C++，但只
发出 C record、C-linkage symbol、C function pointer 与冻结 status。C++ object、callback
abstraction、exception、allocator、registry、runtime owner 或标准库 type 都不会跨越 DSO 边界。

每个 operation DSO 精确导出：

```c
uint32_t ps_operation_plugin_get_abi_version(void);
ps_operation_status_v1 ps_operation_plugin_get_api_v1(
    ps_operation_plugin_api_v1 *api);
```

数字 probe 无副作用。Root discovery 填充 Host 预备的精确 96-byte record。Host 查询精确的
64-byte Definition、Configuration、Inference、Region、Dependency 与 Execution suite。Version、
size、kind、flags、reserved value、stride、count、alignment、pointer/count relation、enum、
identity 与嵌套 relationship 都会被精确验证。不提供 minimum-size、missing-tail、dual-loader、
alias、wrapper 或 forwarding compatibility。

### ABI v1 包含完整 DI-1/DI-2 record

ABI 定义 30 个精确 semantic record kind。Value projection 保留 Schema、Facet、Layout 的
identity/version/digest；DenseTensor rank、extent、element semantics、storage encoding 与可选
quantization；物理 StridedLayout；signed image data/display window；axis；channel/group；
sample-domain/color facts；受限 buffer；input slot/edge identity；以及 logical Region。

Inference 发出不可变 output plan。每个 plan 命名 Host mint 的 identity、完整 result
descriptor/Region，以及精确 buffer size/alignment/access row。只有 Host 可以 allocation。
Execution 接收 callback-scoped mutable output binding，其中包含 Host mint 的 grant identity 与精确
授权 span。插件不会得到 allocator、seal callback、`ValueBuilder` 或可转移 owner。任何非 OK
status、sink failure、exception、cancellation、malformed echo、unauthorized span 或 late result
都会使整个 binding fail closed；Host 最多 seal/publish 一次。

Region、dependency 与 diagnostic row 通过受限 Host sink 发出。首个 sink failure 是 sticky。
所有 plugin output 都由 Host 深拷贝并独立重验，之后才可能影响 planning、allocation、execution、
sealing 或 publication。

### Registration 预备一个不可变 generation

Loader 授权精确 opened object、确认数字 ABI v1、填充 prepared root/suite、枚举并验证全部受限
definition、深拷贝 immutable metadata、创建 callback/context owner，并在一次不抛异常的原子
registry publication 前安装 sealed-object/native DSO 组合 lease。此前任何失败都不改变 visible
state。

每次 publication 保留 revision 与 predecessor identity。Replacement 会 shadow 旧 generation；
卸载被 shadow 的 middle generation 会把 predecessor splice 到更新 snapshot；unload-all 按成功
publication 逆序执行。任何 foreign callback、context destruction、generation destruction 或
DSO close 前都会释放 registry lock。

每个成功 configured-context creation 都会收到精确一次 matching destroy。只有 definition、
context、result、callback snapshot 与 in-flight call 全部释放后，完整 generation 才会收到一次
`destroy_plugin` 尝试。Destruction 在精确 DSO lease 下运行且绝不重试。

### Trusted 与 supervised mode 共用一个 Host validation 边界

Trusted CPU mode 在 generation lease 下进程内调用纯 C callback。Supervised CPU mode 只携带非零
signed runtime-package identity，并解析 matching private `PluginInvocationExecutor`。缺失或不匹配
route 会在 direct callback entry 前失败；不存在 trusted fallback。

独立版本化的 isolation protocol 当前为 version 2。它传输 configuration、
descriptor/facet/layout/Region、不可变 output plan、capability index、diagnostic、dependency 与
written range 的 bounded canonical copy。它绝不传输 pointer、callback/context value、mapped
address、PID、path、descriptor、native handle 或 DSO-generation owner。Child 在 callback entry
前验证；Host 解码为全新 object，并在 publication 前对照 immutable request、当前
invocation/resource identity、output plan 与 Host grant 重新验证 hostile result。

### Package 迁移是原子的

已安装 component/target 是 `operation_plugin_sdk` /
`Photospider::operation_plugin_sdk`；它没有外部 link dependency。`operation_runtime` 保持独立的
显式 Value/runtime component。仓库 operation、lifecycle fixture、OpenCV provider、可选平台
provider、CMake export 与 installed C11/C++17 consumer 都使用 ABI v1。所有 predecessor header、
symbol、registrar/callback contract、loader lookup、fixture、component assertion、adapter 与 active
documentation 都在同一迁移中删除。

## 影响

### 正面影响

- C11/C++17 consumer 共用一套精确且 compiler-neutral 的 binary contract。
- DI-1 metadata 与 DI-2 Host output authority 能跨越边界，不依赖 opaque lossy payload 或第二个
  allocator/publication owner。
- Staged publication、predecessor restoration、reverse unload、in-flight DSO lifetime 与
  exactly-once destruction 都保持显式且可测试。
- Trusted/supervised CPU path 收敛到同一个 Host validation/seal/commit 边界，isolation wire 不含
  process-local pointer。
- Package consumer 可以独立选择 dependency-neutral SDK、Value/runtime 与 OpenCV support。

### 负面影响与缓解

- 精确 record surface 很大。独立 C11/C++17 layout assertion 与 hostile
  root/suite/record/count/stride/reserved/tail fixture 会锁定它。
- Registration 与 inference 会深拷贝受限 metadata。Bound 让开销确定，并防止 DSO-borrowed
  pointer 进入 published state。
- 纯 C 不会 sandbox trusted native code。Trust admission 保持显式；untrusted work 使用
  fresh-process supervisor route。
- Darwin 在当前 trust policy 下无法正向执行 exact-object supervised runtime。它验证 portable
  layout 与 fail-before-process 行为；Linux 仍是 native DSO/isolation integration gate。
- 这是有意的 installed binary break。Consumer 必须针对 matching SDK 重建；不存在模糊的
  compatibility path。

## 范围边界

本决策只迁移 operation plugin 与 isolated-invocation boundary。DI-4 仍负责最终删除或迁移
operation/isolation 边界之外的 public Host、IPC/worker、durable、codec、CLI 与其余
`ImageBuffer` surface。Policy ABI v1 与 data-definition provider ABI v3 继续独立版本化且不变。

## 验证

长期门禁包括精确 installed C11/C++17 consumer、仓库/OpenCV operation 行为、rich
metadata/output-plan/grant/Region/dependency test、malformed record rejection、replacement 与
middle-generation unload、reverse unload、in-flight DSO lease、failure rollback、destroy-once、
isolation protocol-v2 round trip 与 hostile response、route-before-process failure，以及受支持平台
上的 supervised execution/recovery。
