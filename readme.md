# Photospider

Photospider is a C++17 image-processing graph runtime. It loads YAML graphs,
executes node dependencies, caches intermediate results, and exposes both a
command-line entry point and an interactive REPL/TUI for graph inspection and
editing.

The current codebase is a mid-migration runtime: the classic recursive compute
path, cache service, RT/HP scheduler model, ROI propagation, scheduler plugins,
and Metal hooks all coexist. For architecture details, start with
`docs/kernel-architecture/Overview.md`.

## Repository Layout

| Path | Purpose |
| --- | --- |
| `apps/graph_cli/` | Private `graph_cli` application tree: entry point, commands, REPL/TUI, autocomplete, configuration, headers, and help resources. |
| `apps/photospiderd/` | Foreground macOS/Linux daemon process shell and self-pipe signal policy. |
| `src/lib/` | Role-owned backend implementation and private headers: core, graph, compute, runtime, Host, plugin, scheduler, benchmark, adapters, and the internal IPC server/router. |
| `include/photospider/` | The only installable header tree, containing public Host/core contracts and the conditional typed IPC client. |
| `plugins/ops/` | Repository-owned operation plugins, including the private Metal operation implementation. |
| `plugins/schedulers/` | Repository-owned scheduler plugins. |
| `tests/unit/` | Isolated contract tests. |
| `tests/integration/` | Cross-component, runtime, plugin, scheduler, CLI, process, and package behavior tests. |
| `tests/{fixtures,support,verification}/` | Test DSOs, shared internal support, and documented long-lived manual tools. |
| `docs/` | Maintained documentation. Older phase docs live in `docs/outdated`. |
| `util/` | Scratch graphs, examples, and local test inputs. |
| `extern/ftxui/` | Vendored FTXUI source when the submodule is present. |

Generated directories such as `build/`, `cache/`, `out/`, and `sessions/`
should remain untracked.

## Dependencies

Required for the main build:

- C++17 compiler
- CMake 3.16+
- OpenCV components: `core`, `imgproc`, `imgcodecs`, `videoio`
- `yaml-cpp`
- Threads
- FTXUI, either from `extern/ftxui` or an installed CMake package
- `nlohmann_json` 3.9+ when `PHOTOSPIDER_BUILD_IPC=ON` (the default on
  macOS/Linux); it remains private to IPC implementation

Optional or test-only dependencies:

- Apple Metal/Foundation/CoreImage/CoreVideo frameworks on macOS for Metal ops
- GoogleTest and `nlohmann_json` when `BUILD_TESTING=ON`
- CURL and OpenSSL are detected when available

### macOS

```bash
brew install cmake pkg-config opencv yaml-cpp googletest nlohmann-json
git submodule update --init --recursive
```

### Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install build-essential cmake pkg-config libopencv-dev \
  libyaml-cpp-dev libgtest-dev nlohmann-json3-dev
git submodule update --init --recursive
```

## Build

Configure out of tree:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

Build the CLI and backend:

```bash
cmake --build build --target graph_cli -j
```

On macOS/Linux, build the typed IPC client and foreground daemon with:

```bash
cmake --build build --target photospider_ipc_client photospiderd -j
```

The executable is written to:

```text
build/bin/graph_cli
```

Shared libraries are written under `build/lib`, operation plugins under
`build/plugins`, scheduler plugins under `build/schedulers`, and tests under
`build/tests`.

Useful build options:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake -S . -B build -DUSE_ASAN=ON
cmake -S . -B build -DUSE_TSAN=ON
cmake -S . -B build -DPHOTOSPIDER_BUILD_IPC=OFF
```

`PHOTOSPIDER_BUILD_IPC` defaults to `ON` on macOS/Linux and `OFF` elsewhere.
Forcing it on on an unsupported platform fails configuration. With it off, the
package installs neither IPC headers nor the IPC client/daemon targets.

## Test

Run all tests registered with CTest:

```bash
ctest --output-on-failure --test-dir build
```

Build or run a focused test binary:

```bash
cmake --build build --target test_gpu_pipeline_scheduler -j
./build/tests/test_gpu_pipeline_scheduler
```

The current `CMakeLists.txt` keeps stable test target and CTest names while
owning maintained translation units under `tests/unit/` and
`tests/integration/`. `test_propagation` remains a buildable script-driven tool;
the other GoogleTest targets are registered with `gtest_discover_tests`.

## Run

Print command-line options:

```bash
./build/bin/graph_cli --help
```

Load a graph, print it, and exit:

```bash
./build/bin/graph_cli --read path/to/graph.yaml --print
```

Load a graph and enter the REPL:

```bash
./build/bin/graph_cli --read path/to/graph.yaml --repl
```

Start the REPL with no preloaded graph:

```bash
./build/bin/graph_cli
```

Start the local version 1 daemon on its protected per-user socket:

```bash
./build/bin/photospiderd
```

Or choose an explicit absolute socket path in an already protected directory:

```bash
mkdir -m 700 /tmp/photospider-demo
./build/bin/photospiderd --socket /tmp/photospider-demo/daemon.sock
```

`photospiderd` stays in the foreground and shuts down cleanly on SIGINT or
SIGTERM. It serializes same-socket startup with a persistent mode-`0600`
`${socket}.lock` file; the file intentionally remains after shutdown so later
instances synchronize on the same inode. Its typed installable client is
`Photospider::photospider_ipc_client`, with headers under
`photospider/ipc/`. Version 1 exposes the exact 55-method typed Client surface,
including graph/inspection, polling compute and protected image metadata,
bounded events/traces, cache, operation-plugin, and scheduler calls. The
installed `create_ipc_host(socket_path)` adapter implements all 53 current
non-destructor Host virtuals with short-lived typed calls, joined async polling,
exact status propagation, and deterministic `client_stopped` teardown. Its
current secure image consumer strictly validates the artifact and `pread`s an
independent CPU copy before lease-aware release; read-only mmap ownership is the
next migration slice. Compute cancellation, `daemon.shutdown`, TCP, Windows
transport, and `graph_cli --connect` remain unavailable. `graph_cli` therefore
continues to use its embedded Host and all local commands below retain their
existing meaning. See `docs/codebase-structure/IPC-Protocol-v1.md` for the wire,
opaque-session, polling, output-security, permission, and error contracts.

The command-line parser currently supports graph loading, YAML output, tree
printing, traversal display, cache clearing, config selection, and REPL entry.
Graph computation and image saving are REPL commands, not top-level CLI flags.

## REPL Commands

Type `help` in the REPL for the full command list, or `help <command>` for the
text stored in `apps/graph_cli/resources/help/`. CMake configures this resource
root into the build-tree application, so command help does not depend on the
process working directory.

Common commands:

| Command | Purpose |
| --- | --- |
| `read <file>` | Load YAML into the current session. |
| `load <name> [yaml]` | Load or create a named session. |
| `switch <name> [c]` | Switch sessions; `c` copies current content first. |
| `graphs` | List loaded graph sessions. |
| `node [id]` | Open the FTXUI node editor. |
| `print [all|id] [full|simplified] [inspect|i]` | Print dependency trees and optional cache metadata. |
| `inspect <id|all|dirty>` | Inspect cached metadata or the latest backend dirty snapshot. |
| `traversal [f|s|n] [m|d|md] [c|cr]` | Show post-order evaluation and cache state. |
| `ops [all|builtin|plugins]` | List registered operations. |
| `compute <id|all> [flags]` | Compute one node or all ending nodes. |
| `save <id> <file>` | Compute a node and save its image output. |
| `scheduler ...` | Inspect or change HP/RT scheduler configuration. |
| `benchmark <dir>` | Edit a benchmark suite. |
| `bench <benchmark_dir> <output_dir>` | Run benchmark sessions. |

Supported `compute` flags include `force`, `force-deep`, `parallel`,
`t`/`timer`, `tl <path>`, `m`/`mute`, and `nosave`/`ns`.

The CLI and REPL are permanently batch-oriented surfaces. They do not expose
RT intent commands, dirty ROI creation, or dirty source lifecycle commands such
as `compute rt`, `--dirty-roi`, `dirty begin`, `dirty update`, or `dirty end`.
`RealTimeUpdate` and dirty source lifecycle APIs are reserved for kernel tests
and future GUI/WebUI-style frontends, not for `graph_cli`. `inspect dirty` is
read-only inspection of the latest backend dirty snapshot; it does not create a
dirty ROI or trigger realtime compute.

## Configuration

By default, `graph_cli` reads `config.yaml` from the working directory. The root
`config.yaml` is intentionally ignored by the project `.gitignore`, so treat it
as a local override rather than a tracked repository default. If the file is
missing, the app creates one with the generated defaults shown below. If the
file already exists, `graph_cli` loads it as-is, so local values such as
`cache_precision`, `plugin_dirs`, or `default_compute_args` can differ from the
generated defaults.

Important keys:

| Key | Default | Purpose |
| --- | --- | --- |
| `cache_root_dir` | `cache` | Shared disk cache root; each loaded graph uses `<cache_root_dir>/<graph_name>`. |
| `plugin_dirs` | `[build/plugins]` | Operation plugin search paths. |
| `scheduler_dirs` | `[build/schedulers]` | Scheduler plugin search paths. |
| `cache_precision` | `int8` | Disk cache image precision. |
| `default_print_mode` | `full` | REPL `print` default. |
| `default_traversal_arg` | `n` | REPL `traversal` default flags. |
| `default_cache_clear_arg` | `md` | REPL `clear-cache` default target. |
| `default_timer_log_path` | `out/timer.yaml` | Default `compute tl` output path. |
| `default_ops_list_mode` | `all` | REPL `ops` default. |
| `default_compute_args` | empty | Space-separated default `compute` flags. |
| `scheduler_hp_type` | `cpu_work_stealing` | High-precision scheduler type: built-in `cpu_work_stealing`, `serial_debug`, `gpu_pipeline`, `heterogeneous`, or a loaded plugin scheduler name. |
| `scheduler_rt_type` | `cpu_work_stealing` | Kernel RT intent scheduler type using the same supported values; this does not enable CLI RT commands. |
| `scheduler_worker_count` | `0` | CPU worker count; `0` means auto. |

Use another config file with:

```bash
./build/bin/graph_cli --config path/to/config.yaml --repl
```

When the CLI loads a config file, it first copies `scheduler_hp_type`,
`scheduler_rt_type`, and `scheduler_worker_count` into
`Kernel::SchedulerConfig`, then scans configured plugin directories before any
graph load. Scheduler type strings are resolved when a graph creates its
scheduler instances during graph load, or later through `scheduler set <hp|rt>
<type>`, so newly loaded graphs can still use plugin-provided scheduler types
discovered during startup.

Scheduler plugin discovery and scheduler selection are separate phases.
`scheduler plugins` can show a plugin because it was scanned from
`scheduler_dirs`, while `scheduler get all` still shows the graph's configured
HP/RT schedulers. A scanned plugin is used only after the config names it via
`scheduler_hp_type` or `scheduler_rt_type`, or after an explicit
`scheduler set` command.

`cache_root_dir` is also applied before graph load. Relative values are resolved
from the current working directory, and the graph cache root becomes
`<cache_root_dir>/<graph_name>`.

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

## Built-In Operations

Built-in operations are registered in `src/lib/core/ops.cpp`.

| Type | Subtype | Notes |
| --- | --- | --- |
| `image_source` | `path` | Load an image from disk. |
| `image_generator` | `constant` | Create a constant image. |
| `image_generator` | `perlin_noise` | Generate CPU Perlin noise. |
| `analyzer` | `get_dimensions` | Return image width/height metadata. |
| `math` | `divide` | Divide two numeric operands. |
| `image_process` | `convolve` | Apply a kernel image. |
| `image_process` | `resize` | Resize an image. |
| `image_process` | `crop` | Crop a rectangular region. |
| `image_process` | `extract_channel` | Extract B/G/R/A or numeric channel. |
| `image_process` | `gaussian_blur` | Monolithic HP plus tiled HP/RT implementations. |
| `image_process` | `gaussian_blur_tiled` | Backward-compatible tiled alias. |
| `image_process` | `curve_transform` | Tiled curve transform. |
| `image_mixing` | `add_weighted` | Blend two images. |
| `image_mixing` | `diff` | Absolute difference between two images. |
| `image_mixing` | `multiply` | Pixel-wise multiplication. |

`plugins/ops/` currently builds example plugins for `image_process:invert`,
`image_process:threshold`, and `io:save`. On macOS, its private `metal/`
implementation also produces
`image_generator:perlin_noise_metal`, but it is only scanned when
`build/high_performance/metal` is manually present in `plugin_dirs`.

## Maintained Docs

- `manual.md`: user manual and REPL reference.
- `docs/kernel-architecture/Overview.md`: current architecture overview.
- `docs/kernel-architecture/Dirty-Region-Propagation.md`: ROI/dirty-region propagation notes.
- `docs/outdated/`: older milestone plans, reports, and architecture sketches kept for reference.
