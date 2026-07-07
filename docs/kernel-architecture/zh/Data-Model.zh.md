# 内核数据模型

本文档描述当前内核使用的图和节点数据结构。重点是算子、调度器、插件和前端应依赖的公共行为。

## GraphModel

`GraphModel` 是图的内存状态。每个 `GraphRuntime` 拥有一个 `GraphModel`。

重要字段：

| 字段 | 含义 |
| --- | --- |
| 私有节点存储 | 从节点 id 到 `Node` 的映射，通过 `GraphModel` 查找、遍历和变更 helper 访问。 |
| 拓扑邻接索引 | 面向图像边和参数边的 incoming/outgoing `GraphTopologyEdge` 映射，以稳定节点 id 为键。 |
| `cache_root` | 当前图磁盘缓存文件的已解析根目录。 |
| `timing_results` | 启用计时时的最新计时摘要。 |
| `total_io_time_ms` | 累计磁盘缓存 IO 时间。 |
| disk-cache diagnostic snapshot | 最近一次磁盘缓存加载诊断，包含 skipped/miss/hit/error 状态，并在读取失败时包含错误详情。GraphModel 使用专用 diagnostic mutex 保护该状态，并向读取方暴露值快照。 |

外部代码不得通过原始节点 map 改变图结构。读取使用 `node()`、`find_node()`、`node_ids()` 和受控遍历等 helper。结构变更使用 `add_node()`、`replace_node()`、`remove_node()` 和输入重连 API；这些 helper 会在返回前验证并刷新拓扑邻接。节点本地运行态缓存/状态更新可以使用 `mutable_node()`，但结构编辑仍属于模型变更 helper。

内部服务通过模型边界协调锁、计时、缓存、拓扑和遍历行为。大多数前端代码应通过 `Kernel` 或 `InteractionService` 访问图状态。

对于 CLI 加载的 graph，`cache_root` 会从已加载配置中的 `cache_root_dir` 推导为
`<cache_root_dir>/<graph_name>`；相对路径按进程当前工作目录解析。未提供 cache root 的底层
`Kernel::load_graph` 调用继续使用 session-local fallback：`<root_dir>/<graph_name>/cache`。

`GraphModel::clear()` 的目标是重置模型级运行时状态，而不只是删除节点。清理图会重置节点、拓扑邻接、计时结果、累计 IO 时间、skip-save 状态和其他单次运行状态，使 reload 行为不受陈旧元数据污染。

## 拓扑邻接

`GraphModel` 拥有 `GraphTopologyIndex`，它记录图边的两个方向：

- `incoming_by_node`：某个节点的上游依赖。
- `outgoing_by_node`：某个节点的下游依赖者。

每条 `GraphTopologyEdge` 都保存稳定的源/目标节点 id、边类型（`ImageInput` 或 `ParameterInput`）、源输出名、目标输入/参数身份和输入槽位索引。成功的图加载、清空、节点添加、节点替换、节点删除和输入重连，都会在图状态暴露给遍历、计算、缓存、inspect、CLI 或 interaction 消费者之前刷新或清空该索引。

## 节点身份

每个 `Node` 包含：

| 字段 | 含义 |
| --- | --- |
| `id` | 图内唯一整数 id。 |
| `name` | 人类可读标签。 |
| `type` | 操作族，例如 `image_process`。 |
| `subtype` | 操作子类型，例如 `gaussian_blur`。 |
| `preserved` | 防止某些强制重算路径丢弃该节点。 |

操作查找通过 `OpRegistry` 使用 `type:subtype`。

## 输入

节点输入按数据类型拆分：

| 输入类型 | 结构 | 含义 |
| --- | --- | --- |
| 图像输入 | `ImageInput` | 读取上游类图像 `NodeOutput`。 |
| 参数输入 | `ParameterInput` | 读取上游命名数据输出，并写入运行时参数。 |

旧的统一输入模型不是维护中的 schema 的一部分。

## 参数

`Node::parameters` 包含从图 YAML 加载的静态 YAML 参数。

`Node::runtime_parameters` 在执行时通过克隆静态参数并应用 `parameter_inputs` 重建。算子在计算期间应从 `runtime_parameters` 读取有效值。

## 输出

`NodeOutput` 包含：

| 字段 | 含义 |
| --- | --- |
| `image_buffer` | 以公共 `ImageBuffer` 契约表示的图像负载。 |
| `data` | 作为 YAML 节点保存的命名标量或结构化输出。 |
| `space` | 空间变换、尺度和 ROI 元数据。 |
| `debug` | worker/设备/计时/范围诊断信息。 |

算子可以返回图像数据、命名数据，或两者都返回。

## 缓存字段

与缓存相关的节点字段：

| 字段 | 状态 | 含义 |
| --- | --- | --- |
| `cached_output_high_precision` | 正式缓存 | 完整质量、可复用输出的 HP 缓存。 |

只有 HP 输出是正式可复用缓存。这意味着只有 HP 输出可以进入后续 HP 计算、磁盘缓存、长期存储以及其他可复用缓存行为。RT 输出不存放在 `Node` 上，而是位于 `RealtimeProxyGraph`，后者镜像 node id，并保存低分辨率 proxy output、HP-space ROI、version 和 RT dirty-source generation。

Dirty RT worker task 会先通过 `RealtimeProxyWriteBuffer` stage proxy output，再提交到
`RealtimeProxyGraph`。Dirty HP worker task 会先通过 `HighPrecisionDirtyWriteBuffer`
stage 正式 HP 输出，再提交到 `GraphModel`；RealTimeUpdate 的 HP commit 会被 gate 到成功的
RT proxy commit 之后。

## YAML Schema

图 YAML 根节点是节点对象序列。支持的节点字段：

```yaml
- id: 1
  name: source
  type: image_source
  subtype: path
  preserved: false
  image_inputs:
    - from_node_id: 0
      from_output_name: image
  parameter_inputs:
    - from_node_id: 2
      from_output_name: value
      to_parameter_name: strength
  parameters:
    path: assets/input.png
  outputs:
    - output_id: 0
      output_type: image
  caches:
    - cache_type: image
      location: output.png
```

`id` 是必需字段。其他字段根据 `Node::from_yaml` 默认。`parameter_inputs` 要求 `from_output_name` 和 `to_parameter_name` 非空。

## 空间元数据

`SpatialContext` 携带 ROI 传播和 inspect 使用的变换与 ROI 元数据：

| 字段 | 含义 |
| --- | --- |
| `transform_matrix` | 全局变换矩阵。 |
| `inverse_matrix` | 全局逆变换。 |
| `local_inverse_matrix` | 用于上游 ROI 投影的局部逆变换。 |
| `absolute_roi` | 输出范围或有效区域。 |
| `global_scale_x`, `global_scale_y` | 尺度元数据。 |

`SpatialDependencyMap` 是用于数据依赖空间传播的可选节点本地 LUT。
