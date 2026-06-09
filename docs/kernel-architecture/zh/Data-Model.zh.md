# 内核数据模型

本文档描述当前内核使用的图和节点数据结构。重点是算子、调度器、插件和前端应依赖的公共行为。

## GraphModel

`GraphModel` 是图的内存状态。每个 `GraphRuntime` 拥有一个 `GraphModel`。

重要字段：

| 字段 | 含义 |
| --- | --- |
| `nodes` | 从节点 id 到 `Node` 的映射。 |
| `cache_root` | 磁盘缓存文件的根目录。 |
| `timing_results` | 启用计时时的最新计时摘要。 |
| `total_io_time_ms` | 累计磁盘缓存 IO 时间。 |

内部服务是 `GraphModel` 的 friend，因此它们可以协调锁、计时、缓存和遍历行为。大多数前端代码应通过 `Kernel` 或 `InteractionService` 访问图状态。

`GraphModel::clear()` 的目标是重置模型级运行时状态，而不只是删除 `nodes`。清理图应重置节点、计时结果、累计 IO 时间、skip-save 状态和其他单次运行状态，使 reload 行为不受陈旧元数据污染。

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

正式可复用缓存字段：

| 字段 | 状态 | 含义 |
| --- | --- | --- |
| `cached_output_high_precision` | 正式缓存 | 完整质量、可复用输出的 HP 缓存。 |
| `cached_output_real_time` | 临时 RT 状态 | 交互式预览/更新输出。 |
| `cached_output` | 迁移残留 | HP 缓存的旧误命名。 |

只有 HP 输出是正式可复用缓存。这意味着只有 HP 输出可以进入后续 HP 计算、磁盘缓存、长期存储以及其他可复用缓存行为。`cached_output_real_time` 是临时交互式状态，不能作为权威缓存输出使用。`cached_output` 不是第三种长期缓存类型。它应在整个代码库中迁移到 `cached_output_high_precision`。现有兼容路径只能在调用点迁移前读取或镜像它。

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
