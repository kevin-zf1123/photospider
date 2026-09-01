# ADR 0002：外库不进入 Kernel 语义

## 状态

已接受，并由 ADR 0015 针对当前可嵌入 kernel 边界收窄。

## 背景

Reset 之前的产品在 core、adapter、CLI、persistence、service 或产品安全路径中
嵌入 OpenCV、yaml-cpp、FTXUI、CURL 和 OpenSSL。这种耦合使 optional library 成为
kernel build 的一部分，并让它们的类型和 lifecycle 假设影响 kernel 语义。

Scope Reset 保留一个可嵌入 graph compiler/executor，必需 platform dependency 只有
C++ runtime 与 thread library。第三方算法仍可供 operation implementation 使用，但不属于
kernel package contract。

## 决策

Kernel 使用 public contract 和 standard-library representation 拥有
`WorkflowDocument`、typed IR、operation trait、`Value`、`Region`、layout、execution 与
diagnostic 类型。

本仓库不提供 OpenCV/yaml-cpp adapter、CLI library、codec 或 dependency-toggle
compatibility profile。Canonical kernel target 与 installed package 不会 find、link、export
或 advertise 这些库。

受信任的进程内 operation 或 data-provider DSO 可以在私有实现中链接第三方库。
它必须在 operation/provider ABI 边界转换所有 input、output、exception 和 lifecycle
behavior。任何第三方 type、allocator owner、exception、path 或 configuration object 都不得
跨越 public ABI。ABI validation 是 correctness validation，不是 sandboxing 或 native-code
security。

`WorkflowDocument` 是 in-memory compiler input。File format 与 storage service 属于 consumer
关切，不是 kernel adapter 或 authority。

## 结果

- Clean kernel configure/build/install 和 isolated consumer 不需要 optional third-party
  package。
- Kernel 原语刻意保持最小；Photospider 不重建通用图像处理、serialization、UI、
  network 或 crypto library。
- Operation DSO 在 process-global startup-configured operation set 内部，拥有所需的
  library initialization、thread setting 和 exception translation。
- 新增本仓库拥有的 library integration 需要聚焦的 operation/provider 决策；它不得重新
  引入 core dependency、compatibility option 或 filesystem authority。
