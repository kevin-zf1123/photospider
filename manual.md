# Photospider User Manual

This manual describes the current command-line and REPL behavior of
Photospider. For architecture internals, see
`docs/kernel-architecture/Overview.md`.

## 1. Build And Setup

Install the required C++ toolchain, OpenCV, yaml-cpp, and test dependencies if
you plan to run the GoogleTest suite. Initialize submodules when using the
vendored FTXUI source:

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target graph_cli -j
```

The main executable is:

```text
build/bin/graph_cli
```

Plugin and runtime outputs:

| Output | Path |
| --- | --- |
| executable | `build/bin/graph_cli` |
| backend libraries | `build/lib` |
| operation plugins | `build/plugins` |
| scheduler plugins | `build/schedulers` |
| test binaries | `build/tests` |

## 2. Command-Line Mode

Show top-level options:

```bash
./build/bin/graph_cli --help
```

Supported options:

| Option | Argument | Description |
| --- | --- | --- |
| `-h`, `--help` | none | Print top-level CLI help. |
| `-r`, `--read` | `<file>` | Load YAML into the `default` session. |
| `-o`, `--output` | `<file>` | Save the current graph YAML. Requires `--read` first. |
| `-p`, `--print` | none | Print the loaded graph dependency tree. |
| `-t`, `--traversal` | none | Print dependency tree and post-order traversal. |
| `--clear-cache` | none | Clear cache for the current graph after loading one. |
| `--config` | `<file>` | Use a custom config YAML file. |
| `--repl` | none | Enter the interactive REPL after CLI actions. |

Examples:

```bash
./build/bin/graph_cli --read util/testcases/full_ops.yaml --print
./build/bin/graph_cli --read util/testcases/full_ops.yaml --traversal
./build/bin/graph_cli --config config.yaml --read graph.yaml --repl
```

Top-level CLI mode does not expose `--compute` or `--save`. Use the REPL
commands `compute` and `save` for execution and image output.

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
| `switch <name> [c]` | Switch active sessions. With `c`, copy the current session to the target first. |
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

The CLI and REPL compute surface is batch-oriented. It does not currently expose
realtime update interaction commands such as `compute rt` or `--dirty-roi`.
`RealTimeUpdate` is a kernel intent for a future GUI/interaction path, not a
committed CLI feature surface.

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

## 8. Scheduler Commands

Use `scheduler` to inspect or change per-graph schedulers:

```text
scheduler list
scheduler get [hp|rt|all]
scheduler set <hp|rt> <type>
scheduler scan [dir]
scheduler load <path>
scheduler plugins
```

Built-in scheduler types:

| Type | Description |
| --- | --- |
| `cpu_work_stealing` | Multi-threaded CPU scheduler with work stealing. |
| `serial_debug` | Single-threaded scheduler useful for debugging. |
| `gpu_pipeline` | Heterogeneous scheduler with GPU/CPU routing. |
| `heterogeneous` | Alias for `gpu_pipeline`. |

Example:

```text
scheduler get
scheduler set hp serial_debug
scheduler scan build/schedulers
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

The default config file is `config.yaml`. If it is missing, `graph_cli` creates
one with default values.

| Key | Default | Description |
| --- | --- | --- |
| `cache_root_dir` | `cache` | Shared disk cache root; each loaded graph uses `<cache_root_dir>/<graph_name>`. |
| `plugin_dirs` | `[build/plugins]` | Operation plugin search paths. |
| `scheduler_dirs` | `[build/schedulers]` | Scheduler plugin search paths. |
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
| `switch_after_load` | `true` | Make a loaded graph the active session. |
| `session_warning` | `true` | Warn before overwriting session content. |
| `scheduler_hp_type` | `cpu_work_stealing` | Default HP scheduler: built-in `cpu_work_stealing`, `serial_debug`, `gpu_pipeline`, `heterogeneous`, or a loaded plugin scheduler name. |
| `scheduler_rt_type` | `cpu_work_stealing` | Default kernel RT intent scheduler using the same supported values; this does not enable CLI RT commands. |
| `scheduler_worker_count` | `0` | CPU scheduler worker count; `0` means auto. |

When the CLI loads a config file, it scans `scheduler_dirs` before graph load
and then copies these scheduler defaults into `Kernel::SchedulerConfig`. Newly
loaded graphs inherit the configured HP/RT scheduler types and worker count,
including plugin-provided scheduler types discovered from configured
directories. Use `scheduler set <hp|rt> <type>` for an immediate per-graph
scheduler switch.

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

Built-ins are registered in `src/ops.cpp`.

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
| `image_process` | `gaussian_blur_tiled` | Tiled blur alias retained for older graphs. |
| `image_process` | `curve_transform` | Apply the curve transform kernel. |
| `image_mixing` | `add_weighted` | Weighted blend of two images. |
| `image_mixing` | `diff` | Absolute difference of two images. |
| `image_mixing` | `multiply` | Pixel-wise multiplication. |

Plugin examples from `custom_ops/` add `image_process:invert`,
`image_process:threshold`, and `io:save`. On macOS, Metal builds also produce
`image_generator:perlin_noise_metal`, but it is only scanned when
`build/high_performance/metal` is manually present in `plugin_dirs`.

## 12. Validation

Build the CLI:

```bash
cmake --build build --target graph_cli -j
```

Run registered CTest tests:

```bash
ctest --output-on-failure --test-dir build
```

Run a specific test executable:

```bash
cmake --build build --target test_scheduler -j
./build/tests/test_scheduler
```
