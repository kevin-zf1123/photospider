# Photospider: A Dynamic Image Processing Graph Engine

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

Photospider is a powerful, C++17-based command-line tool for creating, managing, and executing complex image processing pipelines. Unlike simple, linear pipeline tools, Photospider is a true **dynamic data flow engine**.

This means the output of one node (e.g., a computed number, a string, or a dimension) can be used as an **input parameter** for another node. This enables the creation of intelligent, self-configuring graphs where processing steps can adapt based on the data flowing through the system.

## Key Features

*   **Dynamic Graph Execution**: Go beyond static pipelines with parameters that are computed at runtime.
*   **Multi-threaded Compute Engine**: A parallel scheduler to accelerate computations on multi-core CPUs.
*   **Interactive TUI Editor**: A terminal-based UI to visually inspect and edit your entire node graph, with live dependency trees and YAML editing.
*   **Advanced REPL/CLI**: An interactive shell (`ps>`) to load, inspect, modify, and run graphs on the fly, with powerful commands and scripting support.
*   **YAML-Based Definitions**: Define complex graphs in a clean, human-readable format.
*   **Extensible Plugin System**: Easily add new C++ functions as nodes by dropping shared libraries (`.so`/`.dll`) into a `plugins` folder, with no need to recompile the main application.
*   **Intelligent Caching**: In-memory and on-disk caching for both images and metadata reduces re-computation.
*   **Cycle Detection**: Protects against invalid graph structures.
*   **Performance Profiling**: Built-in tools to time node execution and cache performance.

## Interactive TUI Editor

Photospider now includes a powerful interactive TUI for editing and inspecting graphs directly in your terminal. Launch it from the REPL using the `node` command.

This interface allows you to:
*   Navigate the list of all nodes in your graph.
*   View and edit the full YAML definition for any node.
*   Instantly see the dependency tree for the entire graph and for the selected node.
*   Apply, discard, or open the node's configuration in an external editor (`$EDITOR`).

## Prerequisites

To build Photospider, you will need a C++17 compliant compiler and the following development libraries:
*   A C++ Compiler (e.g., `g++`, `clang++`)
*   `pkg-config`
*   **OpenCV** (version 4.x recommended)
*   **yaml-cpp**

### Dependency Installation

**On Ubuntu/Debian:**
```bash
sudo apt-get update
sudo apt-get install build-essential pkg-config libopencv-dev libyaml-cpp-dev
```

**On macOS (using Homebrew):**
```bash
brew install pkg-config opencv yaml-cpp
```

## How to Build

1.  **Clone the repository and its submodules:**
```bash
git clone --recurse-submodules https://github.com/kevin-zf1123/photospider.git
cd photospider
```

2.  **Configure the project with CMake:**
    This command tells CMake to look for the source (`-S .`) in the current directory and prepare the build files in the `build` directory (`-B build`).
```bash
cmake -S . -B build
```

3.  **Compile the project:**
    This command invokes the compiler to build the project from the generated files in the `build` directory.
```bash
cmake --build build
```
4. **Build specific target:**
    This command builds the target part of the program from the generated files in the `build` directory.
    
    
    
```bash
cmake --build build --target <your target>
```

The main executable `graph_cli` will be created at `build/graph_cli`, and the `build/plugins` directory will be ready for your custom operations.

## How to Use

Photospider can be controlled via command-line arguments or through its interactive REPL shell.

### Command-Line Arguments
```bash
# Load a graph, compute the final nodes, and exit
./build/graph_cli --read my_graph.yaml

# Load a graph and save the result of node 4 to a file
./build/graph_cli --read my_graph.yaml --compute 4 --save 4 result.png
```

### Interactive Shell (REPL)

For more interactive work, start the REPL. This is the default action if no other flags are provided.
```bash
./build/graph_cli
```
This will give you a `ps>` prompt. Type `help` to see the full list of commands. A more detailed guide is available in `manual.md`.

**Common REPL Commands:**
*   `read <file>`: Load a graph from a YAML file into the current session.
*   `load <name> <file>`: Load a graph into a named session.
*   `node <id>`: Open the interactive TUI editor for a specific node.
*   `print`: Display the detailed dependency tree of the current graph.
*   `ops`: List all available operations, including from plugins.
*   `compute <id|all> [force] [parallel] [t]`: Execute a node or all terminal nodes with optional flags.
*   `save <id> <file>`: Compute a node and save its image output.
*   `config`: Open the interactive configuration editor.
*   `exit`: Quit the shell.

## Available Built-in Operations

| Type | Subtype | Description | Key Parameters |
| :--- | :--- | :--- | :--- |
| **image\_source** | `path` | Loads an image from a file path. | `path` |
| **image\_generator**| `perlin_noise` | Generates Perlin noise. | `width`, `height`, `grid_size` |
| | `constant` | Creates a constant color image. | `width`, `height`, `value`, `channels`|
| **image\_process**| `gaussian_blur` | Applies a Gaussian blur filter. | `ksize`, `sigmaX` |
| | `resize` | Resizes an image to a specific width and height. | `width`, `height`, `interpolation` |
| | `crop` | Extracts a rectangular region from an image. | `mode`, `x`, `y`, `width`, `height` |
| | `extract_channel` | Isolates a single channel (B,G,R,A). | `channel` |
| | `convolve` | Applies a 2D convolution with a kernel image. | `padding`, `absolute` |
| | `curve_transform`| Applies `1 / (1 + k*I)`. | `k` |
| **image\_mixing** | `add_weighted` | Blends two images linearly. | `alpha`, `beta`, `gamma`, `merge_strategy` |
| | `diff` | Computes the absolute difference between two images. | `merge_strategy` |
| | `multiply` | Multiplies two images pixel-wise. | `scale`, `merge_strategy` |
| **analyzer** | `get_dimensions` | Outputs the width and height of an image as data. | (None) |
| **math** | `divide` | Divides two numbers from its parameters. | `operand1`, `operand2` |

## License

This project is licensed under the MIT License.

## Kernel Architecture Docs

- Architecture Overview: `docs/kernel-architecture/Overview.md`
- Development Roadmap: `docs/kernel-architecture/Roadmap.md`
- Dirty Region Propagation Spec: `docs/kernel-architecture/Dirty-Region-Propagation.md`
