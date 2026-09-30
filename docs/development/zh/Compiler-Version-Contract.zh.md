# Compiler 与 Package 版本契约

## 当前安装契约

当前 kernel package 为 **0.30.0**。`DiskCacheConfig` 和 `DiskCacheStatistics` 已移除。`ExecutionContextConfig` 不再包含磁盘缓存配置；`ExecutionContext::clear_disk_cache()`、`flush_disk_cache()` 和 `disk_cache_statistics()` 已移除；`OperationRegistry::persistent_cache_identity()` 也已移除。C++ consumers 必须针对 0.30 headers 重建。Package 使用 `SameMinorVersion` 兼容规则，因此 0.29 与 0.30 package 请求不兼容。

| 契约 | 当前版本 |
| --- | --- |
| Package | 0.30.0 |
| WorkflowDocument schema | 4 |
| OperationTraits | 21 |
| Numeric operation C table | ABI 11 |
| Structured Result operation C table | ABI 1 |
| Data provider C table | ABI 1 |
| Semantic graph identity | `semantic-graph-ir-v19` |
| Physical plan identity | `physical-plan-v19` |
| Outer plan cache key | `plan-cache-key-v15` |
| Optimizer identity | `optimizer-v5-canonical-noop` |
| Tensor-description codecs | TDM4/TDM5，按当前 codec 规则选择 |
| Result schema canonical encoding | Version 2 |

这些版本属于独立兼容轴。即使 C table 版本未变，C++ layout 变化仍要求 C++ consumers 重建。Numeric C table 保持 ABI 11；Result modules 使用独立版本化的 ABI 1 table。已移除的 planar C table 没有兼容 adapter，导出 v1-v3 入口的 modules 会被拒绝。 这些删除改变了 public C++ API 和 `ExecutionContextConfig` 布局。WorkflowDocument schema、OperationTraits 版本、numeric operation C ABI、Result operation C ABI、data-provider C ABI、semantic/physical identity 版本和 Result schema encoding 均未变化。

## Result 与图像接口

Structured Result schema 声明 typed image slots 和/或 primitive fields。`PlanarImage` 是 Result image slot 内的存储 backing。普通非图像 numeric Values 保留 Value storage 契约。Result 图像 samples 使用有界 frame/layer axes、descriptor facts、planar storage order 和 row pitch。图像 bytes 不会表示为 packed primitive Result fields。

Result ABI 1 支持具名 outputs、selected input projections、通过同步 sink 执行的纯 metadata resolution、staged Result callbacks、typed image needs、dependency relations 和 native services。`requested_kind` 区分完整 Result object（`0`）、Value footprint（`1`）和 image footprint（`2`）。Region 数量为零的 Value 或 image footprint 表示 Empty。Resolver 为每个 output 调用一次 `set_output`；host 在同步调用返回前复制并校验嵌套 schema records。

## Identity 与 runtime 匹配

Semantic identity 包含 operation contracts、typed Result schema、semantic metadata、parameters、ordered inputs，以及适用时的 selected output 和 captured query。Physical plan identity 包含影响执行的物理选择。Page offsets 和临时 addresses 不属于 semantic identity。内部 IR 和 plans 不是 serialization formats。

已安装的 C++ headers、链接的 kernel library 和所选 C table 必须描述相同的 package 与 ABI。Loader 在导入 module 前验证精确的 C table 版本和大小。Native GPU 行为还要求选定 backend 和可用设备。Kernel package 不声称每个已注册 production image operation 都实现 Result 图像契约，详见[图像 operations](../../kernel-architecture/zh/Image-Operations.zh.md)。
