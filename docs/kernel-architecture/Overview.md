# Kernel Architecture Overview

This document describes the architecture present in the current branch. Older
phase plans and milestone reports have been moved to `docs/outdated/`.

This directory is the maintained developer documentation for the kernel. Treat
it as the source of truth for public contracts used by operators, schedulers,
plugins, and kernel services. OpenSpec change artifacts are planning material
and are not assumed to be committed with the repository.

## Current State

Photospider is built around a graph runtime with a service split, operation
registry, cache layer, scheduler abstraction, and a frontend-facing Host seam.
Parallel planned work now dispatches through scheduler-owned task runtimes.
Graph-state commands and compute requests that mutate visible graph state enter
an explicit per-graph `GraphStateExecutor` boundary instead of the scheduler
dispatch path. Scheduler-backed parallel compute uses the scheduler runtime for
ready task callbacks inside that graph-state boundary.

The code is useful and testable, but some boundaries are not final. In
particular, `Kernel` and `ComputeService` still coordinate a large amount of
behavior that may eventually move into narrower services.

## Build Modules

The root `CMakeLists.txt` builds these internal modules:

| Target | Role |
| --- | --- |
| `photospider_core_types` | Core data types, OpenCV adapter, YAML node parsing, builtin op registry source. |
| `photospider_operation_plugin_shim` | Narrow shared helper library for dynamic operation callback code that needs `ImageBuffer`/OpenCV adapter symbols without registry state. |
| `photospider_graph` | `GraphModel` plus graph IO, traversal, cache, and inspect services. |
| `photospider_plugin` | Dynamic operation plugin manager and loader. |
| `photospider_compute` | Build-only interaction runtime, schedulers, compute service, and events helper. |
| `photospider` | Static installable backend product, archived as `libphotospider`, linked by CLI and embedded Host frontends. It installs only `include/photospider/**` and exports `Photospider::photospider`; operation plugins register through `OperationPluginRegistrar` and `register_photospider_ops_v1` instead of linking this product for registry state. |
| `photospider_cli_common` | REPL commands, TUI editors, autocomplete, CLI config. |
| `graph_cli` | End-user executable. |

Output directories:

| Output | Path |
| --- | --- |
| executable | `build/bin` |
| libraries | `build/lib` |
| operation plugins | `build/plugins` |
| scheduler plugins | `build/schedulers` |
| tests | `build/tests` |

Package boundary:

- `cmake --install` installs the static `photospider` archive, the
  `include/photospider/**` public header tree, `PhotospiderTargets.cmake`, and
  `PhotospiderConfig.cmake`. The archive is `libphotospider.a` on Unix-like
  toolchains and `photospider.lib` with MSVC.
- `Photospider::photospider` carries `PHOTOSPIDER_STATIC` for consumers and
  keeps `src/` include roots private to repository builds. In the build tree,
  the target's generated public include root contains only `photospider/`
  forwarding headers. CMake tracks additions and removals and the wrappers read
  live source headers without directory symlinks.
- OpenCV (`core`, `imgproc`, `imgcodecs`, `videoio`), `yaml-cpp`, `Threads`,
  platform dynamic-loader libraries, and Apple `Metal`/`Foundation` framework
  flags are implementation link dependencies of the static archive. Library
  dependencies appear as `$<LINK_ONLY:...>` entries on the installed target;
  Apple framework flags are propagated from a private Apple-only product link
  block. Public Host/core headers avoid OpenCV and `yaml-cpp` types; Windows
  consumers receive `PHOTOSPIDER_STATIC` so declarations do not use DLL
  import/export attributes.
- FTXUI, `photospider_cli_common`, operation plugin shim libraries, operation
  plugins, and scheduler plugins are not exported as dependencies of the
  embedded static package.

## Runtime Ownership

```mermaid
graph TD
    CLI["CLI / REPL / TUI"] --> Host["ps::Host"]
    Frontend["Embedded frontend"] --> Host["ps::Host"]
    Host --> EmbeddedHost["embedded Host adapter"]
    EmbeddedHost --> InteractionService
    InteractionService --> Kernel

    Kernel --> GraphRuntime
    Kernel --> GraphIOService
    Kernel --> GraphTraversalService
    Kernel --> GraphCacheService
    Kernel --> GraphInspectService
    Kernel --> RoiPropagationService
    Kernel --> PluginManager

    GraphRuntime --> GraphModel
    GraphRuntime --> GraphStateExecutor
    GraphRuntime --> GraphEventService
    GraphRuntime --> IScheduler

    ComputeService --> GraphModel
    ComputeService --> GraphTraversalService
    ComputeService --> RoiPropagationService
    ComputeService --> GraphExtentResolver
    ComputeService --> GraphCacheService
    ComputeService --> GraphEventService
    ComputeService --> OpRegistry
    ComputeService --> IScheduler

    PluginManager --> OpRegistry
```

## Main Components

| Component | Role |
| --- | --- |
| `Kernel` | Multi-graph facade, service owner, runtime bootstrapper, top-level graph/cache/compute API. |
| `ps::Host` | Public frontend interface under `include/photospider/host`; returns copied request/result/snapshot values and hides Kernel, GraphModel, and GraphRuntime. |
| `embedded Host adapter` | In-process Host implementation backed by `Kernel` and `InteractionService`, used by local frontends while CLI behavior remains unchanged. |
| `GraphRuntime` | Per-graph resource container with model, graph-state executor, events, schedulers, and platform context. |
| `GraphModel` | Graph state holder: private node storage, topology adjacency index, cache root, timing data, quiet/skip-save flags. |
| `InteractionService` | Internal wrapper around `Kernel` used by the embedded Host adapter and backend code; frontends, including the CLI, use the public Host seam. |
| `ComputeService` | Resolves dependencies, checks caches, executes ops, coordinates RT/HP/tiled paths and timing events. |
| `GraphTraversalService` | Topology-only traversal orders, ending-node discovery, ancestor checks, upstream dependency queries, and downstream dependent queries backed by `GraphModel` adjacency. |
| `RoiPropagationService` | ROI/spatial propagation boundary for upstream ROI computation and graph-level forward/backward ROI projection. |
| `GraphExtentResolver` | HP-authoritative output extent resolver used by ROI propagation and dirty-region planning. |
| `GraphCacheService` | Memory/disk cache operations and cache synchronization. |
| `GraphInspectService` | Structured cache/spatial metadata inspection and dependency-tree snapshots built from graph topology. |
| `GraphEventService` | Per-node compute event collection. |
| `PluginManager` | Loads operation plugins, passes the host-provided registrar, records operation sources, and owns dynamic library handles until unload. |
| `OpRegistry` | Global operation implementation registry, including HP/RT, tiled/monolithic, device metadata, and ROI propagators. |

## Maintained Documents

| Document | Scope |
| --- | --- |
| `Overview.md` | Top-level module ownership and current architecture state. |
| `Data-Model.md` | `GraphModel`, `Node`, YAML schema, inputs, outputs, parameters, and cache fields. |
| `Compute-Flow.md` | Sequential, parallel, RT, HP, ROI update, and event/timing flow. |
| `Compute-Service-Split.md` | Planned `ComputeService` facade/internal split and TODO boundaries. |
| `Cache-Model.md` | HP/RT memory cache semantics and disk cache behavior. |
| `Graph-Lifecycle.md` | Graph runtime ownership, graph load/reload/edit failure semantics, and `GraphModel::clear()`. |
| `ImageBuffer-Memory-Contract.md` | Public `ImageBuffer` memory/device contract, alignment, stride, and adapter rules. |
| `Dirty-Region-Propagation.md` | ROI propagation, tile mapping, and current tunable tile defaults. |
| `Scheduler-Architecture.md` | Formal `IScheduler` lifecycle, built-in schedulers, and task-runtime dispatch boundary. |
| `Plugin-ABI.md` | Operation plugin and scheduler plugin ABI contracts. |
| `Development-Validation.md` | Mainline macOS architecture, CTest expectations, and follow-up refactor boundaries. |
| `Benchmark-Spikes.md` | Metal adapter and ARM alignment benchmark plans and follow-up status. |

## Compute Flow

Typical REPL compute flow:

1. A REPL command calls the public `ps::Host` interface.
2. The embedded Host adapter translates public values to internal
   `InteractionService` / `Kernel` requests.
3. `Kernel` resolves the active `GraphRuntime`.
4. `Kernel` creates or uses services needed by `ComputeService`.
5. `ComputeService` resolves topology order with `GraphTraversalService`.
6. `ComputeService` checks memory and disk cache with `GraphCacheService`.
7. Dirty-region paths use `RoiPropagationService` and `GraphExtentResolver`
   for ROI demand and HP-authoritative extents.
8. `ComputeService` selects operation implementations from `OpRegistry`.
9. Work executes recursively or through the configured scheduler path.
10. `GraphEventService` records per-node events and timing data.
11. The embedded Host adapter copies results into public Host value snapshots,
    and the CLI renders those values.

Typical embedded Host compute flow:

1. A local frontend creates `ps::Host` through `create_embedded_host()`.
2. The frontend sends `GraphLoadRequest`, `HostComputeRequest`, or inspection
   requests using public value types from `include/photospider/host` and
   `include/photospider/core`.
3. The embedded Host adapter converts those values into existing
   `InteractionService` / `Kernel` requests.
4. Kernel and service execution follows the same graph-state, compute, cache,
   scheduler, and plugin paths used by the CLI.
5. Results are copied back as `OperationStatus`, `GraphInspectionView`,
   `DirtyRegionInspectionSnapshot`, timing/event snapshots, scheduler info, or
   other Host value snapshots. Host callers never receive `Kernel`,
   `GraphModel`, `GraphRuntime`, OpenCV rectangles, or YAML nodes.
6. For Host-submitted async compute, `close_graph()` waits until the Host
   wrapper has converted backend completion into `OperationStatus`. This keeps
   backend `LastError` classifications available to failed async requests before
   graph close clears runtime diagnostics.
7. Recoverable backend failures become Host status/result values, while
   resource exhaustion remains exceptional: non-destructor Host methods and
   consumed async futures may propagate `std::bad_alloc` as documented by the
   installable interface.

## Scheduler Model

The runtime recognizes two compute intents:

| Intent | Meaning |
| --- | --- |
| `GlobalHighPrecision` / HP | Full-quality compute path. |
| `RealTimeUpdate` / RT | Lower-latency update path for interactive workflows. |

Built-in scheduler types:

| Type | Role |
| --- | --- |
| `cpu_work_stealing` | Multi-threaded CPU execution. |
| `serial_debug` | Single-threaded deterministic debugging path. |
| `gpu_pipeline` | Heterogeneous pipeline with CPU/GPU routing. |
| `heterogeneous` | Alias for `gpu_pipeline`. |

The CLI exposes scheduler controls through the `scheduler` REPL command. Default
types and plugin directories are configured in the local `config.yaml`; the root
file is ignored by the repository and should be treated as a per-worktree
override. Startup scans `scheduler_dirs` before graph load so plugin-provided
scheduler types are discoverable during per-graph scheduler injection. Discovery
does not select a scheduler by itself: the active graph uses the configured
`scheduler_hp_type` / `scheduler_rt_type` values, or a later
`scheduler set <hp|rt> <type>` command.

`IScheduler` is the formal lifecycle interface, and scheduler-owned
`SchedulerTaskRuntime` is the dispatch contract for compute-service planned
parallel work. `GraphStateExecutor` is the separate access boundary for
graph-state operations and compute requests that read or mutate the visible
`GraphModel`.

## Operation Registry

Operations are keyed by `type:subtype`. The registry supports:

- legacy monolithic operation registration
- HP monolithic implementations
- HP tiled implementations
- RT tiled implementations
- per-device implementations such as CPU and Metal
- dirty ROI propagators
- forward ROI propagators
- dependency builders

Built-in operations are registered in `src/ops.cpp`. Runtime plugin examples
live in `custom_ops/`.

## Cache Model

The cache layer uses one node-local formal cache plus one runtime-owned RT
proxy graph:

- `Node::cached_output_high_precision`: formal reusable HP cache.
- `RealtimeProxyGraph`: transient low-resolution RT preview/update state keyed
  by node id, not formal cache authority.
- HP version/ROI fields on `Node`; RT version/ROI fields on proxy node state.
- disk cache files under the configured cache root

`GraphCacheService` keeps cache commands centralized. HP code should use
`cached_output_high_precision`; RT code should use `RealtimeProxyGraph` only as
interactive state. Dirty RT worker writes are staged through
`RealtimeProxyWriteBuffer` before proxy commit; dirty HP worker writes are
staged through `HighPrecisionDirtyWriteBuffer` before graph commit. Formal
cache save, load, synchronization behavior, subsequent HP compute, and
long-term storage use HP output.

## ImageBuffer Contract

`ImageBuffer` is a public kernel contract, not an internal implementation
detail. Operators, schedulers, plugins, adapters, and cache code may depend on
its documented fields and invariants.

CPU buffers owned by the kernel must provide 64-byte aligned row starts. `step`
is the row stride in bytes and may be larger than the packed row size to
preserve alignment. ARM Mac high-performance paths may need or benefit from
128-byte alignment, but 128-byte alignment is an optimization target rather than
the portable minimum.

## Dirty Region Propagation

ROI propagation is handled through `RoiPropagationService` using
registry-provided propagators, `GraphModel` topology adjacency, and
`GraphExtentResolver`. The active propagation notes are in
`docs/kernel-architecture/Dirty-Region-Propagation.md`.

Important current behavior:

- identity propagation for source/generator/analyzer/math-style nodes
- specific propagation for `resize`, `crop`, `convolve`, and `gaussian_blur`
- forward propagation for downstream dirty-region projection
- tiled compute metadata for operators that can execute in tile space
- current tile defaults are tunable implementation parameters, not permanent ABI

## Known Architecture Tensions

These are implementation realities, not immediate blockers:

- `Kernel` is broad and acts as graph manager, service owner, runtime manager,
  cache API, compute API, and editing API.
- `ComputeService` contains planning, cache coordination, execution, ROI update,
  scheduler interaction, and metrics emission.
- Cache APIs expose HP and RT concepts, while compute boundary cleanup is still
  ongoing.

The `ComputeService` split is now tracked by the `split-compute-service`
OpenSpec change and the maintained `Compute-Service-Split.md` plan. The
`GraphTraversalService` topology/ROI split has landed: traversal is topology
only, ROI propagation is a separate service, extent resolution is explicit, and
dependency-tree data is structured by the inspection boundary, exposed through
the internal `InteractionService` to the embedded Host adapter, copied into
public Host snapshots, and rendered by CLI/TUI/frontend code.

## Maintained Documentation Boundary

Active docs should describe current behavior. Historical planning artifacts,
phase reviews, dated state reports, and speculative diagrams belong in
`docs/outdated/`.
