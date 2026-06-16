# 内核缓存模型

内核在每个 `Node` 上拥有内存缓存字段，并在图缓存根目录下拥有磁盘缓存文件。本文档定义目标缓存语义。

## 正式缓存与临时状态

| 字段 | 状态 | 含义 |
| --- | --- | --- |
| `cached_output_high_precision` | 正式缓存 | 完整质量、可复用的 HP 输出。 |
| `cached_output_real_time` | 临时 RT 状态 | 交互式预览/更新输出。 |

只有高精度输出是正式可复用缓存。这意味着只有 HP 输出可以作为后续 HP 计算、磁盘缓存、长期存储以及其他可复用缓存行为的权威来源。`cached_output_real_time` 是临时交互式状态，不能被视为权威缓存，不能作为磁盘缓存同步来源，也不能作为长期存储输入。

## HP 缓存

HP 计算写入 `cached_output_high_precision`。HP 缓存是节点的权威完整质量结果。

相关字段：

| 字段 | 含义 |
| --- | --- |
| `hp_version` | HP 输出变化的版本计数器。 |
| `hp_roi` | 最近更新或合并后的 HP 区域。 |

## RT 状态

RT 计算写入 `cached_output_real_time`。RT 状态是交互式预览或代理结果，分辨率可能低于 HP 输出，并不是正式缓存权威。

相关字段：

| 字段 | 含义 |
| --- | --- |
| `rt_version` | RT 输出变化的版本计数器。 |
| `rt_roi` | RT 更新所代表的最近或合并后的 HP 空间区域。 |

## 磁盘缓存

`GraphCacheService` 处理 `GraphModel::cache_root` 下的磁盘缓存文件。节点缓存条目描述缓存类型和位置。图像缓存文件保存为图像文件，命名的 `NodeOutput::data` 条目保存为图像文件旁边的 YAML 元数据。

对于 CLI 加载的 graph，`GraphModel::cache_root` 会在 graph load 前由 `cache_root_dir`
配置决定，并解析为 `<cache_root_dir>/<graph_name>`。相对 `cache_root_dir` 按进程当前工作目录解析。
未提供 cache root 的直接 `Kernel::load_graph` 调用继续使用 `<root_dir>/<graph_name>/cache`。

磁盘缓存精度当前支持 `int8` 和 `int16` 保存路径。加载的图像缓存数据会转换为浮点图像缓冲区。

磁盘缓存加载尝试会保留既有 try-load 布尔返回契约，同时记录
`GraphModel::last_disk_cache_load_result`。该诊断结果会区分跳过的尝试、真实
miss、命中以及读取/解析错误。损坏的图像文件、无效的 YAML 元数据和文件系统失败
会被记录为带错误码和消息的错误，而不是与普通缓存 miss 混在一起。

## 缓存命令

| 操作 | 效果 |
| --- | --- |
| Clear drive cache | 删除磁盘缓存目录内容并重建根目录。 |
| Clear memory cache | 清理服务当前跟踪的内存 HP 缓存和 RT 临时状态。 |
| Clear cache | 同时清理磁盘和内存缓存。 |
| Cache all nodes | 在配置允许时将具有 HP 输出的节点保存到磁盘。 |
| Free transient memory | 清理非终点节点的内存缓存状态。 |
| Synchronize disk cache | 保存 HP 输出，并为没有 HP 输出的节点移除陈旧磁盘文件。 |

磁盘缓存保存、加载和同步只使用 `cached_output_high_precision`。RT 输出不会保护陈旧磁盘文件，也不会被提升为磁盘缓存状态。

## 缓存规则

- 新 HP 代码写入 `cached_output_high_precision`。
- 新 RT 代码将 `cached_output_real_time` 写为临时交互式状态。
- 正式缓存的保存、加载、同步行为、后续 HP 计算和长期存储必须使用 HP 输出，不能将 RT 输出提升为权威缓存。
- 测试应分别验证 HP 和 RT 字段。

`GraphInspectService` 按 HP、RT 顺序选择显示来源，并明确标注所选来源。RT 元数据可以用于 inspect 展示，但会标注为临时状态，而不是正式缓存权威。
