# 插件 ABI

Photospider 支持操作插件和调度器插件。操作插件通过 host 提供的 registrar 扩展 host 拥有的
`OpRegistry`。调度器插件提供 `IScheduler` 实现。

## 操作插件 ABI

操作插件导出一个带版本的 registrar 入口：

```cpp
extern "C" PLUGIN_API void register_photospider_ops_v1(
    ps::OperationPluginRegistrar* registrar);
```

加载器在加载动态库后调用该函数。Host 创建 `OperationPluginRegistrar`，把它传给插件，并把每一次注册转发到
host 拥有的 `OpRegistry`。操作插件在注册期间不得调用 `OpRegistry::instance()`，否则动态插件和静态
host 可能观察到不同的 registry singleton。

旧的无参数 `register_photospider_ops()` 不是受支持的兼容 ABI。它使用同一个 C 符号形态但没有签名标记，
因此加载器现在只解析 `register_photospider_ops_v1`，并拒绝旧插件，而不是猜测不兼容的函数指针类型。

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

## 操作插件 Shim 与链接方式

操作插件不会为了访问 registry 符号而链接宽泛的静态 `photospider` 产品。仓库内 operation plugin 通过
`OperationPluginRegistrar` 注册；标准插件 target 在需要 `ImageBuffer`/OpenCV adapter 转换函数等运行时
helper 符号时，链接窄边界 `photospider_operation_plugin_shim`。该 shim 明确不包含 `OpRegistry`、内置
operation 注册、plugin manager、plugin loader、graph、scheduler 或 compute-service 代码。

这个拆分支持静态 host 方向：

- Host 可以在静态链接的 Photospider 内拥有 `OpRegistry`。
- 动态 operation plugin 从 host 接收注册 callback，因此 registry mutation 始终发生在 host 拥有的实例中。
- `photospider_operation_plugin_shim` 只作为插件 callback 代码所需 buffer adapter 函数的共享运行时 helper 边界。
- 插件 callback 对象仍可能指向插件代码，因此只要注册 key 仍然 active，host 就必须保留插件库。

符号可见性规则：

- Operation plugin registrar entry 使用 `PLUGIN_API`，loader 只把
  `register_photospider_ops_v1` 视为受支持的 operation-plugin ABI 入口。
  任何其他外部可见的 callback helper 符号都不是 loader 入口或兼容性契约。
- Operation plugin target 会定义 `PHOTOSPIDER_PLUGIN_BUILD`，因此即使在
  Windows 上，`PHOTOSPIDER_API` 也保持为空。插件 callback 代码不得通过
  `ps::GraphError` 等公共 value contract 导入 backend library 符号；只有
  registrar 入口通过 `PLUGIN_API` 导出。
- Loader 解析精确的带版本符号名。
- Shim 导出插件 callback 所需的运行时 adapter helper 符号；Windows 上使用 `WINDOWS_EXPORT_ALL_SYMBOLS`。
- 这仍然是 C++ ABI 边界，因为 callback 使用 `std::function`、`NodeOutput`、`Node` 和 `OpMetadata`。
  在未来纯 C ABI 替代这些 callback 形态之前，编译器、标准库和 Photospider 头兼容性仍然是版本敏感的。

打算通过 `plugin_dirs` 作为当前可加载插件使用的 operation plugin，也必须显式注册 dirty 与
forward ROI propagator。Registry 仍提供 legacy identity fallback 作为迁移支持，但该 fallback
不是完整插件契约。逐像素图像操作可以注册 pass-through ROI 函数；带副作用的 monolithic
操作必须说明自己的副作用语义，并仍然注册显式 propagator，用来描述上游需求和下游受影响区域元数据。

标准示例插件遵守该规则：

| 插件 op | 执行形态 | ROI 契约 |
| --- | --- | --- |
| `image_process:invert` | HP monolithic 逐像素图像变换 | 显式 pass-through dirty 与 forward ROI。 |
| `image_process:threshold` | HP monolithic 逐像素图像变换 | 显式 pass-through dirty 与 forward ROI。 |
| `io:save` | HP monolithic 副作用 sink | 显式 pass-through planning metadata；执行阶段重写完整文件。 |
| `image_generator:perlin_noise_metal` | HP monolithic Metal generator | 显式 generator-local pass-through ROI metadata；未启用 tiled Metal 执行。 |

## 操作插件库生命周期

插件注册的操作回调可能指向该插件动态库内部的代码或 callable 对象。因此，只要该插件注册的任何操作 key
仍能从 `OpRegistry` 中解析，host 就必须保留该动态库句柄。

`PluginManager` 拥有操作插件句柄。一次成功加载会记录插件绝对路径、插件通过 host-provided registrar
注册或替换的 operation key、这些 key 之前的 registry/source 状态，以及一个由 RAII 管理的动态库句柄。
卸载时会先从 `OpRegistry` 移除该插件的回调，恢复被该插件替换的旧实现，然后释放保留的句柄。这个顺序可以防止
registry 暴露代码已经被卸载的回调，同时允许覆盖型插件被干净卸载。

如果旧插件已经被新插件 shadow，卸载旧插件时可能不会移除任何 active operation key。`PluginManager`
仍会在释放旧句柄前清理依赖该旧插件的恢复快照，防止新插件后续把回调恢复到已卸载的动态库中。

旧的 `load_plugins` helper 会让成功加载的操作插件库在进程生命周期内常驻。需要显式卸载语义的调用方应使用
`PluginManager` 或保留句柄的 loader API，而不是在注册后立即丢弃动态库。

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
| `create` | 是 | 为某类型创建调度器实例。返回对象必须同时实现 `IScheduler` 和 `SchedulerTaskRuntime`。 |
| `get_description` | 否 | 人类可读类型描述。 |
| `destroy` | 所有权所需 | 销毁插件创建的调度器实例。 |
| `get_version` | 否 | 插件版本字符串。 |

## 调度器实例运行时契约

当前调度器插件实例是以 `ps::IScheduler*` 返回的 C++ 对象，但 host 还要求同一个对象实现
`ps::SchedulerTaskRuntime`。加载器会在创建期间通过 `dynamic_cast` 验证这一点。一个插件即使导出了有效的
C 符号，并返回了 `IScheduler`，如果该对象没有同时实现 `SchedulerTaskRuntime`，它可以被发现，但在 host
尝试实例化该调度器类型时会被拒绝。

这是当前过渡性 C++ ABI 的一部分。在长期纯 C ABI 替代该要求之前，插件作者应让具体调度器类同时继承这两个接口。

## 调度器实例所有权

由插件创建的调度器实例必须通过该插件的 destroy 函数销毁。加载器不得依赖默认 C++ deletion 销毁插件创建的实例。

该规则避免 allocator、runtime 和动态库边界问题。

调度器插件库必须在由它创建的任何调度器实例仍可能存在期间保持加载。

## 当前 ABI 状态

当前调度器插件接口使用 C 符号名称，但返回 C++ `ps::IScheduler*`。这意味着二进制兼容性当前依赖兼容的 C++ ABI、编译器、标准库和 Photospider 接口版本。

这是过渡状态。长期方向是使用不透明 handle 或回调表的纯 C 调度器 ABI，使插件不依赖 C++ 二进制 ABI。

## 兼容性指南

- 操作插件应使用已发布的注册 API 和公共数据契约。
- 操作插件必须使用 `OperationPluginRegistrar` 和 `register_photospider_ops_v1`；无参数注册 ABI 不受支持。
- 操作插件不得仅为了共享 registry 状态而链接 `photospider`。只有当插件 callback 代码需要窄运行时 helper
  符号时，才使用 `photospider_operation_plugin_shim`。
- 调度器插件应将 `IScheduler` 和 `SchedulerTaskRuntime` ABI 兼容性都视为版本敏感。
- 调度器插件作者应实现 `ps_scheduler_plugin_destroy`。
- Host 应使用插件 destroy 销毁插件创建的调度器实例。
- 未来 C ABI 工作应作为单独的兼容性 change 完成。
