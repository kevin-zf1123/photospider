# Plugin ABI

Photospider supports operation plugins and scheduler plugins. Operation plugins
extend `OpRegistry`. Scheduler plugins provide `IScheduler` implementations.

## Operation Plugin ABI

An operation plugin exports:

```cpp
extern "C" void register_photospider_ops();
```

The loader calls this function after loading the dynamic library. The plugin
registers operations through `OpRegistry`.

Supported operation registrations include:

| Registration | Meaning |
| --- | --- |
| HP monolithic | Full-image HP implementation. |
| HP tiled | Tile-based HP implementation. |
| RT tiled | Tile-based RT implementation. |
| Dirty ROI propagator | Backward ROI propagation. |
| Forward ROI propagator | Downstream ROI projection. |
| Dependency LUT builder | Data-dependent spatial dependency map. |
| Device implementation | CPU, Metal, CUDA, or future backend implementation metadata. |

Operation plugins depend on the public `ImageBuffer` and `NodeOutput`
contracts.

Current operation plugins that are intended to be loadable through
`plugin_dirs` must also register explicit dirty and forward ROI propagators. The
registry still provides legacy identity fallback for migration, but that
fallback is not a complete plugin contract. Pointwise image operations can
register pass-through ROI functions; side-effecting monolithic operations must
document their side-effect semantics and still register explicit propagators
that describe upstream demand and downstream affected-region metadata.

The standard example plugins follow this rule:

| Plugin op | Execution shape | ROI contract |
| --- | --- | --- |
| `image_process:invert` | HP monolithic pointwise image transform | Explicit pass-through dirty and forward ROI. |
| `image_process:threshold` | HP monolithic pointwise image transform | Explicit pass-through dirty and forward ROI. |
| `io:save` | HP monolithic side-effect sink | Explicit pass-through planning metadata; execution rewrites the full file. |
| `image_generator:perlin_noise_metal` | HP monolithic Metal generator | Explicit generator-local pass-through ROI metadata; tiled Metal execution is not enabled. |

## Operation Plugin Library Lifetime

Operation callbacks registered by a plugin may point to code or callable
objects inside that plugin's dynamic library. The host must therefore retain
the library handle for as long as any registered operation key from that plugin
can be resolved from `OpRegistry`.

`PluginManager` owns operation plugin handles. A successful load records the
absolute plugin path, the operation keys registered or replaced by the plugin,
the previous registry/source state for those keys, and a retained RAII library
handle. Unload first removes the plugin's callbacks from `OpRegistry`, restores
any previous implementation that the plugin replaced, then releases the
retained handle. This ordering prevents the registry from exposing callbacks
whose code has already been unmapped while still allowing overriding plugins to
be unloaded cleanly.

If an older plugin has already been shadowed by a newer plugin, unloading the
older plugin may remove no active operation keys. `PluginManager` still clears
dependent restoration snapshots before releasing the older handle so the newer
plugin cannot later restore callbacks into an unmapped library.

The legacy `load_plugins` helper keeps successful operation plugin libraries
resident for process lifetime. Callers that need explicit unload semantics
should use `PluginManager` or the handle-retaining loader API rather than
dropping the library immediately after registration.

## Scheduler Plugin ABI

A scheduler plugin exports C symbols with these names:

```text
ps_scheduler_plugin_get_count
ps_scheduler_plugin_get_name
ps_scheduler_plugin_get_description
ps_scheduler_plugin_create
ps_scheduler_plugin_destroy
ps_scheduler_plugin_get_version
```

Required exports:

| Export | Required | Meaning |
| --- | --- | --- |
| `get_count` | Yes | Number of scheduler types in the plugin. |
| `get_name` | Yes | Type name at an index. |
| `create` | Yes | Create a scheduler instance for a type. The returned object must implement both `IScheduler` and `SchedulerTaskRuntime`. |
| `get_description` | No | Human-readable type description. |
| `destroy` | Required for ownership | Destroy plugin-created scheduler instance. |
| `get_version` | No | Plugin version string. |

## Scheduler Instance Runtime Contract

The current scheduler plugin instance is a C++ object returned as
`ps::IScheduler*`, but the host also requires the same object to implement
`ps::SchedulerTaskRuntime`. The loader validates this with `dynamic_cast` during
creation. A plugin that exports valid C symbols and returns an `IScheduler`
which does not also implement `SchedulerTaskRuntime` can be discovered, but it
will be rejected when the host tries to instantiate that scheduler type.

This is part of the current transitional C++ ABI. Plugin authors should inherit
both interfaces in the concrete scheduler class until the long-term pure C ABI
replaces this requirement.

## Scheduler Instance Ownership

Scheduler instances created by a plugin must be destroyed through that plugin's
destroy function. The loader must not rely on default C++ deletion for
plugin-created instances.

This rule avoids allocator, runtime, and dynamic-library boundary problems.

The scheduler plugin library must remain loaded while any scheduler instance
created by that plugin may still exist.

## Current ABI Status

The current scheduler plugin interface uses C symbol names but returns C++
`ps::IScheduler*`. That means binary compatibility currently depends on a
compatible C++ ABI, compiler, standard library, and Photospider interface
version.

This is transitional. The long-term direction is a pure C scheduler ABI using
opaque handles or callback tables so plugins do not depend on C++ binary ABI.

## Compatibility Guidelines

- Operation plugins should use the published registration APIs and public data
  contracts.
- Scheduler plugins should treat both `IScheduler` and `SchedulerTaskRuntime`
  ABI compatibility as version sensitive.
- Scheduler plugin authors should implement `ps_scheduler_plugin_destroy`.
- The host should use plugin destroy for plugin-created scheduler instances.
- Future C ABI work should be done as a separate compatibility change.
