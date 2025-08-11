# Photospider: A Dynamic Image Processing Graph Engine

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

Photospider is a powerful, C++-based command-line tool for creating, managing, and executing complex image processing pipelines. Unlike simple, linear pipeline tools, Photospider is a true **dynamic data flow engine**.

This means the output of one node (e.g., a computed number, a string, or a dimension) can be used as an **input parameter** for another node. This enables the creation of intelligent, self-configuring graphs where processing steps can adapt based on the data flowing through the system.

## Core Concepts

The fundamental principle of Photospider is the **Node Graph**. A graph is a collection of nodes, where each node represents a specific operation (e.g., loading an image, applying a blur, mixing two images).

The power of Photospider comes from its dynamic connections:

* **Image Connections**: The standard flow of image data from one node to another.
* **Parameter Connections**: A node can output data (like an image's width). Another node can then consume this data to configure its own parameters (like the kernel size for a blur), all at runtime.

This allows you to create graphs where, for example, a `resize` node can dynamically get its target width from an `analyzer` node that measures another image, or a `blur` filter's intensity can be calculated based on the difference between two other nodes.

## Key Features

* **Dynamic Graph Execution**: Go beyond static pipelines with parameters that are computed at runtime.
* **YAML-Based Definitions**: Define complex graphs in a clean, human-readable format.
* **Extensible Operation System**: Easily add new C++ functions as nodes in the graph.
* **Intelligent Caching**: In-memory and on-disk caching for both images and metadata reduces re-computation.
* **Interactive REPL**: An interactive shell (`ps>`) to load, inspect, modify, and run graphs on the fly.
* **Cycle Detection**: Protects against invalid graph structures.

## Prerequisites

To build Photospider, you will need:

* A C++17 compliant compiler (e.g., GCC, Clang, MSVC)
* **CMake** (version 3.10 or higher)
* **OpenCV** (version 4.x recommended)
* **yaml-cpp** library

### Dependency Installation

**On Ubuntu/Debian:**

```bash
sudo apt-get update
sudo apt-get install build-essential cmake libopencv-dev libyaml-cpp-dev
```

**On macOS (using Homebrew):**

```bash
brew install cmake opencv yaml-cpp
```

## How to Build

1. **Clone the repository:**

   ```bash
   git clone <your-repo-url>
   cd photospider
   ```
2. **Create a build directory:**

   ```bash
   mkdir build
   cd build
   ```
3. **Run CMake to configure the project:**

   ```bash
   cmake ..
   ```
4. **Compile the project:**

   ```bash
   make
   ```

   The main executable `graph_cli` will be located in the `build/cli/` directory.

## How to Use

Photospider can be controlled via command-line arguments or through its interactive REPL shell.

### Command-Line Arguments

```bash
# Load a graph, compute all ending nodes, and then exit
./build/cli/graph_cli --read my_graph.yaml --compute all

# Load a graph and save the result of node 5 to a file
./build/cli/graph_cli --read my_graph.yaml --save 5 result.png
```

### Interactive Shell (REPL)

For more interactive work, start the REPL:

```bash
./build/cli/graph_cli --repl
```

This will give you a `ps>` prompt. Type `help` to see the full list of commands.

**Common REPL Commands:**

* `read <file>`: Load a graph from a YAML file.
* `print`: Display the detailed dependency tree of the current graph.
* `traversal`: Show the evaluation order of the graph.
* `compute <id|all>`: Execute a specific node or all terminal nodes.
* `save <id> <file>`: Compute a node and save its image output.
* `clear-cache [d|m]`: Clear the on-disk, in-memory, or both caches.
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
    path: assets/my_image.jpg
  caches:
    - cache_type: image
      location: source.png

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
    - from_node_id: 2
      from_output_name: width
      to_parameter_name: operand1
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
    - cache_type: image
      location: final_blur.png
```

## Available Built-in Operations

| Type                    | Subtype            | Description                                             | Key Parameters                                     |
| ----------------------- | ------------------ | ------------------------------------------------------- | -------------------------------------------------- |
| **image_source**  | `path`           | Loads an image from a file path.                        | `path`, `resize`                               |
| **image_process** | `gaussian_blur`  | Applies a Gaussian blur filter.                         | `ksize`, `sigmaX`                              |
|                         | `resize`         | Resizes an image to a specific width and height.        | `width`, `height`, `interpolation`           |
| **image_mixing**  | `add_weighted`   | Blends two images together linearly.                    | `alpha`, `beta`, `gamma`, `merge_strategy` |
|                         | `diff`           | Computes the absolute difference between two images.    | `merge_strategy`                                 |
| **analyzer**      | `get_dimensions` | Outputs the width and height of an input image as data. | (None)                                             |
| **math**          | `divide`         | Divides two numbers from its parameters.                | `operand1`, `operand2`                         |

## Roadmap

* Add a wider variety of image processing operations (e.g., Canny edge detection, thresholding, color space conversions).
* Implement a plugin system to allow users to load custom operations from shared libraries without recompiling.
* Develop a GUI for visual graph construction.

## License

This project is licensed under the MIT License. See the `LICENSE` file for details.
