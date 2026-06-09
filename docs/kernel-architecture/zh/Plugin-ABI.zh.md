# 插件 ABI

Photospider 支持操作插件和调度器插件。操作插件扩展 `OpRegistry`。调度器插件提供 `IScheduler` 实现。

## 操作插件 ABI

操作插件导出：

```cpp
extern "C" void register_photospider_ops();
```

加载器在加载动态库后调用该函数。插件通过 `OpRegistry` 注册操作。

支持的操作注册包括：

| 注册 | 含义 |
| --- | --- |
| HP monolithic | 全图 HP 实现。 |
| HP tiled | 基于 tile 的 HP 实现。 |
| RT tiled | 基于 tile 的 RT 实现。 |
| Dirty ROI propagator | 反向 ROI 传播。 |
| Forward ROI propagator | 下游 ROI 投影。 |
| Dependency LUT builder | 数据依赖空间依赖映射。 |
| Device implementation | CPU、Metal、CUDA 或未来后端实现元数据。 |

操作插件依赖公共 `ImageBuffer` 和 `NodeOutput` 契约。

## 调度器插件 ABI

调度器插件导出这些名称的 C 符号：

```text
ps_scheduler_plugin_get_count
ps_scheduler_plugin_get_name
ps_scheduler_plugin_get_description
ps_scheduler_plugin_create
ps_scheduler_plugin_destroy
ps_scheduler_plugin_get_version
```

必需导出：

| 导出 | 是否必需 | 含义 |
| --- | --- | --- |
| `get_count` | 是 | 插件中的调度器类型数量。 |
| `get_name` | 是 | 某索引处的类型名称。 |
| `create` | 是 | 为某类型创建 `IScheduler` 实例。 |
| `get_description` | 否 | 人类可读类型描述。 |
| `destroy` | 所有权所需 | 销毁插件创建的调度器实例。 |
| `get_version` | 否 | 插件版本字符串。 |

## 调度器实例所有权

由插件创建的调度器实例必须通过该插件的 destroy 函数销毁。加载器不得依赖默认 C++ deletion 销毁插件创建的实例。

该规则避免 allocator、runtime 和动态库边界问题。

调度器插件库必须在由它创建的任何调度器实例仍可能存在期间保持加载。

## 当前 ABI 状态

当前调度器插件接口使用 C 符号名称，但返回 C++ `ps::IScheduler*`。这意味着二进制兼容性当前依赖兼容的 C++ ABI、编译器、标准库和 Photospider 接口版本。

这是过渡状态。长期方向是使用不透明 handle 或回调表的纯 C 调度器 ABI，使插件不依赖 C++ 二进制 ABI。

## 兼容性指南

- 操作插件应使用已发布的注册 API 和公共数据契约。
- 调度器插件应将 `IScheduler` ABI 兼容性视为版本敏感。
- 调度器插件作者应实现 `ps_scheduler_plugin_destroy`。
- Host 应使用插件 destroy 销毁插件创建的调度器实例。
- 未来 C ABI 工作应作为单独的兼容性 change 完成。

