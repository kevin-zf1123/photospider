# Photospider: The Complete User Manual

## 1. Introduction

Welcome to Photospider, a powerful, node-based command-line tool for orchestrating complex image processing pipelines. It is designed from the ground up as a **dynamic data flow engine**, setting it apart from simple, linear processing tools.

In Photospider, you define a graph of operations in a simple YAML file. Each "node" in the graph performs a specific task, such as loading an image, applying a filter, or analyzing image properties. The true power of Photospider lies in its dynamic nature: nodes can pass not only images but also data (like width, height, or computed values) to downstream nodes. This allows for the creation of intelligent, self-configuring, and highly adaptive pipelines.

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

### How to Build

1.  **Clone the repository and its submodules:**
    ```bash
    git clone --recurse-submodules https://github.com/kevin-zf1123/photospider.git
    cd photospider
    ```

2.  **Configure the project with CMake:**
    This command prepares the build files in the `build` directory without leaving the project root.
    ```bash
    cmake -S . -B build
    ```

3.  **Compile the project:**
    This command runs the build process from the configured directory.
    ```bash
    cmake --build build
    ```

The main executable `graph_cli` will be created at `build/graph_cli`, and the plugins directory will be at `build/plugins`.

---

## 3. Execution Modes

### 3.1. Command-Line Flags

| Flag | Argument | Description |
| :--- | :--- | :--- |
| `-h`, `--help` | - | Displays a help message about the command-line flags. |
| `-r`, `--read` | `<file>` | Loads and processes the specified graph YAML file into a 'default' session. |
| `-o`, `--output` | `<file>` | Saves the current graph state to a YAML file. |
| `-p`, `--print` | - | Prints the dependency tree of the loaded graph. |
| `-t`, `--traversal`| - | Shows the evaluation order of the loaded graph. |
| `--config` | `<file>` | Specifies a custom configuration file. |
| `--clear-cache` | - | Deletes the contents of the configured cache directory before execution. |
| `--repl` | - | Starts the interactive shell (REPL). This is the default if no other actions are specified. |

**Example: Executing a graph directly**
```bash
# Load a graph, compute node 4, and save its output image
./build/graph_cli --read my_graph.yaml --compute 4 --save 4 final_image.png```

### 3.2. The REPL (Interactive Shell)

The REPL is the most powerful way to use Photospider. Start it by running the application with no flags:
```bash
./build/graph_cli
```
This will present you with the `ps>` prompt.

---

## 4. REPL Commands

#### `help`
Displays a list of all available REPL commands.

#### `read <filepath>`
Loads a graph from the specified YAML file into the **current session**, overwriting its content. If no session is active, loads into a new 'default' session.

#### `load <name> [yaml_path]`
Loads a graph into a named session called `<name>`. If `yaml_path` is omitted, it tries to load from `sessions/<name>/content.yaml`. If provided, it loads from the specified file and copies it into the session.

#### `switch <name> [c]`
Switches the active session to `<name>`. If `c` is provided, it first copies the content of the current session to the target session `<name>` and then switches.

#### `close <name>`
Closes a loaded graph session, removing it from memory.

#### `graphs`
Lists all currently loaded graph sessions and indicates which one is active.

#### `source <filepath>`
Executes a series of REPL commands from a script file. Lines starting with `#` are ignored.

#### `node [<id>]`
Opens the interactive **TUI Node Editor**. If `<id>` is provided, it focuses on that node.

#### `print [<id>|all] [full|simplified]`
Displays a hierarchical dependency tree.
*   **Target**: `<id>` for a specific tree, or `all` for all final nodes (default).
*   **Mode**: `full` shows parameters, `simplified` hides them.

#### `traversal [flags]`
Shows the evaluation order for the graph.
*   **Tree Flags**: `f` (full), `s` (simplified), `n` (no tree).
*   **Cache Flags**: `m` (memory status), `d` (disk status), `c` (sync memory to disk), `cr` (sync and remove orphaned disk files).

#### `ops [all|builtin|plugins]`
Lists all registered operations.
*   `all`: Shows all operations (default).
*   `builtin`: Shows only built-in operations.
*   `plugins`: Shows only operations from plugins.

#### `output <filepath>`
Saves the structure of the currently loaded graph to a new YAML file.

#### `clear-graph`
Removes all nodes from the current in-memory graph.

#### `cc` or `clear-cache [m|d|md]`
Clears the computation cache for the current graph.
*   `m`: Clears the in-memory cache only.
*   `d`: Clears the on-disk cache only.
*   `md` or `dm`: Clears both (default).

#### `compute <id|all> [flags]`
Executes nodes and their dependencies.
*   **Target**: `all` computes all final nodes; `<id>` computes a specific node.
*   **Flags**:
    *   `force`: Re-computes nodes even if memory caches exist.
    *   `force-deep`: Re-computes nodes even if memory or disk caches exist.
    *   `parallel`: Uses a multi-threaded scheduler for computation.
    *   `t`: Prints a simple console timer summary.
    *   `tl [path]`: Logs detailed timer info to a structured YAML file.
    *   `m` or `mute`: Suppresses progress output during computation.

#### `config`
Opens the interactive **TUI Configuration Editor**.

#### `save <id> <filepath>`
Computes a node and saves its image output to a file.

#### `free`
Frees the in-memory cache for any node that is not a final "ending" node.

#### `exit`
Quits the interactive shell.

---

## 5. The TUI Editors

### TUI Node Editor (`node` command)
*   **Layout**:
    *   **Left Column**: A list of all nodes and the YAML editor for the selected node.
    *   **Right Column**: A view of the entire graph's dependency tree and a context-sensitive tree for the selected node.
*   **Keybindings**:
    *   `<Tab>`: Cycle focus between the four main panes.
    *   `<Ctrl-S>`: **Apply** changes from the editor to the in-memory graph.
    *   `<Ctrl-D>`: **Discard** changes in the editor.
    *   `<Ctrl-E>`: Open the node's YAML in your system's default `$EDITOR`.
    *   `<Esc>` or `<Ctrl-C>`: Exit the editor.

### TUI Config Editor (`config` command)
*   **Modes**: Navigate, Edit, and Command modes for managing settings.
*   **Keybindings**:
    *   `e`: Enter **Edit Mode** on the selected line.
    *   `: `: Enter **Command Mode**.
    *   `q`: Quit the editor.
*   **Commands**: In command mode, use `w` to write/save, `a` to apply for the current session, and `q` to quit.

---

## 6. Configuration (`config.yaml`)

This file controls the application's default behavior.

| Key | Description | Example Value |
| :--- | :--- | :--- |
| `cache_root_dir` | Directory for on-disk cache files. | `cache` |
| `plugin_dirs` | List of directories to scan for plugins (`.so`/`.dll`). | `[build/plugins]` |
| `cache_precision`| Precision for cached images ('int8' or 'int16'). | `int8` |
| `default_print_mode`| Default mode for the `print` command (`full` or `simplified`).| `full` |
| `default_traversal_arg`| Default flags for the `traversal` command. | `n` |
| `default_cache_clear_arg`| Default flags for the `cc` or `clear-cache` command. | `md` |
| `default_exit_save_path`| Default filename when saving an unsaved graph on exit. | `graph_out.yaml` |
| `exit_prompt_sync`| Default answer for syncing cache on exit (`true`/`false`). | `true` |
| `config_save_behavior`| Save action after using the TUI `config` editor (`current`, `default`, `ask`, `none`). | `ask` |
| `editor_save_behavior`| Behavior for saving from the `node` editor (`ask`, `auto_save_on_apply`, `manual`). | `ask` |
| `default_timer_log_path`| Default file path for the `compute tl` command. | `out/timer.yaml` |
| `default_ops_list_mode`| Default mode for the `ops` command (`all`, `builtin`, `plugins`). | `all` |
| `ops_plugin_path_mode`| How to display plugin paths in `ops` (`name_only`, `relative_path`, `absolute_path`). | `name_only` |
| `default_compute_args`| Space-separated default flags for the `compute` command. | `t` |
| `history_size` | Number of commands to remember in REPL history. | `1000` |
| `switch_after_load`| After a `load`, automatically `switch` to the new session. | `true` |
| `session_warning`| Show a warning when overwriting an existing session's content. | `true` |

---

## 7. Built-in Operations

### 7.1. Image Source & Generation

#### `image_source:path`
Loads an image from a local file path.
*   **Parameters**: `path` (string, required).

#### `image_generator:perlin_noise`
Generates a Perlin noise image.
*   **Parameters**: `width` (int, 256), `height` (int, 256), `grid_size` (double, 1.0).

#### `image_generator:constant`
Creates an image filled with a constant value.
*   **Parameters**: `width` (int, 0), `height` (int, 0), `value` (int, 0), `channels` (int, 1).

### 7.2. Image Processing

#### `image_process:gaussian_blur`
Applies a Gaussian blur.
*   **Parameters**: `ksize` (int, 3), `sigmaX` (double, 0.0).

#### `image_process:resize`
Changes image dimensions.
*   **Parameters**: `width` (int, required), `height` (int, required), `interpolation` (string, "linear").

#### `image_process:crop`
Extracts a rectangular region.
*   **Parameters**: `mode` (string, "value"), `x`, `y`, `width`, `height` (required).

#### `image_process:extract_channel`
Isolates a single color channel.
*   **Parameters**: `channel` (string or int, "a"). Values: "b", "g", "r", "a" or 0-3.

#### `image_process:convolve`
Applies a 2D convolution using another image as the kernel.
*   **Inputs**: 2 images (source, kernel).
*   **Parameters**: `padding` (string, "replicate"), `absolute` (int, 1), `horizontal_and_vertical` (int, 0).

#### `image_process:curve_transform`
Applies the formula `output = 1 / (1 + k * input)`.
*   **Parameters**: `k` (double, 1.0).

### 7.3. Image Mixing

#### `image_mixing:add_weighted`
Blends two images: `alpha*img1 + beta*img2 + gamma`.
*   **Inputs**: 2 images.
*   **Parameters**: `alpha` (double, 0.5), `beta` (double, 0.5), `gamma` (double, 0.0), `merge_strategy` (string, "resize"), `channel_mapping` (map, optional).

#### `image_mixing:diff`
Computes the absolute difference between two images.
*   **Inputs**: 2 images.
*   **Parameters**: `merge_strategy` (string, "resize").

#### `image_mixing:multiply`
Performs pixel-wise multiplication of two images.
*   **Inputs**: 2 images.
*   **Parameters**: `scale` (double, 1.0), `merge_strategy` (string, "resize").

### 7.4. Data & Utility

#### `analyzer:get_dimensions`
Outputs the width and height of an image as data.
*   **Outputs**: Data (`width`: int, `height`: int).

#### `math:divide`
Divides two numbers.
*   **Outputs**: Data (`result`: double).
*   **Parameters**: `operand1` (double, required), `operand2` (double, required).
