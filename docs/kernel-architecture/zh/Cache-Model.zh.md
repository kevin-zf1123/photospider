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

`GraphCacheService` 处理 `GraphModel::cache_root` 下的磁盘缓存文件。节点缓存条目描述缓存类型和位置。图像缓存文件保存为图像文件，命名的 `NodeOutput::data` 条目保存为图像文件旁边的 YAML 元数据。

对于 CLI 加载的 graph，`GraphModel::cache_root` 会在 graph load 前由 `cache_root_dir`
配置决定，并解析为 `<cache_root_dir>/<graph_name>`。相对 `cache_root_dir` 按进程当前工作目录解析。
未提供 cache root 的直接 `Kernel::load_graph` 调用继续使用 `<root_dir>/<graph_name>/cache`。

磁盘缓存精度当前支持 `int8` 和 `int16` 保存路径。加载的图像缓存数据会转换为浮点图像缓冲区。

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

## 缓存规则

- 新 HP 代码写入 `cached_output_high_precision`。
- 新 RT 代码将 `RealtimeProxyGraph` 写为临时交互式状态；dirty worker 写入必须先经过
  `RealtimeProxyWriteBuffer`，再提交到 proxy。
- 正式缓存的保存、加载、同步行为、后续 HP 计算和长期存储必须使用 HP 输出，不能将 RT 输出提升为权威缓存。
- 测试应分别验证 HP graph cache 和 RT proxy graph state。

`GraphInspectService` 只从 HP cache 选择 node-local 显示 metadata。当前 Host inspection surface
不会把 RT proxy state 提升到 `GraphModel`，也不会将其作为权威 cache metadata 暴露。
