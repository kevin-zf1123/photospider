# Photospider User Manual

This manual describes the current command-line and REPL behavior of
Photospider. For architecture internals, see
`docs/kernel-architecture/Overview.md`.

## 1. Build And Setup

Install the required C++ toolchain, OpenCV, yaml-cpp, and test dependencies for
the default CLI and GoogleTest profile. Initialize submodules when using the
vendored FTXUI source:

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target graph_cli -j
```

The two product capabilities default to `ON`. Dependent target defaults follow
the capability state:

| Option | Default and effect |
| --- | --- |
| `PHOTOSPIDER_ENABLE_OPENCV` | `ON`; enables the OpenCV image-processing implementation, image codec, public adapter, and the default OpenCV provider/plugin choices. |
| `PHOTOSPIDER_ENABLE_YAML` | `ON`; enables YAML graph-document and cache-metadata adapters. |
| `PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER` | Follows `PHOTOSPIDER_ENABLE_OPENCV`; explicitly requesting it while OpenCV is disabled is invalid. |
| `PHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS` | Follows `PHOTOSPIDER_ENABLE_OPENCV`; explicitly requesting it while OpenCV is disabled is invalid. |
| `PHOTOSPIDER_BUILD_GRAPH_CLI` | Defaults to `ON` only when both OpenCV and YAML are enabled; explicitly requesting it without both capabilities is invalid. |

With `BUILD_TESTING=ON`, the provider-dependent broad suite is available only
when both capabilities, the graph CLI, the repository OpenCV provider, and the
repository OpenCV plugins are enabled. Explicitly disabling only the provider
keeps provider-independent focused targets and
`DependencyDisabledInstallSmoke` available without registering the broad
suite.

A dependency-disabled installed Host build is:

```bash
cmake -S . -B build/minimal \
  -DBUILD_TESTING=OFF \
  -DPHOTOSPIDER_BUILD_IPC=OFF \
  -DPHOTOSPIDER_ENABLE_OPENCV=OFF \
  -DPHOTOSPIDER_ENABLE_YAML=OFF
cmake --build build/minimal --target photospider_kernel photospider -j
cmake --install build/minimal --prefix /desired/fresh/prefix
```

Use a fresh build and install prefix when changing capability values. This
profile supports neutral image allocation plus in-memory and empty-session Host
lifecycles. Explicit image-artifact or YAML persistence operations return
`GraphErrc::Io`; `graph_cli` is unavailable because its file-oriented workflow
requires both capabilities.

The main executable is:

```text
build/bin/graph_cli
```

On macOS/Linux, when `PHOTOSPIDER_BUILD_IPC=ON`, build the local daemon
separately:

```bash
cmake --build build --target photospiderd -j
```

Plugin and runtime outputs:

| Output | Path | Availability |
| --- | --- | --- |
| executable | `build/bin/graph_cli` | when `PHOTOSPIDER_BUILD_GRAPH_CLI=ON` |
| local daemon | `build/bin/photospiderd` | macOS/Linux with `PHOTOSPIDER_BUILD_IPC=ON` |
| backend libraries | `build/lib` | base build |
| operation plugins | `build/plugins` | when their targets are built |
| policy plugins | `build/policies` | when their targets are built |
| test binaries | `build/tests` | with `BUILD_TESTING=ON` |

## 2. Command-Line Mode

Show top-level options:

```bash
./build/bin/graph_cli --help
```

Supported options:

| Option | Argument | Description |
| --- | --- | --- |
| `-h`, `--help` | none | Print top-level CLI help. |
| `-r`, `--read` | `<file>` | Load YAML into the `default` session and use the Host-returned session as the target of later actions in this invocation. |
| `-o`, `--output` | `<file>` | Save the current graph YAML. Requires `--read` first. |
| `-p`, `--print` | none | Print the loaded graph dependency tree. |
| `-t`, `--traversal` | none | Print dependency tree and post-order traversal. |
| `--clear-cache` | none | Clear cache for the current graph after loading one. |
| `--config` | `<file>` | Use a custom config YAML file. |
| `--repl` | none | Enter the interactive REPL after CLI actions. |

Examples:

```bash
./build/bin/graph_cli --read graph.yaml --print
./build/bin/graph_cli --read graph.yaml --traversal
./build/bin/graph_cli --config config.yaml --read graph.yaml --repl
```

Top-level CLI mode does not expose `--compute` or `--save`. Use the REPL
commands `compute` and `save` for execution and image output.
Ordered top-level actions always target the successful `-r` result, even when
`switch_after_load` disables interactive session switching. The short `-t`
option takes no argument, exactly like `--traversal`.

Top-level actions continue in command-line order after recoverable failures so
later independent actions can still run. If any requested read, output, print,
traversal, or cache-clear action fails—or an action is missing its required
loaded graph—the invocation returns status 2 before either the default REPL
fallback or an explicit `--repl` can start. Earlier successful action effects
are not rolled back. An invocation with no top-level action still starts the
REPL normally, and `--repl` enters it after all preceding actions succeed.

Process exit statuses are 0 for successful actions or a normal REPL exit, 1
for an invalid command-line option, 2 for a recoverable configuration,
startup, action, or REPL failure, and 3 for resource exhaustion.

### Local daemon and typed IPC client

On macOS/Linux, `PHOTOSPIDER_BUILD_IPC` defaults to `ON`. Start the foreground
daemon on its protected per-user socket:

```bash
./build/bin/photospiderd
```

An explicit socket must be absolute and live in a uid-owned protected
directory:

```bash
mkdir -m 700 /tmp/photospider-demo
./build/bin/photospiderd --socket /tmp/photospider-demo/daemon.sock
```

The daemon creates a persistent mode-`0600` `${socket}.lock`, holds an
exclusive nonblocking lifecycle lock while inspecting/reclaiming and serving
the socket, and releases it only after socket cleanup. The lock file remains
after shutdown so concurrent or later instances always synchronize on one
stable inode.

The installed C++17 target `Photospider::photospider_ipc_client` exposes the
move-only `ps::ipc::Client` and complete `create_ipc_host(socket_path)` adapter.
The Client provides the exact 60 typed version 2 methods for daemon identity,
graph/inspection, polling compute and protected image metadata, bounded
events/traces, cache, operation plugins, process policy, and private execution
routes. Graph loads retain the caller's safe Host session name as request
metadata but return a separate opaque daemon session id; disconnecting the
client does not close that session.

An IPC-only CMake consumer selects the installed component explicitly:

```cmake
find_package(Photospider CONFIG REQUIRED COMPONENTS ipc_client)
target_link_libraries(app PRIVATE Photospider::photospider_ipc_client)
```

This lookup resolves only Threads. The default component-less lookup and
`COMPONENTS embedded` discover only the dependencies enabled in the producer:
OpenCV and yaml-cpp in the default profile, neither in the
dependency-disabled profile, plus Threads and applicable Apple framework
dependencies when required by enabled features. Unknown required components,
or required `ipc_client` against an IPC-disabled install, fail package
discovery. An OpenCV-disabled install reports optional `operation_opencv` as
unavailable and rejects it when required; it does not export the adapter target
or install its header.

Extension DSOs use separate installed components:

```cmake
find_package(Photospider CONFIG REQUIRED COMPONENTS operation_sdk)
target_link_libraries(my_operation PRIVATE Photospider::operation_sdk)

find_package(Photospider CONFIG REQUIRED COMPONENTS policy_sdk)
target_link_libraries(my_policy PRIVATE Photospider::policy_sdk)
```

`operation_sdk` contains the v2 `ps::plugin` contracts and transitively links
the no-external-dependency `operation_runtime` image factories. An adapter user
requests `operation_opencv`, which discovers only OpenCV `core`; algorithm-
specific modules remain the plugin's responsibility. `policy_sdk` is a
dependency-neutral C11/C++17 header-only contract. Policy DSOs export
`ps_policy_plugin_get_abi_version` and `ps_policy_plugin_get_api_v1`; callbacks
receive only bounded immutable ranking snapshots and return one candidate id or
an abstention. They receive no worker, queue, resource grant, Graph, Run, or
lifecycle capability. The operation SDK likewise exposes no mutable backend
owner, and old source-tree extension includes have no forwarding compatibility.

The IPC Host implements all 58 current non-destructor Host virtuals through
short-lived typed connections. Compute submits once, polls immediately and then
at a 10/20/40/80/160/320/500-ms cadence without a synchronous total timeout;
owned async workers are stopped, woken, interrupted, completed as Transport
`client_stopped` (5), and joined during adapter destruction. Image mode
performs strict same-user artifact validation while the delivery lease is
active, creates a shared read-only mapping, and then releases the matching
job/lease. The final image reference unmaps and closes the retained descriptor
exactly once.

Every accepted compute reports `cancellable:false` and advances only through
`queued`, `running`, `succeeded`, or `failed`. A terminal failure is a normal
successful polling response containing an immutable exact Graph- or
Daemon-domain `OperationStatus`; RPC/admission/lookup failures remain separate.
Across embedded and IPC calls, the sole status vocabulary distinguishes
`none`, `transport`, `protocol`, `graph`, and `daemon`, and callers must branch
on domain/code/name rather than diagnostic text. The
daemon retains at most 64 active jobs, 256 terminal jobs, 64 image artifacts,
one GiB of artifact bytes with a 512-MiB per-artifact ceiling, 8,192 compute
events per session, and 65,536 execution-trace entries per session. Event
drains are destructive pages of at most 1,024 entries; trace pages are
non-destructive pages of at most 4,096 entries, and every frame is capped at
16 MiB. Issue #73 adds private cooperative cancellation inside the embedded
Kernel/ComputeService boundary, but it does not add a Host virtual, CLI command,
or IPC method. User-controlled compute cancellation, `daemon.shutdown`, TCP,
Windows transport, and `graph_cli --connect` therefore remain unavailable, and
daemon jobs continue to report `cancellable:false`. The CLI options and REPL
commands in this manual remain local embedded-Host behavior and do not
auto-connect to a daemon. See
`docs/codebase-structure/IPC-Protocol-v2.md` for framing, errors, polling,
output security, socket selection, and lifecycle details.

## 3. REPL Mode

Start the REPL:

```bash
./build/bin/graph_cli
```

or load a graph first:

```bash
./build/bin/graph_cli --read graph.yaml --repl
```

The prompt is `ps>`. Type `help` for all commands or `help <command>` for
command-specific help.

## 4. Session Commands

| Command | Description |
| --- | --- |
| `read <file>` | Load YAML into the current session, creating `default` when needed. |
| `load <name> [yaml]` | Load a named graph session. If `yaml` is omitted, loads `sessions/<name>/content.yaml`. |
| `switch <name> [c]` | Switch active sessions. With `c`, save the current session before copying it; a save failure aborts before target files are created or loaded. |
| `close <name>` | Close a loaded graph session. |
| `graphs` | List loaded sessions and mark the current one. |
| `source <file>` | Execute REPL commands from a script file; blank lines and `#` comments are ignored. |
| `exit`, `quit`, `q` | Leave the REPL. |

## 5. Graph Inspection Commands

| Command | Description |
| --- | --- |
| `print [all|<id>] [full|simplified] [inspect|i]` | Print dependency trees. `inspect` also dumps cached debug/spatial metadata. |
| `traversal [f|s|n] [m|d|md] [c|cr]` | Print post-order evaluation for ending nodes. Optional flags control tree detail, cache status, and disk sync. |
| `inspect <node_id> | all | dirty` | Display cached output metadata or the latest backend dirty snapshot. |
| `ops [all|builtin|plugins]` | List registered operations and plugin sources. |
| `output <file>` | Save the active graph YAML. |
| `clear-graph` | Remove all nodes and edges from the active graph. |

`print` modes:

- `full`: include parameters.
- `simplified`: hide parameters.
- `inspect` or `i`: include cache/debug metadata for each node.

`inspect dirty` is read-only. It reports the latest backend dirty region
snapshot when one exists, but it does not trigger realtime update or dirty
compute from the CLI.

`traversal` flags:

- `f`, `s`, `n`: full tree, simplified tree, or no tree.
- `m`, `d`, `md`: memory, disk, or both cache status.
- `c`: check and save cache state.
- `cr`: synchronize disk cache and remove orphaned files.

## 6. Compute And Cache Commands

| Command | Description |
| --- | --- |
| `compute <id|all> [flags]` | Compute one node or all ending nodes. |
| `save <id> <file>` | Compute a node and save its floating-point image output. |
| `clear-cache [m|d|md]` or `cc [m|d|md]` | Clear memory cache, disk cache, or both. |
| `free` | Free memory used by non-essential intermediate nodes. |

`compute` flags:

| Flag | Description |
| --- | --- |
| `force` | Clear in-memory caches before compute. |
| `force-deep` | Clear memory and disk caches before compute. |
| `parallel` | Use multi-threaded compute where supported. |
| `t`, `-t`, `timer` | Print simple timing to the console. |
| `tl`, `-tl <path>` | Write detailed timings to a file. |
| `m`, `-m`, `mute` | Suppress node result output. |
| `nosave`, `ns` | Skip saving caches for this compute. |

Examples:

```text
compute 7 parallel t
compute all force tl out/timer.yaml
save 7 out/result.png
```

The CLI and REPL compute surface is permanently batch-oriented. It does not
expose RT intent commands, dirty ROI creation, or dirty source lifecycle
commands such as `compute rt`, `--dirty-roi`, `dirty begin`, `dirty update`, or
`dirty end`. `RealTimeUpdate` and dirty source lifecycle APIs are reserved for
kernel tests and future GUI/WebUI-style frontends, not for `graph_cli`.

## 7. TUI Commands

| Command | Description |
| --- | --- |
| `node [<id>]` | Open the FTXUI node editor and optionally focus a node. |
| `config` | Open the interactive configuration editor. |
| `benchmark <benchmark_dir>` | Open the benchmark-suite TUI editor. |

The node editor lets you inspect graph structure, edit node YAML, apply or
discard edits, and open the selected node in `$EDITOR`.

The config editor edits the active `CliConfig` fields and can save or apply the
current values depending on `config_save_behavior`.

## 8. Policy And Execution Commands

Use `policy` to inspect process policy bindings and load authority-free ranking
plugins:

```text
policy list
policy get [interactive|throughput|all]
policy set <interactive|throughput> <type>
policy set all <interactive-type> <throughput-type>
policy scan [directory]
policy load <path>
policy plugins
```

Built-in policy types are `interactive` and `throughput`. A policy ranks only
work already admitted by the Host; it does not own workers, queues, resources,
Graph state, or execution routes.

Use `execution` to inspect or replace the current session's private physical
routes:

```text
execution list
execution get [hp|rt|all]
execution set <hp|rt> <type>
execution set all <hp-type> <rt-type>
```

Execution routes are fixed Host implementation values, not plugins:

| Type | Description |
| --- | --- |
| `cpu` | Shared Host-owned CPU execution service; CPU only. |
| `serial_debug` | CPU worker-zero single-flight route for debugging; CPU only. |
| `gpu_pipeline` | Private Metal/CPU route: Metal then CPU when Metal is available, otherwise CPU only. |

The process service freezes the selected operation implementation and device
before admitting full HP, dirty HP/RT, or connected-parameter preflight work.
CPU fallback uses the fixed CPU pool; Metal work uses one dedicated private
lane. Both lanes share the same Run limit, resource ledger, cancellation,
completion, exception, reuse, and shutdown behavior. A Graph stores only its
copied route id and generation and never owns either lane.

Example:

```text
policy get all
policy scan build/policies
execution get all
execution set hp serial_debug
```

## 9. Benchmark Commands

| Command | Description |
| --- | --- |
| `benchmark <benchmark_dir>` | Edit `<benchmark_dir>/benchmark_config.yaml` in the benchmark TUI. |
| `bench <benchmark_dir> <output_dir>` | Run enabled benchmark sessions and write `summary.md` and `raw_data.csv`. |

Example:

```text
benchmark benchmarks/milestone2
bench benchmarks/milestone2 out/milestone2_results
```

## 10. Configuration

The default config file is `config.yaml`. The root `config.yaml` is intentionally
ignored by the project `.gitignore`; it is a local override, not a tracked
repository default. If it is missing, `graph_cli` creates one with the generated
default values shown below. If it exists, `graph_cli` loads that local file
as-is, so local entries such as `cache_precision`, `plugin_dirs`, or
`default_compute_args` can intentionally differ from generated defaults.

| Key | Default | Description |
| --- | --- | --- |
| `cache_root_dir` | `cache` | Shared disk cache root; each loaded graph uses `<cache_root_dir>/<graph_name>`. |
| `plugin_dirs` | `[build/plugins]` | Operation plugin search paths. |
| `policy_dirs` | `[build/policies]` | Policy plugin search paths. |
| `cache_precision` | `int8` | Disk cache image precision. |
| `history_size` | `1000` | REPL history length. |
| `default_print_mode` | `full` | Default `print` mode. |
| `default_traversal_arg` | `n` | Default traversal flags. |
| `default_cache_clear_arg` | `md` | Default cache clear target. |
| `default_exit_save_path` | `graph_out.yaml` | Default save path on exit prompts. |
| `exit_prompt_sync` | `true` | Prompt to sync cache on exit. |
| `config_save_behavior` | `current` | Config editor save behavior. |
| `editor_save_behavior` | `ask` | Node editor save behavior. |
| `default_timer_log_path` | `out/timer.yaml` | Default detailed timing path. |
| `default_ops_list_mode` | `all` | Default `ops` list mode. |
| `ops_plugin_path_mode` | `name_only` | How plugin paths are displayed. |
| `default_compute_args` | empty | Default flags appended to `compute` when none are provided. |
| `switch_after_load` | `true` | Make a graph loaded interactively in the REPL the active session; ordered top-level action targeting is unaffected. |
| `session_warning` | `true` | Warn before overwriting session content. |
| `policy_interactive_type` | `interactive` | Process policy type for latency-sensitive ready work. |
| `policy_throughput_type` | `throughput` | Process policy type for background ready work. |
| `execution_hp_type` | `cpu` | Private route captured by future HP graph sessions. |
| `execution_rt_type` | `cpu` | Private route captured by future RT graph sessions; this does not enable CLI RT commands. |
| `execution_worker_count` | `0` | Host CPU worker request: `0` resolves automatically; `1..8` stays exact. Other values are invalid. |

At startup the CLI scans `policy_dirs`, applies both process policy bindings as
one transaction, and applies future-session execution defaults. Policy types
may come from built-ins or successfully loaded policy DSOs. Execution routes
are the fixed `cpu`, `gpu_pipeline`, and `serial_debug` vocabulary and cannot be
loaded from a DSO. Every removed `scheduler_*` key is rejected rather than
translated.

The YAML loader and config editor reject negative, malformed, and greater-than-
eight worker values instead of clamping them. The Host fixes one process CPU
pool on first configuration/use; zero or an equal request preserves it, while a
different positive request is rejected. Existing Graph sessions keep their
captured HP/RT route ids when future defaults change. `execution set` replaces
only the selected current-session route and does not construct a plugin owner.

`policy plugins` reports DSO labels scanned from `policy_dirs`; `policy get all`
reports the actual Interactive/Throughput binding types and generations.
`execution get all` reports copied HP/RT route information for the current
Graph session. These observations expose no context, executor, queue, or DSO
lease.

`cache_root_dir` is applied before graph load as well. Relative values are
resolved from the current working directory, and the graph cache root becomes
`<cache_root_dir>/<graph_name>`. Lower-level `Kernel` callers that omit this
setting continue to use the session-local `sessions/<name>/cache` fallback.

Metal operation plugins are manual config entries, not generated defaults. On
macOS, CMake places the Metal loader plugin in `build/high_performance/metal`
and its implementation library under `build/high_performance/metal/ops`. Add the
loader directory to `plugin_dirs` before starting `graph_cli` if
`image_generator:perlin_noise_metal` should be scanned and registered:

```yaml
plugin_dirs:
  - build/plugins
  - build/high_performance/metal
```

## 11. Built-In Operations

Dependency-neutral built-ins are registered in `src/lib/core/ops.cpp`.
OpenCV-backed entries are provided by the optional
`PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER` module in
`src/lib/providers/opencv/`.

| Type | Subtype | Description |
| --- | --- | --- |
| `image_source` | `path` | Load an image from a local path. |
| `image_generator` | `constant` | Create a constant image. |
| `image_generator` | `perlin_noise` | Generate a Perlin noise image. |
| `analyzer` | `get_dimensions` | Emit width and height metadata. |
| `math` | `divide` | Divide numeric operands. |
| `image_process` | `convolve` | Apply a convolution kernel image. |
| `image_process` | `resize` | Resize an image. |
| `image_process` | `crop` | Crop a rectangular region. |
| `image_process` | `extract_channel` | Extract B/G/R/A or numeric channel. |
| `image_process` | `gaussian_blur` | Gaussian blur with monolithic and tiled implementations. |
| `image_process` | `curve_transform` | Apply the curve transform kernel. |
| `image_mixing` | `add_weighted` | Weighted blend of two images. |
| `image_mixing` | `diff` | Absolute difference of two images. |
| `image_mixing` | `multiply` | Pixel-wise multiplication. |

Plugin examples from `plugins/ops/` add `image_process:invert`,
`image_process:threshold`, and `io:save`. On macOS, Metal builds also produce
`image_generator:perlin_noise_metal`, but it is only scanned when
`build/high_performance/metal` is manually present in `plugin_dirs`.

## 12. Validation

Build the CLI:

```bash
cmake --build build --target graph_cli -j
```

Build and run focused IPC product tests on macOS/Linux:

```bash
cmake --build build --target photospider_ipc_client \
  photospider_ipc_server_internal photospiderd test_ipc_protocol test_ipc_host \
  test_compute_request_registry test_collection_snapshot_registry \
  test_output_store test_event_stream_boundaries test_ipc_daemon \
  public_header_self_containment -j
ctest --test-dir build --output-on-failure \
  -R '^(FrameCodec|ProtocolEnvelope|IntegerCodec|ProtocolErrors|ProtocolParams|ProtocolGraphLoad|ProtocolGraphClose|ProtocolOperationPlugins|HostRoutedGraphStateProtocolTest|StableInspectionPagingProtocolTest|InspectionJson|SessionRegistry|ComputeRequestRegistry|CollectionSnapshotRegistry|OutputStore|ComputeEventRing|ExecutionTraceRing|UnixSocketConnect|ClientLifecycle|ClientSurface|ClientCollectionAggregation|ClientJobValidation|ClientRetryPolicy|ClientResultValidation|IpcHost|IpcDaemon|IpcDaemonOperationPlugins|IpcDaemonPolicy|IpcDaemonExecution|IpcObservationFixtureDaemon|StaticProductConsumerSmoke|IpcDisabledInstallSmoke|PublicHeaderSelfContainment)'
```

Run registered CTest tests:

```bash
ctest --output-on-failure --test-dir build
```

Run a specific test executable:

```bash
cmake --build build --target test_policy_registry -j
./build/tests/test_policy_registry
```
