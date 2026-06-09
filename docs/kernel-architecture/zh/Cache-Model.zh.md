# 内核缓存模型

内核在每个 `Node` 上拥有内存缓存字段，并在图缓存根目录下拥有磁盘缓存文件。本文档定义目标缓存语义。

## 正式内存缓存

| 字段 | 状态 | 含义 |
| --- | --- | --- |
| `cached_output_high_precision` | 正式 | 完整质量 HP 输出。 |
| `cached_output_real_time` | 正式 | 交互式 RT 输出。 |
| `cached_output` | 迁移残留 | HP 输出的旧误命名。 |

`cached_output` 不是独立的长期缓存类型。它是 HP 缓存的旧名称，应迁移到 `cached_output_high_precision`。

## HP 缓存

HP 计算写入 `cached_output_high_precision`。HP 缓存是节点的权威完整质量结果。

相关字段：

| 字段 | 含义 |
| --- | --- |
| `hp_version` | HP 输出变化的版本计数器。 |
| `hp_roi` | 最近更新或合并后的 HP 区域。 |

## RT 缓存

RT 计算写入 `cached_output_real_time`。RT 缓存是交互式预览或代理结果，分辨率可能低于 HP 输出。

相关字段：

| 字段 | 含义 |
| --- | --- |
| `rt_version` | RT 输出变化的版本计数器。 |
| `rt_roi` | RT 更新所代表的最近或合并后的 HP 空间区域。 |

## 遗留 HP 名称

`cached_output` 存在于遗留计算路径和当前迁移支持中。它应被视为错误的 HP 缓存名称。迁移应将 HP 读写移动到 `cached_output_high_precision`，并在调用点更新后移除镜像。

## 磁盘缓存

`GraphCacheService` 处理 `GraphModel::cache_root` 下的磁盘缓存文件。节点缓存条目描述缓存类型和位置。图像缓存文件保存为图像文件，命名的 `NodeOutput::data` 条目保存为图像文件旁边的 YAML 元数据。

磁盘缓存精度当前支持 `int8` 和 `int16` 保存路径。加载的图像缓存数据会转换为浮点图像缓冲区。

## 缓存命令

| 操作 | 效果 |
| --- | --- |
| Clear drive cache | 删除磁盘缓存目录内容并重建根目录。 |
| Clear memory cache | 清理服务当前跟踪的内存缓存状态。 |
| Clear cache | 同时清理磁盘和内存缓存。 |
| Cache all nodes | 在配置允许时将已缓存节点输出保存到磁盘。 |
| Free transient memory | 清理非终点节点的内存缓存状态。 |
| Synchronize disk cache | 保存内存缓存并移除陈旧磁盘文件。 |

部分缓存命令仍在旧 `cached_output` 路径上运行。这是已知迁移区域。

## 迁移规则

- 新 HP 代码写入 `cached_output_high_precision`。
- 新 RT 代码写入 `cached_output_real_time`。
- 新代码不应依赖 `cached_output`。
- 兼容 fallback 只能在 HP 调用点迁移到 `cached_output_high_precision` 期间读取 `cached_output`。
- 测试应分别验证 HP 和 RT 字段。

出于兼容性，`GraphInspectService` 依次偏好 HP、RT、遗留 `cached_output`。未来 inspect 工作应显式显示 HP 和 RT 元数据，而不是折叠成一个被选中的输出。

