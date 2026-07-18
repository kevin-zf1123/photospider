# 内核缓存模型

内核在每个 `Node` 上拥有正式 HP 内存缓存，在 `RealtimeProxyGraph` 中拥有临时 RT proxy
状态，并在图缓存根目录下拥有磁盘缓存文件。本文档定义当前缓存语义。

## 正式缓存与临时状态

| 位置 | 状态 | 含义 |
| --- | --- | --- |
| `Node::cached_output_high_precision` | 正式缓存 | 完整质量、可复用的 HP 输出。 |
| `RealtimeProxyGraph` node state | 临时 RT proxy | 低分辨率交互式预览/更新输出。 |

只有高精度输出是正式可复用缓存。这意味着只有 HP 输出可以作为后续 HP 计算、磁盘缓存、长期存储以及其他可复用缓存行为的权威来源。RT proxy 输出是临时交互式状态，不能被视为权威缓存，不能作为磁盘缓存同步来源，也不能作为长期存储输入。

## HP 缓存

HP 计算写入 `cached_output_high_precision`。HP 缓存是节点的权威完整质量结果。

相关字段：

| 字段 | 含义 |
| --- | --- |
| `hp_version` | HP 输出变化的版本计数器。 |
| `hp_roi` | 最近更新或合并后的 HP 区域。 |

## RT 状态

RT 计算写入 `RealtimeProxyGraph`。每个 proxy node 以原 graph node id 为 key，只保存低分辨率输出、HP-space ROI metadata、version 和 RT dirty-source generation。它不复制 Node 参数、输入、拓扑、cache 或正式 HP 状态。当观察到的 graph topology generation 改变时，同步会重置 live proxy entries，而不是按复用 node id 保留 state，因此 reload/edit workflow 不会暴露上一份 graph 的陈旧低分辨率输出。

Dirty RT execution 不会写 graph-owned RT 字段。Worker task 会先把代理输出、ROI metadata、版本计数和 dirty-source commit generation stage 到 `RealtimeProxyWriteBuffer`，然后在 RT dirty work set drain 后把 staged state 提交到 `RealtimeProxyGraph`。Dirty HP execution 同样会先把 HP 输出 stage 到 `HighPrecisionDirtyWriteBuffer`，再提交到 `GraphModel`，因此 HP/RT sibling 可以并发计算，同时保持 RT-first commit 顺序。

相关字段：

| Proxy 字段 | 含义 |
| --- | --- |
| `version` | RT proxy 输出变化的版本计数器。 |
| `roi_hp` | RT 更新所代表的最近或合并后的 HP 空间区域。 |
| `dirty_source_generation` | 用于 stale source 检查的 RT dirty source generation。 |

## 磁盘缓存

`GraphCacheService` 处理 `GraphModel::cache_root` 下的磁盘缓存文件。节点缓存条目描述缓存类型和位置。
图像缓存文件保存为图像文件，命名的 `NodeOutput::data` 条目保存为图像文件旁边的 YAML 元数据。
内存中的命名 data 始终是脱离 adapter 的 `plugin::ParameterMap`；
`GraphCacheService` 绝不构造 YAML value。

对于 CLI 加载的 graph，`GraphModel::cache_root` 会在 graph load 前由 `cache_root_dir`
配置决定，并解析为 `<cache_root_dir>/<graph_name>`。相对 `cache_root_dir` 按进程当前工作目录解析。
未提供 cache root 的直接 `Kernel::load_graph` 调用继续使用 `<root_dir>/<graph_name>/cache`。

磁盘缓存精度当前支持 `int8` 和 `int16` 保存路径。加载的图像缓存数据会转换为浮点图像缓冲区。

图像字节通过私有、依赖中立的 `ImageArtifactCodec` 契约。`Kernel` 从产品组合根取得一个配置好的
共享 codec，并将其注入 `GraphCacheService`；Graph/cache 代码只提供 path、`ImageBuffer` 与
规范化整数精度。当前生产 adapter 使用 OpenCV imgcodecs，并把 provider failure 翻译为
`GraphErrc::Io`，同时让 OpenCV `StsNoMem` 保持为 `std::bad_alloc`。测试会注入确定性 fake，
在不读取或写入真实图像格式的情况下验证调用顺序、生命周期保持、精度选择、可恢复错误与资源耗尽。

Named value 独立通过私有、依赖中立的 `CacheMetadataCodec` contract，只交换 path 和脱离
adapter 的 `ParameterMap` value。`Kernel` 注入、`GraphCacheService` 保留第二个不可变 shared
owner，且其 service lifetime 与 image codec 相同。Cache policy 仍负责推导同级 `.yml` path、
创建目录、选择 entry、记录计时和诊断、保持 HP 权威性，以及移除陈旧文件。只有已配置的
`YamlCacheMetadataCodec` 拥有 YAML node、递归 value conversion、stream IO 与 provider
exception translation。Null document 解码为空 map；无效 representation 变成
`GraphErrc::InvalidYaml`，可恢复 write/emission failure 变成 `GraphErrc::Io`，
`std::bad_alloc` 原样传播。确定性 fake 会验证精确 path、value、保留生命周期、error category
与资源耗尽，而 cache code 不声明 YAML type。

磁盘缓存加载尝试会保留既有 try-load 布尔返回契约，同时通过 GraphModel 专用的
disk-cache diagnostic mutex 记录最新诊断。调用方通过 snapshot API 检查该状态，
而不是直接读取可变 optional 存储。该诊断结果会区分跳过的尝试、真实 miss、命中以及读取/解析错误。
损坏的图像文件、无效的 YAML 元数据和文件系统失败会被记录为带错误码和消息的错误，而不是与普通缓存
miss 混在一起。

## 缓存命令

| 操作 | 效果 |
| --- | --- |
| Clear drive cache | 删除磁盘缓存目录内容并重建根目录。 |
| Clear memory cache | 清理 `GraphModel` 跟踪的内存 HP 缓存。 |
| Clear cache | 同时清理磁盘和内存缓存。 |
| Cache all nodes | 在配置允许时将具有 HP 输出的节点保存到磁盘。 |
| Free transient memory | 清理非终点节点的内存缓存状态。 |
| Synchronize disk cache | 保存 HP 输出，并为没有 HP 输出的节点移除陈旧磁盘文件。 |

磁盘缓存保存、加载和同步只使用 `cached_output_high_precision`。RT proxy 输出不会保护陈旧磁盘文件，也不会被提升为磁盘缓存状态。

## 边界与原理

- HP 路径写入 `cached_output_high_precision`。
- RT 路径将 `RealtimeProxyGraph` 写为临时交互式状态；dirty worker 写入必须先经过
  `RealtimeProxyWriteBuffer`，再提交到 proxy。
- 正式缓存的保存、加载、同步行为、后续 HP 计算和长期存储必须使用 HP 输出，不能将 RT 输出提升为权威缓存。
- 长期测试分别验证 HP graph cache 和 RT proxy graph state。

`GraphInspectService` 只从 HP cache 选择 node-local 显示 metadata。当前 Host inspection surface
不会把 RT proxy state 提升到 `GraphModel`，也不会将其作为权威 cache metadata 暴露。

只有一个正式缓存权威，可以防止低分辨率 preview 静默变成 HP dependency 或 persistence source。
Request-local staging 会让尚未组装完成的 dirty output 保持不可见，直到相应 domain 的工作 settle。

当前私有 disk-cache 实现既不调用 OpenCV image codec，也不调用 YAML API。它依赖注入的
`ImageArtifactCodec` 与 `CacheMetadataCodec` contract；已配置的私有 adapter 拥有 provider
decode/encode、递归 conversion、stream IO 与 exception translation。Issue #62 完成了这条
runtime/cache value 边界。正常 configured product 仍会为其具体 adapter 发现并链接 yaml-cpp；
dependency-disabled product/build evidence 仍属于 Issue #63。
[ADR 0002](../../adr/zh/0002-external-libraries-are-kernel-adapters.zh.md)
和精确的[依赖中立内核目标](../../roadmap/zh/Kernel-Evolution.zh.md#依赖中立内核)描述最终 adapter 与
document boundary。

## 实现与验证入口

- `src/lib/core/image_artifact_codec.hpp`
- `src/lib/core/cache_metadata_codec.hpp`
- `src/lib/adapters/opencv/image_artifact_codec_opencv.*`
- `src/lib/adapters/yaml/yaml_cache_metadata_codec.*`
- `src/lib/adapters/yaml/parameter_value_yaml.*`
- `src/lib/providers/configured_image_artifact_codec.*`
- `src/lib/graph/graph_cache_service.*`
- `src/lib/graph/graph_model.*`
- `src/lib/compute/realtime_proxy_graph.*`
- `src/lib/compute/dirty_write_buffers.*`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_host_adapter.cpp`
