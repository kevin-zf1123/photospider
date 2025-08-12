# Photospider: A Dynamic Image Processing Graph Engine

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

Photospider is a powerful, C++-based command-line tool for creating, managing, and executing complex image processing pipelines. Unlike simple, linear pipeline tools, Photospider is a true **dynamic data flow engine**.

This means the output of one node (e.g., a computed number, a string, or a dimension) can be used as an **input parameter** for another node. This enables the creation of intelligent, self-configuring graphs where processing steps can adapt based on the data flowing through the system.

## Key Features

* **Dynamic Graph Execution**: Go beyond static pipelines with parameters that are computed at runtime.
* **Interactive TUI Editor**: A terminal-based UI to visually inspect and edit your entire node graph, with live dependency trees and YAML editing.
* **Advanced REPL/CLI**: An interactive shell (`ps>`) to load, inspect, modify, and run graphs on the fly, with powerful commands and scripting support.
* **YAML-Based Definitions**: Define complex graphs in a clean, human-readable format.
* **Extensible Plugin System**: Easily add new C++ functions as nodes by dropping shared libraries (`.so`/`.dll`) into a `plugins` folder, with no need to recompile the main application.
* **Intelligent Caching**: In-memory and on-disk caching for both images and metadata reduces re-computation.
* **Cycle Detection**: Protects against invalid graph structures.
* **Performance Profiling**: Built-in tools to time node execution and cache performance.

## Interactive TUI Editor

Photospider now includes a powerful interactive TUI for editing and inspecting graphs directly in your terminal. Launch it from the REPL using the `node` command.

This interface allows you to:

* Navigate the list of all nodes in your graph.
* View and edit the full YAML definition for any node.
* Instantly see the dependency tree for the entire graph and for the selected node.
* Apply, discard, or open the node's configuration in an external editor (`$EDITOR`).

## Prerequisites

To build Photospider, you will need a C++17 compliant compiler and the following development libraries:

* A C++ Compiler (e.g., `g++`, `clang++`)
* `pkg-config`
* **OpenCV** (version 4.x recommended)
* **yaml-cpp**

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

1. **Clone the repository and its submodules:**

   ```bash
   git clone --recurse-submodules https://github.com/kevin-zf1123/photospider.git
   cd photospider
   ```
2. **Compile the project using the Makefile:**

   ```bash
   make
   ```

The main executable `graph_cli` will be created at `build/graph_cli`, and a `build/plugins` directory will be ready for your custom operations.

## How to Use

Photospider can be controlled via command-line arguments or through its interactive REPL shell.

### Command-Line Arguments

```bash
# Load a graph, compute the final nodes, and exit
./build/graph_cli --read my_graph.yaml

# Load a graph and save the result of node 4 to a file
./build/graph_cli --read my_graph.yaml --save 4 result.png
```

### Interactive Shell (REPL)

For more interactive work, start the REPL. This is the default action if no other flags are provided.

```bash
./build/graph_cli
```

This will give you a `ps>` prompt. Type `help` to see the full list of commands. A more detailed guide is available in `manual.md`.

**Common REPL Commands:**

* `read <file>`: Load a graph from a YAML file.
* `node <id>`: Open the interactive TUI editor for a specific node.
* `print`: Display the detailed dependency tree of the current graph.
* `ops`: List all available operations, including from plugins.
* `compute <id|all> [force] [t]`: Execute a node or all terminal nodes with optional flags.
* `save <id> <file>`: Compute a node and save its image output.
* `config`: Open the interactive configuration editor.
* `exit`: Quit the shell.

## The YAML Graph Format

A Photospider graph is a YAML sequence of nodes. Each node is a map with several key properties.

### Node Structure

```yaml
- id: <integer>                      # Unique integer ID for the node.
  name: <string>                     # A human-readable name.
  type: <string>                     # The general category of the operation (e.g., 'image_process').
  subtype: <string>                  # The specific operation (e.g., 'gaussian_blur').

  image_inputs:                      # List of nodes providing image inputs.
    - from_node_id: <integer>
      from_output_name: <string>     # Optional. The named output to connect to (defaults to 'image').

  parameter_inputs:                  # List of nodes providing parameter inputs.
    - from_node_id: <integer>
      from_output_name: <string>     # The data output from the source node (e.g., 'width').
      to_parameter_name: <string>    # The parameter to set on this node (e.g., 'ksize').

  parameters:                        # Static parameters for this node.
    key: value

  caches:                            # Optional. Defines how to cache the output.
    - cache_type: image
      location: relative/path/to/cache.png
```

### Example: A Dynamic Graph

This example loads an image, gets its width, calculates a blur radius based on that width, and then applies the blur.

```yaml
# 1. Load the source image
- id: 1
  name: source-image
  type: image_source
  subtype: path
  parameters:
    path: assets/a.jpg
  caches:
    - { cache_type: image, location: "source.png" }

# 2. Analyze the image to get its dimensions.
# This node outputs no image, but produces data named 'width' and 'height'.
- id: 2
  name: get-dims
  type: analyzer
  subtype: get_dimensions
  image_inputs:
    - from_node_id: 1

# 3. Calculate a blur kernel size.
# Takes the 'width' from node 2, divides it by 30, and outputs 'result'.
- id: 3
  name: calculate-blur-ksize
  type: math
  subtype: divide
  parameter_inputs:
    - { from_node_id: 2, from_output_name: width, to_parameter_name: operand1 }
  parameters:
    operand2: 30.0 # Static parameter

# 4. Apply the blur using the dynamically calculated kernel size.
- id: 4
  name: dynamic-blur
  type: image_process
  subtype: gaussian_blur
  image_inputs:
    - from_node_id: 1 # Image comes from the source
  parameter_inputs:
    - from_node_id: 3 # Parameter comes from the math node
      from_output_name: result
      to_parameter_name: ksize # The 'result' is wired to the 'ksize' parameter
  caches:
    - { cache_type: image, location: "final_blur.png" }
```

## Available Built-in Operations

| Type                     | Subtype             | Description                                                    | Key Parameters                                     |
| :----------------------- | :------------------ | :------------------------------------------------------------- | :------------------------------------------------- |
| **image\_source**  | `path`            | Loads an image from a file path.                               | `path`                                           |
| **image\_process** | `gaussian_blur`   | Applies a Gaussian blur filter.                                | `ksize`, `sigmaX`                              |
|                          | `resize`          | Resizes an image to a specific width and height.               | `width`, `height`, `interpolation`           |
|                          | `crop`            | Extracts a rectangular region from an image.                   | `mode`, `x`, `y`, `width`, `height`      |
|                          | `extract_channel` | Isolates a single channel (B,G,R,A) as a grayscale image.      | `channel`                                        |
| **image\_mixing**  | `add_weighted`    | Blends two images linearly. Supports advanced channel mapping. | `alpha`, `beta`, `gamma`, `merge_strategy` |
|                          | `diff`            | Computes the absolute difference between two images.           | `merge_strategy`                                 |
| **analyzer**       | `get_dimensions`  | Outputs the width and height of an input image as data.        | (None)                                             |
| **math**           | `divide`          | Divides two numbers from its parameters.                       | `operand1`, `operand2`                         |

## Roadmap

* Enhance the TUI editor with more interactive features (e.g., visual node connection).
* Develop a wider variety of built-in image processing operations (e.g., Canny edge detection, thresholding, color space conversions).
* Expand the plugin API with more capabilities and helper functions.

## License

This project is licensed under the MIT License.
-----------------------------------------------
