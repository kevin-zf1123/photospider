# Photospider

## What's this

Photospider is a C++17 image-processing graph runtime. It loads YAML graphs,
executes node dependencies, caches intermediate results, and exposes an
embedded Host API plus an interactive CLI, REPL, and TUI.

On macOS and Linux, it can also build a foreground, same-user local daemon and
a typed IPC client.

`graph_cli` always uses the embedded Host and does not connect to the daemon.
See the [architecture overview](docs/kernel-architecture/Overview.md).

## How to use

### Quick start

The default profile needs CMake 3.16+, a C++17 compiler, OpenCV (`core`,
`imgproc`, `imgcodecs`, and `videoio`), yaml-cpp, Threads, FTXUI, and
nlohmann/json when IPC is enabled. The commands below disable test targets for
a smaller user build.

On macOS:

```bash
brew install cmake pkg-config opencv yaml-cpp nlohmann-json
```

On Ubuntu or Debian:

```bash
sudo apt-get update
sudo apt-get install build-essential cmake pkg-config libopencv-dev \
  libyaml-cpp-dev nlohmann-json3-dev
```

Then initialize FTXUI, configure the project, build the CLI, and start the
REPL:

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
cmake --build build --target graph_cli -j
./build/bin/graph_cli
```

### Dependency-disabled Host build

The installed embedded Host can also be built without discovering OpenCV or
yaml-cpp:

```bash
cmake -S . -B build/minimal \
  -DBUILD_TESTING=OFF \
  -DPHOTOSPIDER_BUILD_IPC=OFF \
  -DPHOTOSPIDER_ENABLE_OPENCV=OFF \
  -DPHOTOSPIDER_ENABLE_YAML=OFF
cmake --build build/minimal --target photospider_kernel photospider -j
```

This profile keeps the real kernel aggregate and installable Host product. It
uses the standard-library image-buffer implementation and supports in-memory
and empty-session Host workflows. Image artifact IO and YAML graph/cache
persistence are explicit unavailable adapters that return `GraphErrc::Io`.
The OpenCV provider, OpenCV operation plugins, public OpenCV adapter, and
`graph_cli` default to `OFF` when their required capability is disabled.
Explicitly enabling one of those targets without its capability fails during
configuration with a targeted diagnostic.

### CLI and REPL

Use the top-level CLI to inspect options, load YAML, print a graph, or enter the
REPL:

```bash
./build/bin/graph_cli --help
./build/bin/graph_cli --read path/to/graph.yaml --print
./build/bin/graph_cli --read path/to/graph.yaml --repl
```

With no top-level action, `graph_cli` starts the REPL. Computation and image
saving are REPL commands, not top-level flags.

| Goal | REPL command |
| --- | --- |
| Load a graph | `read <file>` or `load <name> [yaml]` |
| Inspect a graph | `print all full`, `inspect <id>`, `inspect all`, or `inspect dirty` |
| Compute output | `compute <id> [flags]` or `compute all [flags]` |
| Save an image | `save <id> <file>` |
| Discover commands | `help` or `help <command>` |
| Leave the REPL | `exit` |

The complete command and configuration reference is in the
[user manual](manual.md). Command-specific help is also available through
`help <command>` inside the REPL.

### Local daemon and IPC

On macOS and Linux, `PHOTOSPIDER_BUILD_IPC` defaults to `ON`. Build and start
the foreground daemon with:

```bash
cmake --build build --target photospider_ipc_client photospiderd -j
./build/bin/photospiderd
```

`photospiderd` is a same-user local Unix-domain sidecar, not a system service,
remote endpoint, or TCP server. The CLI does not connect to it.

See the [IPC protocol](docs/codebase-structure/IPC-Protocol-v2.md) for its typed
client, socket policy, lifecycle, and limits.

### Install and integrate

Build the installable products, then choose an installation prefix:

```bash
cmake --build build -j
cmake --install build --prefix /desired/prefix
```

CMake installs the backend, public headers, enabled SDK components, capability
metadata, and `photospiderd` when IPC is enabled. It does not install
`graph_cli`; run the CLI from `build/bin/graph_cli`. An OpenCV-disabled install
does not install or advertise `operation_opencv`; its package config discovers
only the dependencies recorded as enabled by the producer.

| Use case | CMake component | Imported target |
| --- | --- | --- |
| Embedded backend | `embedded` | `Photospider::photospider` |
| Typed local IPC | `ipc_client` | `Photospider::photospider_ipc_client` |
| Operation plugin | `operation_sdk` | `Photospider::operation_sdk` |
| OpenCV operation adapter | `operation_opencv` | `Photospider::operation_opencv` |
| Policy plugin | `policy_sdk` | `Photospider::policy_sdk` |

For example, an embedded consumer can use:

```cmake
find_package(Photospider CONFIG REQUIRED COMPONENTS embedded)
target_link_libraries(app PRIVATE Photospider::photospider)
```

Operation and policy extension authors should use only their narrow SDK
component. The [plugin ABI guide](docs/kernel-architecture/Plugin-ABI.md)
defines the public contracts and required entry points.

### Documentation

| Need | Start here |
| --- | --- |
| CLI, REPL, configuration, and built-in operations | [User manual](manual.md) |
| Architecture reading order | [Kernel architecture index](docs/kernel-architecture/README.md) |
| Current modules and ownership | [Architecture overview](docs/kernel-architecture/Overview.md) |
| Local daemon and typed IPC | [IPC protocol](docs/codebase-structure/IPC-Protocol-v2.md) |
| Operation and policy extensions | [Plugin ABI](docs/kernel-architecture/Plugin-ABI.md) |
| Build and validation guidance | [Testing and validation](docs/development/Testing-and-Validation.md) |

English documentation is authoritative. Matching files under the relevant
`zh/` directories provide maintained, reader-oriented Chinese translations.

## Acknowledgement

The default Photospider profile builds on [OpenCV](https://opencv.org/),
[yaml-cpp](https://github.com/jbeder/yaml-cpp),
[FTXUI](https://github.com/ArthurSonzogni/FTXUI), and
[nlohmann/json](https://github.com/nlohmann/json). OpenCV and yaml-cpp are
build-time capabilities and may be disabled for the embedded Host product.

CURL and OpenSSL are optional, and GoogleTest supports the maintained test
suite.

The vendored FTXUI submodule retains its own
[MIT license](extern/ftxui/LICENSE).

## License

Photospider is licensed under the [MIT License](LICENSE).

Copyright (c) 2026 Zhu Feng.
