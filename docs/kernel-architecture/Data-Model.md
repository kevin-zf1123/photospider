# Kernel Data Model

This document describes the graph and node data structures used by the current
kernel. It focuses on the public behavior that operators, schedulers, plugins,
and frontends should rely on.

## GraphModel

`GraphModel` is the in-memory state for a graph. Each `GraphRuntime` owns one
`GraphModel`.

Important fields:

| Field | Meaning |
| --- | --- |
| private node storage | Map from node id to `Node`, accessed through `GraphModel` lookup, iteration, and mutation helpers. |
| topology adjacency index | Incoming and outgoing `GraphTopologyEdge` maps for image and parameter edges, keyed by stable node id. |
| `cache_root` | Resolved root directory for this graph's disk cache files. |
| `timing_results` | Latest timing summary when timing is enabled. |
| `total_io_time_ms` | Accumulated disk cache IO time. |
| `last_disk_cache_load_result` | Most recent disk-cache load diagnostic, including skipped/miss/hit/error status and error details when a read fails. |

External code must not mutate graph structure through raw node-map access.
Reads use helpers such as `node()`, `find_node()`, `node_ids()`, and controlled
iteration. Structural changes use helpers such as `add_node()`,
`replace_node()`, `remove_node()`, and input-rewire APIs, which validate and
refresh topology adjacency before returning. Runtime cache/state updates may
use `mutable_node()` for node-local runtime fields, but structural edits still
belong to the model mutation helpers.

Internal services coordinate locking, timing, cache, topology, and traversal
behavior through the model boundary. Most frontend code should reach graph
state through `Kernel` or `InteractionService`.

For CLI-loaded graphs, `cache_root` is derived from the loaded
`cache_root_dir` config as `<cache_root_dir>/<graph_name>`, with relative paths
resolved from the process working directory. Lower-level `Kernel::load_graph`
callers that omit a cache root keep the session-local fallback
`<root_dir>/<graph_name>/cache`.

`GraphModel::clear()` is intended to reset model-level runtime state, not only
erase nodes. Clearing a graph resets nodes, topology adjacency, timing results,
accumulated IO time, skip-save state, and other per-run state so reload behavior
is not polluted by stale metadata.

## Topology Adjacency

`GraphModel` owns `GraphTopologyIndex`, which records both directions of graph
edges:

- `incoming_by_node`: upstream dependencies for a node.
- `outgoing_by_node`: downstream dependents for a node.

Each `GraphTopologyEdge` stores stable source and target node ids, edge kind
(`ImageInput` or `ParameterInput`), source output name, target input/parameter
identity, and input slot index. Successful graph load, clear, node addition,
node replacement, node removal, and input rewiring refresh or clear this index
before graph state is visible to traversal, compute, cache, inspection, CLI, or
interaction consumers.

## Node Identity

Each `Node` has:

| Field | Meaning |
| --- | --- |
| `id` | Unique integer id inside the graph. |
| `name` | Human-readable label. |
| `type` | Operation family, such as `image_process`. |
| `subtype` | Operation subtype, such as `gaussian_blur`. |
| `preserved` | Prevents some force-recompute paths from discarding the node. |

Operation lookup uses `type:subtype` through `OpRegistry`.

## Inputs

Node inputs are split by data kind:

| Input type | Structure | Meaning |
| --- | --- | --- |
| Image input | `ImageInput` | Reads an upstream image-like `NodeOutput`. |
| Parameter input | `ParameterInput` | Reads an upstream named data output and writes it into runtime parameters. |

The old unified input model is not part of the maintained schema.

## Parameters

`Node::parameters` contains static YAML parameters loaded from graph YAML.

`Node::runtime_parameters` is rebuilt for execution by cloning static
parameters and applying `parameter_inputs`. Operators should read effective
values from `runtime_parameters` during compute.

## Outputs

`NodeOutput` contains:

| Field | Meaning |
| --- | --- |
| `image_buffer` | Image payload as the public `ImageBuffer` contract. |
| `data` | Named scalar or structured outputs stored as YAML nodes. |
| `space` | Spatial transform, scale, and ROI metadata. |
| `debug` | Worker/device/timing/range diagnostics. |

Operators may return image data, named data, or both.

## Cache Fields

The cache-related node fields are:

| Field | Status | Meaning |
| --- | --- | --- |
| `cached_output_high_precision` | Formal cache | HP cache for full-quality reusable output. |
| `cached_output_real_time` | Transient RT state | Interactive preview/update output. |

Only HP output is formal reusable cache. That means only HP output may feed
subsequent HP compute, disk cache, long-term storage, and other reusable cache
behavior. `cached_output_real_time` is transient interactive state and must not
be used as authoritative cached output.

## YAML Schema

Graph YAML root is a sequence of node objects. Supported node fields:

```yaml
- id: 1
  name: source
  type: image_source
  subtype: path
  preserved: false
  image_inputs:
    - from_node_id: 0
      from_output_name: image
  parameter_inputs:
    - from_node_id: 2
      from_output_name: value
      to_parameter_name: strength
  parameters:
    path: assets/input.png
  outputs:
    - output_id: 0
      output_type: image
  caches:
    - cache_type: image
      location: output.png
```

`id` is required. Other fields default according to `Node::from_yaml`.
`parameter_inputs` require non-empty `from_output_name` and
`to_parameter_name`.

## Spatial Metadata

`SpatialContext` carries transform and ROI metadata used by ROI propagation and
inspection:

| Field | Meaning |
| --- | --- |
| `transform_matrix` | Global transform matrix. |
| `inverse_matrix` | Global inverse transform. |
| `local_inverse_matrix` | Local inverse used for upstream ROI projection. |
| `absolute_roi` | Output extent or valid region. |
| `global_scale_x`, `global_scale_y` | Scale metadata. |

`SpatialDependencyMap` is an optional node-local LUT for data-dependent spatial
propagation.
