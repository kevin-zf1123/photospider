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
| `cli/` | `graph_cli` executable entry point. |
| `src/cli/` | REPL commands, command help, TUI editors, autocomplete. |
| `src/kernel/` | Kernel facade, graph runtime, schedulers, services. |
| `src/adapter/` | Buffer adapters for OpenCV and Metal. |
| `src/benchmark/` | Benchmark configuration and execution support. |
| `src/metal/` | Apple Metal-backed operation code. |
| `include/` | Public headers mirroring the runtime and CLI modules. |
| `custom_ops/` | Shared operation plugins built into `build/plugins`. |
| `tests/` | GoogleTest sources. |
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
```

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

Note: the current `CMakeLists.txt` builds every `tests/test_*.cpp` executable,
but only selected targets are registered with `gtest_discover_tests`.

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

The command-line parser currently supports graph loading, YAML output, tree
printing, traversal display, cache clearing, config selection, and REPL entry.
Graph computation and image saving are REPL commands, not top-level CLI flags.

## REPL Commands

Type `help` in the REPL for the full command list, or `help <command>` for the
text stored in `src/cli/command/help/`.

Common commands:

| Command | Purpose |
| --- | --- |
| `read <file>` | Load YAML into the current session. |
| `load <name> [yaml]` | Load or create a named session. |
| `switch <name> [c]` | Switch sessions; `c` copies current content first. |
| `graphs` | List loaded graph sessions. |
| `node [id]` | Open the FTXUI node editor. |
| `print [all|id] [full|simplified] [inspect|i]` | Print dependency trees and optional cache metadata. |
| `traversal [f|s|n] [m|d|md] [c|cr]` | Show post-order evaluation and cache state. |
| `ops [all|builtin|plugins]` | List registered operations. |
| `compute <id|all> [flags]` | Compute one node or all ending nodes. |
| `save <id> <file>` | Compute a node and save its image output. |
| `scheduler ...` | Inspect or change HP/RT scheduler configuration. |
| `benchmark <dir>` | Edit a benchmark suite. |
| `bench <benchmark_dir> <output_dir>` | Run benchmark sessions. |

Supported `compute` flags include `force`, `force-deep`, `parallel`,
`t`/`timer`, `tl <path>`, `m`/`mute`, and `nosave`/`ns`.

## Configuration

By default, `graph_cli` reads `config.yaml` from the working directory. If it is
missing, the app creates one with defaults.

Important keys:

| Key | Default | Purpose |
| --- | --- | --- |
| `cache_root_dir` | `cache` | Disk cache root. |
| `plugin_dirs` | `[build/plugins]` | Operation plugin search paths. |
| `scheduler_dirs` | `[build/schedulers]` | Scheduler plugin search paths. |
| `cache_precision` | `int8` | Disk cache image precision. |
| `default_print_mode` | `full` | REPL `print` default. |
| `default_traversal_arg` | `n` | REPL `traversal` default flags. |
| `default_cache_clear_arg` | `md` | REPL `clear-cache` default target. |
| `default_timer_log_path` | `out/timer.yaml` | Default `compute tl` output path. |
| `default_ops_list_mode` | `all` | REPL `ops` default. |
| `default_compute_args` | empty | Space-separated default `compute` flags. |
| `scheduler_hp_type` | `cpu_work_stealing` | High-precision scheduler type. |
| `scheduler_rt_type` | `cpu_work_stealing` | Real-time scheduler type. |
| `scheduler_worker_count` | `0` | CPU worker count; `0` means auto. |

Use another config file with:

```bash
./build/bin/graph_cli --config path/to/config.yaml --repl
```

## Built-In Operations

Built-in operations are registered in `src/ops.cpp`.

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

`custom_ops/` currently builds example plugins for `image_process:invert`,
`image_process:threshold`, `io:save`, and on macOS `image_generator:perlin_noise_metal`.

## Maintained Docs

- `manual.md`: user manual and REPL reference.
- `docs/kernel-architecture/Overview.md`: current architecture overview.
- `docs/kernel-architecture/Dirty-Region-Propagation.md`: ROI/dirty-region propagation notes.
- `docs/outdated/`: older milestone plans, reports, and architecture sketches kept for reference.
