
# Photospider User Manual & Documentation

## 1. Introduction

Photospider is a powerful, node-based command-line tool for orchestrating complex image processing pipelines. It allows you to define a graph of operations in a simple YAML file, where each node performs a specific task, such as loading an image, applying a filter, or blending two images together.

Its key features include:

* **Dynamic Graph Execution**: Nodes can pass not only images but also data (like width, height, or computed values) to downstream nodes, allowing for highly adaptive pipelines.
* **Interactive TUI Editor**: A terminal-based UI to visually inspect and edit your entire node graph, with live dependency trees and YAML editing.
* **Extensible Operations**: A plugin system allows for new C++ operations to be easily added by dropping shared libraries into a folder.
* **Intelligent Caching**: Results from nodes can be cached to both memory and disk, dramatically speeding up repeated computations.
* **Advanced REPL/CLI**: A powerful interactive mode for loading, inspecting, computing, and configuring graphs on the fly.
* **Performance Profiling**: Built-in timing tools to measure the performance of each step, including cache lookups and computations.

---

## 2. Getting Started

The application can be run in two primary modes: as a one-shot command-line tool or as an interactive shell (REPL).

### Command-Line Flags

| Flag                    | Argument   | Description                                                                                 |
| :---------------------- | :--------- | :------------------------------------------------------------------------------------------ |
| `-h`, `--help`      | -          | Displays this help message about the command-line flags.                                    |
| `-r`, `--read`      | `<file>` | Loads and processes the specified graph YAML file.                                          |
| `-o`, `--output`    | `<file>` | Saves the current graph state to a YAML file.                                               |
| `-p`, `--print`     | -          | Prints the dependency tree of the loaded graph.                                             |
| `-t`, `--traversal` | -          | Shows the evaluation order of the loaded graph.                                             |
| `--config`            | `<file>` | Specifies a custom configuration file to use instead of the default `config.yaml`.        |
| `--clear-cache`       | -          | Deletes the contents of the configured cache directory.                                     |
| `--repl`              | -          | Starts the interactive shell (REPL). This is the default if no other actions are specified. |

**Example: Executing a graph directly**

```bash
# Load a graph, compute it, and save the final node's output image
./build/graph_cli --read my_graph.yaml --compute 4 --save 4 final_image.png
```

**Example: Starting the REPL with a custom config**

```bash
./build/graph_cli --config alternate_settings.yaml --repl
```

---

## 3. The REPL (Interactive Shell)

The REPL is the most powerful way to use Photospider. Start it by running the application with the `--repl` flag or with no flags at all.

### REPL Commands

#### `help`

Displays a list of all available REPL commands.

#### `read <filepath>`

Clears the current graph and loads a new one from the specified YAML file.

* **Example**: `read graphs/complex_blend.yaml`

#### `source <filepath>`

Executes a series of REPL commands from a script file. Each line in the file is treated as a command. Lines starting with `#` are ignored.

* **Example**: `source my_script.txt`

#### `node [<id>]`

Opens the interactive **TUI Node Editor**.

* If `<id>` is provided, the editor opens focused on that specific node.
* If `<id>` is omitted, a selection menu appears, allowing you to choose which node to open.
* See the "TUI Node Editor" section for a full guide.
* **Example**: `node 4` or `node`

#### `print [<id>|all] [mode]`

Displays a hierarchical dependency tree.

* **Target**:
  * `<id>`: Shows the dependency tree required to compute the specified node.
  * `all`: Shows the trees for all "ending" nodes in the graph (default).
* **Mode**:
  * `d` or `detailed`: Shows static parameters for each node (default, configured in `config.yaml`).
  * `s` or `simplified`: Hides the static parameters for a cleaner view.
* **Example**: `print 4 s`

#### `traversal [flags]`

Shows the topological post-order (the actual evaluation order) for each branch of the graph. It can be combined with flags to show cache status and control tree display.

* **Tree Flags**:
  * `d` or `detailed`: Show the full dependency tree first.
  * `s` or `simplified`: Show the simplified dependency tree first.
  * `n` or `no_tree`: Do not show a dependency tree (default, configured in `config.yaml`).
* **Cache Flags**:
  * `m`: Show "in memory" status.
  * `d`: Show "on disk" status.
  * `c`: Synchronize in-memory results to the disk cache.
  * `cr`: Synchronize cache and remove any "orphaned" files on disk.
* **Example**: `traversal cr s` (Show simplified tree, then sync cache and show status)

#### `ops [mode]`

Lists all registered operations available for use in graphs.

* **Mode**:
  * `a` or `all`: Show all operations (default, configured in `config.yaml`).
  * `b` or `builtin`: Show only the operations compiled into the application.
  * `p` or `plugins`: Show only the operations loaded from external plugin libraries.

#### `output <filepath>`

Saves the structure of the currently loaded graph to a new YAML file.

* **Example**: `output my_current_graph.yaml`

#### `clear-graph`

Removes all nodes from the current in-memory graph.

#### `cc` or `clear-cache [flags]`

Clears the cache.

* **Flags**:
  * `d`: Clears the on-disk cache only.
  * `m`: Clears the in-memory cache only.
  * `md` or `dm`: Clears both (default).
* **Example**: `cc m`

#### `compute <id|all> [flags]`

The core command to execute nodes.

* **Target**:
  * `all`: Computes all final "ending" nodes in the graph.
  * `<id>`: Computes the node with the specified integer ID and all its dependencies.
* **Flags**:
  * `force`: Forces re-computation of a node and its dependencies, even if they are already cached.
  * `t`: Enables a simple console timer. After computation, it prints a summary of how long each step took.
  * `tl [path]`: Enables detailed timer logging to a structured YAML file. If `[path]` is omitted, it uses the default from `config.yaml`.
* **Examples**:
  * `compute 4`
  * `compute all t`
  * `compute 4 tl`
  * `compute all force tl logs/run1.yaml`

#### `config`

Opens the interactive **TUI Configuration Editor**, allowing you to view and modify application settings for the current session and save them permanently. See the "TUI Config Editor" section for details.

#### `save <id> <filepath>`

A convenient shortcut that computes a node and saves its output image to a file.

* **Example**: `save 4 final_output.png`

#### `free`

Frees the in-memory cache for any node that is not an "ending" node. This is useful for conserving memory in very large graphs after a computation is complete.

#### `exit`

Quits the interactive shell. It will prompt you to save any unsaved graph changes and to synchronize the disk cache.

---

## 4. The TUI Node Editor

The TUI Node Editor is a powerful terminal interface for inspecting and editing your graph. Launch it with the `node [<id>]` command.

### Layout

The editor is split into two main columns:

* **Left Column**:

  * **Nodes Pane**: A list of all nodes in the graph. Use `↑`/`↓` to select a node, which updates all other panes.
  * **Editor Pane**: A text editor showing the full YAML definition of the selected node. You can edit the node directly here.
* **Right Column**:

  * **Whole Graph Pane**: Shows the dependency tree for all final nodes.
  * **Context Pane**: Shows a dependency tree relevant to the selected node.

### Keybindings

* **Global**:
  * `<Tab>`: Cycle focus between the four main panes (Nodes, Editor, Whole Graph, Context). The active pane has a heavy border.
  * `<Esc>` or `<Ctrl-C>`: Exit the editor.
* **Editor Pane**:
  * `<Ctrl-S>`: **Apply** the changes from the YAML editor to the in-memory node.
  * `<Ctrl-D>`: **Discard** any changes and reload the node's original YAML.
  * `<Ctrl-E>`: **External Edit**. Opens the node's YAML in your system's default `$EDITOR`. Changes are loaded back when you close the editor.
* **Tree Panes**:
  * `↑`/`↓`: Scroll the view up and down.
  * `←`/`→`: Scroll the view left and right for long lines.

### Toggles

Above the tree panes are two toggles:

* **Context Toggle**: Switches the Context Pane between:
  * `From node`: Shows the dependency tree required to compute the selected node.
  * `Trees containing`: Shows all final output trees that depend on the selected node.
* **View Toggle**: Switches both tree panes between:
  * `Detailed`: Shows static parameters for each node.
  * `Simplified`: Hides parameters for a cleaner view.

---

## 5. The TUI Config Editor

The `config` command opens a TUI for managing settings.

### Keybindings

* **Navigate Mode** (Default):
  * `↑`/`↓`: Move selection.
  * `e`: Enter Edit Mode on the selected line.
  * `a`: Add a new item (e.g., for plugin directories).
  * `d`: Delete the selected item (e.g., a plugin directory).
  * `: `: Enter Command Mode.
  * `q`: Quit the editor.
* **Edit Mode**:
  * `<Enter>`: Accept the change and return to Navigate Mode.
  * `<Esc>`: Cancel the change and return.
* **Command Mode**:
  * Enter a command and press `<Enter>`.
  * **Commands**: `w` (write/save), `a` (apply to session), `q` (quit).

---

## 6. Configuration (`config.yaml`)

When you first run the REPL, a `config.yaml` file is automatically created. This file controls the default behavior of the interactive commands.

| Key                         | Description                                                                                              | Example Value       |
| :-------------------------- | :------------------------------------------------------------------------------------------------------- | :------------------ |
| `cache_root_dir`          | The directory for storing cache files. (Requires restart)                                                | `cache`           |
| `plugin_dirs`             | A list of directories to scan for plugin (`.so`/`.dll`) files.                                       | `[build/plugins]` |
| `default_traversal_arg`   | Default flags for the `traversal` command.                                                             | `n`               |
| `default_print_mode`      | Default mode for the `print` command (`detailed` or `simplified`).                                 | `detailed`        |
| `default_cache_clear_arg` | Default flags for the `cc` or `clear-cache` command.                                                 | `md`              |
| `default_exit_save_path`  | Default filename when saving a graph on exit.                                                            | `graph_out.yaml`  |
| `exit_prompt_sync`        | Default answer for syncing cache on exit (`true`/`false`).                                           | `true`            |
| `config_save_behavior`    | Default save action after using the TUI `config` editor (`current`, `default`, `ask`, `none`). | `ask`             |
| `editor_save_behavior`    | Behavior for saving changes from the `node` editor (`ask`, `auto_save_on_apply`, `manual`).      | `ask`             |
| `default_timer_log_path`  | Default file path for the `compute tl` command.                                                        | `out/timer.yaml`  |
| `default_ops_list_mode`   | Default mode for the `ops` command (`all`, `builtin`, `plugins`).                                | `all`             |
| `ops_plugin_path_mode`    | How to display plugin paths in `ops` (`name_only`, `relative_path`, `absolute_path`).            | `name_only`       |

---

## 7. The Graph YAML Format

A graph is a sequence of nodes defined in a YAML file. Each node is a map with the following keys:

| Key                  | Type     | Description                                                                                                   |
| :------------------- | :------- | :------------------------------------------------------------------------------------------------------------ |
| `id`               | integer  | A unique integer identifier for this node.                                                                    |
| `name`             | string   | A human-readable name for the node.                                                                           |
| `type`             | string   | The general category of the operation (e.g.,`image_source`, `image_process`).                             |
| `subtype`          | string   | The specific operation within the category (e.g.,`path`, `gaussian_blur`).                                |
| `image_inputs`     | sequence | A list of image inputs this node requires. Each item is a map with `from_node_id`.                          |
| `parameter_inputs` | sequence | A list of data inputs this node requires. Links an output from another node to one of this node's parameters. |
| `parameters`       | map      | Static parameters for the operation, defined directly in the file.                                            |
| `caches`           | sequence | A list of cache configurations. Typically used to specify an output filename for the disk cache.              |
| `outputs`          | sequence | A descriptive list of the outputs this node produces. (Currently for documentation only).                     |

---

## 8. Built-in Operations (Nodes)

### 8.1. Image Source

#### `image_source:path`

Loads an image from a local file path.

* **Inputs**: None
* **Outputs**: Image
* **Parameters**:
  * `path` (string, **required**): The file path to the image.
* **YAML Example**:
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

### 8.2. Image Processing

#### `image_process:gaussian_blur`

Applies a Gaussian blur filter.

* **Inputs**: 1 Image
* **Outputs**: Image
* **Parameters**:
  * `ksize` (integer, default: 3): The kernel size (must be positive and odd).
  * `sigmaX` (double, default: 0.0): The Gaussian kernel standard deviation.
* **YAML Example**:
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

* **Inputs**: 1 Image
* **Outputs**: Image
* **Parameters**:
  * `width` (integer, **required**): The target width.
  * `height` (integer, **required**): The target height.
  * `interpolation` (string, default: "linear"): Algorithm to use (`linear`, `cubic`, `nearest`, `area`).
* **YAML Example**:
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

* **Inputs**: 1 Image
* **Outputs**: Image
* **Parameters**:
  * `mode` (string, default: "value"): How to interpret coordinates (`value` for pixels, `ratio` for 0.0-1.0 scale).
  * `x`, `y`, `width`, `height` (**required**): Defines the crop rectangle.
* **YAML Example**:
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

Isolates a single channel (B, G, R, or A) from an image.

* **Inputs**: 1 Image
* **Outputs**: Image (single-channel), Data (`channel`: integer index)
* **Parameters**:
  * `channel` (string|integer, default: "a"): The channel to extract (`"b"`, `"g"`, `"r"`, `"a"` or `0`, `1`, `2`, `3`).
* **YAML Example**:
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

### 8.3. Image Mixing

#### `image_mixing:add_weighted`

Blends two images: `output = alpha*img1 + beta*img2 + gamma`. Supports simple and advanced channel-mapping modes.

* **Inputs**: 2 Images
* **Outputs**: Image
* **Parameters**:
  * `alpha`, `beta` (double, default: 0.5): Weights for the first and second image.
  * `gamma` (double, default: 0.0): Scalar added to the sum.
  * `merge_strategy` (string, default: "resize"): How to handle different sizes (`resize` or `crop`).
  * `channel_mapping` (map, optional): Enables advanced mode. See example below.
* **YAML Example (Advanced Channel Mapping)**:
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

* **Inputs**: 2 Images
* **Outputs**: Image
* **Parameters**:
  * `merge_strategy` (string, default: "resize"): `resize` or `crop`.
* **YAML Example**:
  ```yaml
  - id: 7
    name: "Difference"
    type: "image_mixing"
    subtype: "diff"
    image_inputs:
      - { from_node_id: 1 }
      - { from_node_id: 2 } # Compare original with blurred
  ```

### 8.4. Data & Utility

#### `analyzer:get_dimensions`

Extracts the width and height of an image as data outputs. Does not produce an image.

* **Inputs**: 1 Image
* **Outputs**: Data (`width`: integer, `height`: integer)
* **YAML Example**:
  ```yaml
  - id: 20
    name: "Get Image Size"
    type: "analyzer"
    subtype: "get_dimensions"
    image_inputs:
      - { from_node_id: 1 }
  ```

#### `math:divide`

Divides two numbers. Does not use images.

* **Inputs**: None
* **Outputs**: Data (`result`: double)
* **Parameters**:
  * `operand1` (double, **required**): The numerator.
  * `operand2` (double, **required**): The denominator.
* **YAML Example**:
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

## 9. Advanced Example: Dynamic Cropping

This example demonstrates the power of dynamic graphs by cropping an image to its center, using a width that is dynamically calculated to match a 16:9 aspect ratio.

```yaml
# File: dynamic_crop.yaml
- id: 10
  name: "Load Source"
  type: "image_source"
  subtype: "path"
  parameters: { path: "assets/wide_image.png" }

- id: 20
  name: "Get Source Dimensions"
  type: "analyzer"
  subtype: "get_dimensions"
  image_inputs:
    - { from_node_id: 10 }

- id: 30
  name: "Calculate 16:9 Width"
  type: "math"
  subtype: "divide"
  # Use the height from node 20 as the first operand
  parameter_inputs:
    - { from_node_id: 20, from_output_name: "height", to_parameter_name: "operand1" }
  # The second operand is the ratio 9/16 to get the width
  parameters:
    operand2: 0.5625 # (9 / 16)

- id: 40
  name: "Final Crop"
  type: "image_process"
  subtype: "crop"
  # The image comes from the source node
  image_inputs:
    - { from_node_id: 10 }
  # The width and height for the crop are taken dynamically from other nodes
  parameter_inputs:
    - { from_node_id: 30, from_output_name: "result", to_parameter_name: "width" }
    - { from_node_id: 20, from_output_name: "height", to_parameter_name: "height" }
  # We still need to provide static parameters for things that aren't dynamic
  parameters:
    mode: "value"
    x: 0
    y: 0
  caches:
    - { cache_type: image, location: "dynamic_crop_output.png" }
```e
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

* Develop a wider variety of built-in image processing operations (e.g., Canny edge detection, thresholding, color space conversions).
* Implement a GUI for visual graph construction and inspection.
* Enhance the plugin API with more capabilities.

## License

This project is licensed under the MIT License.
-----------------------------------------------
