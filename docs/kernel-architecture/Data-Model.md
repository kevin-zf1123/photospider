# Kernel Data Model

This document describes the graph and node data structures used by the current
kernel. `GraphModel` and `Node` are private backend state, not shared public
contracts. Frontends use `ps::Host` values, operation plugins use the operation
SDK, and schedulers receive only ready-task metadata. This document explains
the internal behavior those boundaries ultimately operate on.

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
| disk-cache diagnostic snapshot | Most recent disk-cache load diagnostic, including skipped/miss/hit/error status and error details when a read fails. GraphModel protects this state with a dedicated diagnostic mutex and exposes value snapshots to readers. |

External code must not mutate graph structure through raw node-map access.
Reads use helpers such as `node()`, `find_node()`, `node_ids()`, and controlled
iteration. Structural changes use helpers such as `add_node()`,
`replace_node()`, `remove_node()`, and input-rewire APIs, which validate and
refresh topology adjacency before returning. Runtime cache/state updates may
use `mutable_node()` for node-local runtime fields, but structural edits still
belong to the model mutation helpers.

Internal services coordinate locking, timing, cache, topology, and traversal
behavior through the model boundary. Frontend, CLI, and TUI code reaches graph
state through the public `ps::Host` seam. The embedded Host adapter delegates
to the internal `InteractionService`/`Kernel` boundary; backend services may
use that internal boundary but do not expose it to frontend callers.

For CLI-loaded graphs, `cache_root` is derived from the loaded
`cache_root_dir` config as `<cache_root_dir>/<graph_name>`, with relative paths
resolved from the process working directory. Lower-level `Kernel::load_graph`
callers that omit a cache root keep the session-local fallback
`<root_dir>/<graph_name>/cache`.

`GraphModel::clear()` resets model-level runtime state, not only nodes.
Clearing a graph resets nodes, topology adjacency, timing results,
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

## Parameters

`Node::parameters` is a `plugin::ParameterMap` containing deep-owned static
values. `Node::from_yaml()` converts the graph document once at ingestion;
Graph storage does not retain the source YAML tree. Values use the exact
`ParameterValue` alternatives `Null`, `Bool`, `Int64`, `Double`, `String`,
`Array`, and string-keyed `Object`.

`Node::runtime_parameters` is another `ParameterMap`, rebuilt for execution by
copying static values and applying `parameter_inputs`. Connected named outputs
replace same-name static values without a format conversion. Operators should
read effective values from `runtime_parameters` during compute. Executors
populate it on request-local node snapshots; it is not committed as reusable
Graph state.

## Outputs

`NodeOutput` contains:

| Field | Meaning |
| --- | --- |
| `image_buffer` | Image payload as the public `ImageBuffer` contract. |
| `data` | Named scalar or structured outputs stored as a `plugin::ParameterMap`. |
| `space` | Spatial transform, scale, and ROI metadata. |
| `debug` | Worker/device/timing/range diagnostics. |

Operators may return image data, named data, or both.

## Cache Fields

The cache-related node fields are:

| Field | Status | Meaning |
| --- | --- | --- |
| `cached_output_high_precision` | Formal cache | HP cache for full-quality reusable output. |

Only HP output is formal reusable cache. That means only HP output may feed
subsequent HP compute, disk cache, long-term storage, and other reusable cache
behavior. RT output is not stored on `Node`; it lives in `RealtimeProxyGraph`,
which mirrors node ids and stores low-resolution proxy output, HP-space ROI,
version, and RT dirty-source generation.

Dirty RT worker tasks stage proxy output through `RealtimeProxyWriteBuffer`
before committing to `RealtimeProxyGraph`. Dirty HP worker tasks stage formal
HP output through `HighPrecisionDirtyWriteBuffer` before committing to
`GraphModel`, with RealTimeUpdate HP commits gated behind successful RT proxy
commit.

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

## Boundaries and Rationale

- `GraphModel` and `Node` are private backend state. Public Host callers and
  operation plugins receive copied public values rather than model references.
- Structural mutation goes through model helpers so node storage, both
  adjacency directions, topology generation, and cached planning state become
  visible as one coherent graph state.
- Schedulers receive ready-task metadata and never own node storage,
  parameters, output values, topology, or cache authority.
- `YAML::Node` remains a graph-document, legacy output-port configuration, and
  disk-cache metadata representation at adapter boundaries. Static/effective
  parameters and named operation outputs are `ParameterValue` trees throughout
  Graph, compute, ROI, and operation invocation. Graph extents, spatial
  metadata, dirty snapshots, and compute-task geometry use kernel-owned
  `PixelSize` and `PixelRect` values. OpenCV geometry is created only inside an
  OpenCV provider or algorithm implementation when a matrix slice or library
  call requires it.

Keeping graph identity and topology in one model makes traversal, compute,
inspection, and mutation observe the same generation. The accepted replacement
for the remaining YAML and provider-library dependencies is governed by
[ADR 0002](../adr/0002-external-libraries-are-kernel-adapters.md) and the exact
[dependency-neutral kernel target](../roadmap/Kernel-Evolution.md#dependency-neutral-kernel);
neither document changes the current fields described above.

## Implementation and Validation Entry Points

- `src/lib/graph/graph_model.*`
- `src/lib/graph/node.hpp`
- `src/lib/graph/node_yaml.cpp`
- `src/lib/core/parameter_value_adapter.*`
- `src/lib/graph/graph_io_service.*`
- `src/lib/core/ps_types.*`
- `tests/unit/test_graph_topology_boundaries.cpp`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_graph_document_errors.cpp`
