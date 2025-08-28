# Photospider: The Complete User Manual

## 1. Introduction

Welcome to Photospider, a powerful, node-based command-line tool for orchestrating complex image processing pipelines. It is designed from the ground up as a **dynamic data flow engine**, setting it apart from simple, linear processing tools.

In Photospider, you define a graph of operations in a simple YAML file. Each "node" in the graph performs a specific task, such as loading an image, applying a filter, or analyzing image properties. The true power of Photospider lies in its dynamic nature: nodes can pass not only images but also data (like width, height, or computed values) to downstream nodes. This allows for the creation of intelligent, self-configuring, and highly adaptive pipelines.

### Key Features

*   **Dynamic Graph Execution**: Go beyond static pipelines. The output of one node (e.g., a computed number) can be used as an input parameter for another, enabling graphs that adapt to the data flowing through them.
*   **Interactive TUI Editor**: A terminal-based user interface to visually inspect and edit your entire node graph in real-time, complete with live dependency trees and direct YAML editing.
*   **Extensible Operations**: A robust plugin system allows you to easily add new C++ operations by simply dropping shared libraries (`.so` or `.dll`) into a designated folder, with no need to recompile the main application.
*   **Intelligent Caching**: Results from nodes can be cached to both memory and disk, dramatically speeding up repeated computations and development iterations.
*   **Advanced REPL/CLI**: A powerful interactive shell (the REPL) provides commands for loading, inspecting, computing, and configuring graphs on the fly.
*   **Performance Profiling**: Built-in timing tools allow you to measure the performance of each node, helping you identify and optimize bottlenecks in your pipeline.
*   **Cycle Detection**: Protects against invalid graph structures to ensure predictable execution.

---

## 2. Getting Started

### Prerequisites

To build Photospider, you will need a C++17 compliant compiler and the following development libraries:

*   A C++ Compiler (e.g., `g++`, `clang++`)
*   `pkg-config`
*   **OpenCV** (version 4.x is recommended)
*   **yaml-cpp**

#### Dependency Installation

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
2.  **Create a build directory and configure the project with CMake:**
    This step generates the necessary build files (like Makefiles) inside the `build` directory.

    ```bash
    mkdir build
    cd build
    cmake ..
    ```

3.  **Compile the project:**
    Now, run `make` from inside the `build` directory to compile the source code.

    ```bash
    make
    ```

The main executable `graph_cli` will be created inside the `build` directory (`./build/graph_cli`), and the plugins directory will be at `./build/plugins`.

---

## 3. Execution Modes

The application can be run in two primary modes: as a one-shot command-line tool or as an interactive shell (REPL).

### 3.1. Command-Line Flags

For automated tasks, you can control Photospider directly from your shell using command-line arguments.

| Flag | Argument | Description |
| :--- | :--- | :--- |
| `-h`, `--help` | - | Displays a help message about the command-line flags. |
| `-r`, `--read` | `<file>` | Loads and processes the specified graph YAML file. |
| `-o`, `--output` | `<file>` | Saves the current graph state to a YAML file. |
| `-p`, `--print` | - | Prints the dependency tree of the loaded graph. |
| `-t`, `--traversal`| - | Shows the evaluation order of the loaded graph. |
| `--config` | `<file>` | Specifies a custom configuration file to use instead of the default `config.yaml`. |
| `--clear-cache` | - | Deletes the contents of the configured cache directory before execution. |
| `--repl` | - | Starts the interactive shell (REPL). This is the default if no other actions are specified. |

**Example: Executing a graph directly**
```bash
# Load a graph, compute node 4, and save its output image
./build/graph_cli --read my_graph.yaml --compute 4 --save 4 final_image.png
```

**Example: Starting the REPL with a custom configuration**```bash
./build/graph_cli --config alternate_settings.yaml --repl
```

### 3.2. The REPL (Interactive Shell)

The REPL is the most powerful and flexible way to use Photospider. It is ideal for developing, debugging, and experimenting with graphs. Start it by running the application with the `--repl` flag or with no flags at all.

```bash
./build/graph_cli
```

This will present you with the `ps>` prompt.

---

## 4. REPL Commands

The following commands are available within the interactive shell.

#### `help`
Displays a list of all available REPL commands.

#### `read <filepath>`
Clears the current graph from memory and loads a new one from the specified YAML file.
*   **Example**: `read graphs/complex_blend.yaml`

#### `source <filepath>`
Executes a series of REPL commands from a script file. Each line in the file is treated as a command. Lines starting with `#` are ignored as comments.
*   **Example**: `source my_script.txt`

#### `node [<id>]`
Opens the interactive **TUI Node Editor**.
*   If `<id>` is provided, the editor opens focused on that specific node.
*   If `<id>` is omitted, a selection menu appears, allowing you to choose which node to open.
*   See the "TUI Node Editor" section for a full guide.
*   **Examples**: `node 4` or `node`

#### `print [<id>|all] [mode]`
Displays a hierarchical dependency tree, showing how nodes are connected.
*   **Target**:
    *   `<id>`: Shows the dependency tree required to compute the specified node.
    *   `all`: Shows the trees for all "ending" nodes in the graph (default).
*   **Mode**:
    *   `d` or `detailed`: Shows static parameters for each node (default behavior can be set in `config.yaml`).
    *   `s` or `simplified`: Hides the static parameters for a cleaner view.
*   **Example**: `print 4 s`

#### `traversal [flags]`
Shows the topological post-order (the actual evaluation order) for each branch of the graph. It can be combined with flags to show cache status and control tree display.
*   **Tree Flags**:
    *   `d` or `detailed`: Show the full dependency tree first.
    *   `s` or `simplified`: Show the simplified dependency tree first.
    *   `n` or `no_tree`: Do not show a dependency tree (default).
*   **Cache Flags**:
    *   `m`: Show "in memory" status for each node.
    *   `d`: Show "on disk" status for each node.
    *   `c`: Synchronize (save) in-memory results to the disk cache.
    *   `cr`: Synchronize cache and remove any "orphaned" files on disk that don't correspond to a cached node.
*   **Example**: `traversal cr s` (Show simplified tree, then sync cache and show status)

#### `ops [mode]`
Lists all registered operations available for use in graphs.
*   **Mode**:
    *   `a` or `all`: Show all operations (default).
    *   `b` or `builtin`: Show only the operations compiled into the application.
    *   `p` or `plugins`: Show only the operations loaded from external plugin libraries.

#### `output <filepath>`
Saves the structure of the currently loaded graph (including any changes made in the TUI editor) to a new YAML file.
*   **Example**: `output my_current_graph.yaml`

#### `clear-graph`
Removes all nodes from the current in-memory graph.

#### `cc` or `clear-cache [flags]`
Clears the computation cache.
*   **Flags**:
    *   `d`: Clears the on-disk cache only.
    *   `m`: Clears the in-memory cache only.
    *   `md` or `dm`: Clears both (default).
*   **Example**: `cc m`

#### `compute <id|all> [flags]`
The core command to execute nodes and their dependencies.
*   **Target**:
    *   `all`: Computes all final "ending" nodes in the graph.
    *   `<id>`: Computes the node with the specified integer ID.
*   **Flags**:
    *   `force`: Forces re-computation of a node and its entire dependency chain, even if valid caches exist.
    *   `t`: Enables a simple console timer. After computation, it prints a summary of how long each step took.
    *   `tl [path]`: Enables detailed timer logging to a structured YAML file. If `[path]` is omitted, it uses the default path from `config.yaml`.
*   **Examples**:
    *   `compute 4`
    *   `compute all t`
    *   `compute 4 force tl logs/run1.yaml`

#### `config`
Opens the interactive **TUI Configuration Editor**, allowing you to view and modify application settings for the current session and save them permanently.

#### `save <id> <filepath>`
A convenient shortcut that first computes a node (if necessary) and then saves its image output to a file.
*   **Example**: `save 4 final_output.png`

#### `free`
Frees the in-memory cache for any node that is not an "ending" node. This is useful for conserving memory in very large graphs after a full computation.

#### `exit`
Quits the interactive shell. It will prompt you to save any unsaved graph changes and to synchronize the disk cache.

---

## 5. The TUI Node Editor

The TUI Node Editor is a powerful terminal interface for inspecting and editing your graph. Launch it with the `node [<id>]` command from the REPL.

### Layout

The editor is split into two main columns:

*   **Left Column**:
    *   **Nodes Pane**: A list of all nodes in the graph. Use `↑`/`↓` to select a node, which updates all other panes.
    *   **Editor Pane**: A text editor showing the full YAML definition of the selected node. You can edit the node directly here.
*   **Right Column**:
    *   **Whole Graph Pane**: Shows the dependency tree for all final nodes.
    *   **Context Pane**: Shows a dependency tree relevant to the currently selected node.

### Keybindings

*   **Global**:
    *   `<Tab>`: Cycle focus between the four main panes (Nodes, Editor, Whole Graph, Context). The active pane is highlighted with a heavy border.
    *   `<Esc>` or `<Ctrl-C>`: Exit the editor and return to the REPL.
*   **Editor Pane**:
    *   `<Ctrl-S>`: **Apply** the changes from the YAML editor to the in-memory node.
    *   `<Ctrl-D>`: **Discard** any changes and revert the editor to the node's original state.
    *   `<Ctrl-E>`: **External Edit**. Opens the node's YAML in your system's default `$EDITOR`. Changes are loaded back when you close the external editor.
*   **Tree Panes**:
    *   `↑`/`↓`: Scroll the view up and down.
    *   `←`/`→`: Scroll the view left and right for long lines.

### Toggles

Above the tree panes are two interactive toggles:

*   **Context Toggle**: Switches the Context Pane's view between:
    *   `From node`: Shows the dependency tree required to compute the selected node (i.e., its inputs).
    *   `Trees containing`: Shows all final output trees that depend on the selected node (i.e., its outputs).
*   **View Toggle**: Switches both tree panes between:
    *   `Detailed`: Shows static parameters for each node.
    *   `Simplified`: Hides parameters for a cleaner, high-level view.

---

## 6. The TUI Config Editor

The `config` command opens a TUI for managing application settings.

### Keybindings

*   **Navigate Mode** (Default):
    *   `↑`/`↓`: Move selection up and down.
    *   `e`: Enter **Edit Mode** on the selected line.
    *   `a`: Add a new item (used for list items like plugin directories).
    *   `d`: Delete the selected item.
    *   `: `: Enter **Command Mode**.
    *   `q`: Quit the editor.
*   **Edit Mode**:
    *   `<Enter>`: Accept the change and return to Navigate Mode.
    *   `<Esc>`: Cancel the change and return.
*   **Command Mode**:
    *   Enter a command and press `<Enter>`.
    *   **Commands**: `w` (write/save), `a` (apply to session), `q` (quit).

---

## 7. Configuration (`config.yaml`)

When you first run the REPL, a `config.yaml` file is automatically created if one does not exist. This file controls the default behavior of the application and its interactive commands.

| Key | Description | Example Value |
| :--- | :--- | :--- |
| `cache_root_dir` | The directory for storing on-disk cache files. (Requires restart to take effect). | `cache` |
| `plugin_dirs` | A list of directories to scan for plugin (`.so`/`.dll`) files. | `[build/plugins]` |
| `default_traversal_arg` | Default flags for the `traversal` command. | `n` |
| `default_print_mode` | Default mode for the `print` command (`detailed` or `simplified`). | `detailed` |
| `default_cache_clear_arg`| Default flags for the `cc` or `clear-cache` command. | `md` |
| `default_exit_save_path` | Default filename when saving an unsaved graph on exit. | `graph_out.yaml` |
| `exit_prompt_sync` | Default answer for syncing cache on exit (`true`/`false`). | `true` |
| `config_save_behavior` | Default save action after using the TUI `config` editor (`current`, `default`, `ask`, `none`). | `ask` |
| `editor_save_behavior` | Behavior for saving changes from the `node` editor (`ask`, `auto_save_on_apply`, `manual`). | `ask` |
| `default_timer_log_path` | Default file path for the `compute tl` command. | `out/timer.yaml` |
| `default_ops_list_mode` | Default mode for the `ops` command (`all`, `builtin`, `plugins`). | `all` |
| `ops_plugin_path_mode` | How to display plugin paths in `ops` (`name_only`, `relative_path`, `absolute_path`). | `name_only` |

---

## 8. The Graph YAML Format

A graph is defined as a sequence of nodes in a YAML file. Each node is a map with the following keys:

| Key | Type | Required | Description |
| :--- | :--- | :--- | :--- |
| `id` | integer | Yes | A unique integer identifier for this node. |
| `name` | string | Yes | A human-readable name for the node. |
| `type` | string | Yes | The general category of the operation (e.g.,`image_source`, `image_process`). |
| `subtype` | string | Yes | The specific operation within the category (e.g.,`path`, `gaussian_blur`). |
| `image_inputs` | sequence | No | A list of image inputs this node requires. Each item is a map with `from_node_id`. |
| `parameter_inputs`| sequence | No | A list of data inputs this node requires. Links an output from another node to one of this node's parameters. |
| `parameters` | map | No | Static parameters for the operation, defined directly in the file. |
| `caches` | sequence | No | A list of cache configurations. Typically used to specify an output filename for the disk cache. |
| `outputs` | sequence | No | A descriptive list of the outputs this node produces (for documentation purposes). |

---

## 9. Built-in Operations

### 9.1. Image Source

#### `image_source:path`
Loads an image from a local file path.
*   **Inputs**: None
*   **Outputs**: Image
*   **Parameters**:
    *   `path` (string, **required**): The file path to the image.
*   **YAML Example**:
    ```yaml
    - id: 1
      name: "Load Source Image"
      type: "image_source"
      subtype: "path"
      parameters:
        path: "assets/inputA.png"
      caches:
        - { cache_type: image, location: "source.png" }
    ```

### 9.2. Image Processing

#### `image_process:gaussian_blur`
Applies a Gaussian blur filter.
*   **Inputs**: 1 Image
*   **Outputs**: Image
*   **Parameters**:
    *   `ksize` (integer, default: 3): The kernel size (must be positive and odd).
    *   `sigmaX` (double, default: 0.0): The Gaussian kernel standard deviation.
*   **YAML Example**:
    ```yaml
    - id: 2
      name: "Blur Image"
      type: "image_process"
      subtype: "gaussian_blur"
      image_inputs:
        - { from_node_id: 1 }
      parameters:
        ksize: 21
    ```

#### `image_process:resize`
Changes the dimensions of an image.
*   **Inputs**: 1 Image
*   **Outputs**: Image
*   **Parameters**:
    *   `width` (integer, **required**): The target width.
    *   `height` (integer, **required**): The target height.
    *   `interpolation` (string, default: "linear"): Algorithm to use (`linear`, `cubic`, `nearest`, `area`).
*   **YAML Example**:
    ```yaml
    - id: 3
      name: "Resize to 512x512"
      type: "image_process"
      subtype: "resize"
      image_inputs:
        - { from_node_id: 1 }
      parameters:
        width: 512
        height: 512
    ```

#### `image_process:crop`
Extracts a rectangular region from an image.
*   **Inputs**: 1 Image
*   **Outputs**: Image
*   **Parameters**:
    *   `mode` (string, default: "value"): How to interpret coordinates (`value` for pixels, `ratio` for a 0.0-1.0 scale).
    *   `x`, `y`, `width`, `height` (**required**): Defines the crop rectangle.
*   **YAML Example**:
    ```yaml
    - id: 4
      name: "Crop Center"
      type: "image_process"
      subtype: "crop"
      image_inputs:
        - { from_node_id: 1 }
      parameters:
        mode: "ratio"
        x: 0.25
        y: 0.25
        width: 0.5
        height: 0.5
    ```

#### `image_process:extract_channel`
Isolates a single channel (B, G, R, or A) from an image and outputs it as a single-channel grayscale image.
*   **Inputs**: 1 Image
*   **Outputs**: Image (single-channel), Data (`channel`: integer index)
*   **Parameters**:
    *   `channel` (string | integer, default: "a"): The channel to extract (`"b"`, `"g"`, `"r"`, `"a"` or `0`, `1`, `2`, `3`).
*   **YAML Example**:
    ```yaml
    - id: 5
      name: "Get Alpha Channel"
      type: "image_process"
      subtype: "extract_channel"
      image_inputs:
        - { from_node_id: 1 }
      parameters:
        channel: "a" # or 3
    ```

### 9.3. Image Mixing

#### `image_mixing:add_weighted`
Blends two images using the formula: `output = alpha*img1 + beta*img2 + gamma`. Supports simple blending and advanced channel-mapping modes.
*   **Inputs**: 2 Images
*   **Outputs**: Image
*   **Parameters**:
    *   `alpha`, `beta` (double, default: 0.5): Weights for the first and second image.
    *   `gamma` (double, default: 0.0): Scalar added to the sum.
    *   `merge_strategy` (string, default: "resize"): How to handle images of different sizes (`resize` or `crop`).
    *   `channel_mapping` (map, optional): Enables advanced mode. See example below.
*   **YAML Example (Advanced Channel Mapping)**:
    ```yaml
    # This example takes the red channel from image 1 and puts it into the
    # blue and red channels of the output. It also takes the blue channel
    # from image 2 and adds it to the green channel of the output.
    - id: 6
      name: "Complex Blend"
      type: "image_mixing"
      subtype: "add_weighted"
      image_inputs:
        - { from_node_id: 1 } # input0
        - { from_node_id: 2 } # input1
      parameters:
        alpha: 1.0
        beta: 1.0
        merge_strategy: "crop"
        channel_mapping:
          input0:
            2: [0, 2]   # Map source Red(2) to destination Blue(0) and Red(2)
          input1:
            0: 1        # Map source Blue(0) to destination Green(1)
    ```

#### `image_mixing:diff`
Computes the absolute difference between two images.
*   **Inputs**: 2 Images
*   **Outputs**: Image
*   **Parameters**:
    *   `merge_strategy` (string, default: "resize"): How to handle images of different sizes (`resize` or `crop`).
*   **YAML Example**:
    ```yaml
    - id: 7
      name: "Difference"
      type: "image_mixing"
      subtype: "diff"
      image_inputs:
        - { from_node_id: 1 }
        - { from_node_id: 2 } # Compare original with blurred
    ```

### 9.4. Data & Utility

#### `analyzer:get_dimensions`
Extracts the width and height of an image as data outputs. This node does not produce an image.
*   **Inputs**: 1 Image
*   **Outputs**: Data (`width`: integer, `height`: integer)
*   **YAML Example**:
    ```yaml
    - id: 20
      name: "Get Image Size"
      type: "analyzer"
      subtype: "get_dimensions"
      image_inputs:
        - { from_node_id: 1 }
    ```

#### `math:divide`
Divides two numbers. This node does not use images.
*   **Inputs**: None (parameters can be static or dynamic)
*   **Outputs**: Data (`result`: double)
*   **Parameters**:
    *   `operand1` (double, **required**): The numerator.
    *   `operand2` (double, **required**): The denominator.
*   **YAML Example**:
    ```yaml
    - id: 21
      name: "Calculate Half Height"
      type: "math"
      subtype: "divide"
      parameter_inputs:
        - { from_node_id: 20, from_output_name: "height", to_parameter_name: "operand1" }
      parameters:
        operand2: 2.0
    ```

---

## 10. Advanced Example: Dynamic Cropping

This example demonstrates the power of dynamic graphs by cropping an image to its center, using a width that is dynamically calculated to match a 16:9 aspect ratio based on the input image's height.

```yaml
# File: dynamic_crop.yaml

# 1. Load the source image.
- id: 10
  name: "Load Source"
  type: "image_source"
  subtype: "path"
  parameters: { path: "assets/wide_image.png" }

# 2. Analyze the source image to get its dimensions. This node outputs
#    'width' and 'height' as data, but no image.
- id: 20
  name: "Get Source Dimensions"
  type: "analyzer"
  subtype: "get_dimensions"
  image_inputs:
    - { from_node_id: 10 }

# 3. Calculate the new width needed for a 16:9 aspect ratio.
#    It takes the 'height' data from node 20 as its first operand.
- id: 30
  name: "Calculate 16:9 Width"
  type: "math"
  subtype: "divide"
  parameter_inputs:
    - { from_node_id: 20, from_output_name: "height", to_parameter_name: "operand1" }
  # The second operand is the ratio 9/16 to get the width (height / (9/16) = height * 16/9)
  # Let's assume we want height / width = 9/16, so width = height * 16/9
  parameters:
    operand2: 0.5625 # (9 / 16)

# 4. Crop the original image. The 'width' and 'height' for the crop are taken
#    dynamically from the outputs of nodes 20 and 30.
- id: 40
  name: "Final Crop"
  type: "image_process"
  subtype: "crop"
  image_inputs:
    - { from_node_id: 10 }
  parameter_inputs:
    - { from_node_id: 30, from_output_name: "result", to_parameter_name: "width" }
    - { from_node_id: 20, from_output_name: "height", to_parameter_name: "height" }
  # We still provide static parameters for things that aren't dynamic,
  # like the crop starting position.
  parameters:
    mode: "value"
    x: 0
    y: 0
  caches:
    - { cache_type: image, location: "dynamic_crop_output.png" }
```

In this example, if you replace `assets/wide_image.png` with an image of a different height, the `Final Crop` (node 40) will automatically adjust its crop width to maintain the 16:9 aspect ratio, without you needing to change any other part of the graph. This is the core benefit of Photospider's dynamic engine.