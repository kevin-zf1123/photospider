# Kernel Data Model

This document describes the graph and node data structures used by the current
kernel. `GraphDefinition` is the detached persistent document value;
`GraphModel` and `Node` are private backend runtime state, not shared public
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

## GraphDefinition and the In-Memory Adapter

`GraphDefinition` is a deep-owned, format-neutral value for one complete graph
document. It owns an ordered vector of `NodeDefinition` values. Each
`NodeDefinition` contains only persistent identity, operation type/subtype,
image and parameter edges, static `ParameterMap`, output and cache descriptors,
and the `preserved` flag. It has no runtime parameters, computed outputs,
revisions, ROI/LUT state, timing, dirty state, or cache results.

`InMemoryGraphDocumentAdapter` is the only translator between that detached
value and private `Node`/`GraphModel` state:

- apply validates duplicate ids and required parameter-edge names while
  staging a complete `GraphModel::NodeMap`, then calls
  `GraphModel::replace_nodes()` exactly once;
- capture visits graph nodes in ascending id order and copies only persistent
  fields into an independent definition;
- single-node materialization/capture supports the ABI-stable
  `Host::get_node_yaml()` / `Host::set_node_yaml()` operations without
  restoring YAML methods on `Node`.

The adapter owns no graph, file, parser tree, cache, or thread. Callers retain
the existing `GraphStateExecutor` serialization responsibility. A definition
or topology failure before replacement preserves the prior node map, topology,
generation, and runtime state.

`GraphDocumentReader` and `GraphDocumentWriter` are separate format-neutral
contracts. Complete-graph methods exchange filesystem paths and detached
`GraphDefinition` values; node methods exchange owned text and detached
`NodeDefinition` values. Neither contract exposes yaml-cpp, `GraphModel`,
`Node`, cache state, or provider-library types.

`GraphIOService` requires non-null shared reader/writer owners. It retains
model orchestration only: load asks the reader for a detached definition and
applies it through `InMemoryGraphDocumentAdapter`; save captures a definition
before calling the writer; node-document operations cross the same injected
boundary. It constructs no parser, emitter, or graph-document stream.

The configured `YamlGraphDocumentAdapter` owns the private YAML translator,
filesystem read, node-text conversion, complete emission, and direct
open/write/flush/close behavior. `create_embedded_host()` constructs one
adapter and injects the same shared owner as both contracts through `Kernel`;
Kernel and GraphIO have no default persistence construction. A private
explicit-dependency Host root supports deterministic fake substitution without
adding an installed API. Issue #61 implements this document boundary. Issue
#62 moves the shared YAML value translator into that private adapter area and
removes YAML types from runtime and cache contracts. Only the
dependency-disabled product profile remains for Issue #63.

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

`NodeDefinition::parameters` and `Node::parameters` are
`plugin::ParameterMap` values containing deep-owned static data. The configured
YAML adapter's private translator converts a graph document into a detached
definition once; the in-memory adapter then copies that definition into Graph
state. Neither the definition nor Graph storage retains the source YAML tree.
Values use the exact `ParameterValue` alternatives `Null`, `Bool`, `Int64`,
`Double`, `String`, `Array`, and string-keyed `Object`.

Inspection renders these values through the format-neutral
`format_parameter_value_for_inspection()` helper. Scalar spellings remain
stable, arrays and objects are rendered recursively, object keys keep their
ordered-map order, and strings are quoted and escaped without constructing a
YAML node or emitter.

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
| `debug` | Worker/device/timing/range diagnostics. Enabled CPU range inspection walks active scalar bytes through `ImageBuffer::step`; padding is excluded and opaque device values retain provider diagnostics. |

Operators may return image data, named data, or both.

Persistent `OutputPort::output_parameters` is an optional deep-owned
`ParameterValue`. An empty optional means the document field was absent; an
engaged null value preserves an explicitly present YAML null. Nested output
configuration therefore survives parser destruction without retaining
`YAML::Node`.

For tiled `image_mixing`, a secondary input that requires crop/pad is
materialized as a request-local `NodeOutput`: named data, spatial/debug
provenance, and plugin-library lifetime are copied, while its image descriptor
is replaced by aligned storage produced through kernel fill/copy primitives.
Resize and channel conversion remain local OpenCV algorithm calls. The
normalization context owns these temporary outputs until every synchronous tile
callback finishes; exact-shape inputs continue to borrow the upstream output.

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
      output_parameters:
        color_space: linear
        channels: [red, green, blue]
  caches:
    - cache_type: image
      location: output.png
```

`id` is required. Other fields use the configured YAML adapter translator's
established defaults. `parameter_inputs` require non-empty
`from_output_name` and `to_parameter_name`. `output_parameters` may be absent,
explicitly null, or any representable recursive `ParameterValue`.

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

- `GraphDefinition` is a detached private document value; `GraphModel` and
  `Node` are private backend runtime state. Public Host callers and operation
  plugins receive copied public values rather than model references.
- Structural mutation goes through model helpers so node storage, both
  adjacency directions, topology generation, and cached planning state become
  visible as one coherent graph state.
- Schedulers receive ready-task metadata and never own node storage,
  parameters, output values, topology, or cache authority.
- `YAML::Node` remains only inside private YAML adapters for graph documents,
  shared value translation, and configured cache metadata. It is not declared
  by runtime, graph, compute, inspection, or cache contracts and is not owned
  by `GraphDefinition`, persistent `Node` fields, or `OutputPort`.
  Static/effective parameters, output-port configuration, and named operation
  outputs are `ParameterValue` trees. Graph extents, spatial metadata, dirty
  snapshots, and compute-task geometry use kernel-owned `PixelSize` and
  `PixelRect` values. OpenCV geometry is created only inside an OpenCV provider
  or algorithm implementation when a matrix slice or library call requires it.

Keeping graph identity and topology in one model makes traversal, compute,
inspection, and mutation observe the same generation. Issue #62 completes the
runtime/cache YAML value boundary without making the configured product
dependency-optional. The remaining configured-product and provider-library
dependency work is governed by
[ADR 0002](../adr/0002-external-libraries-are-kernel-adapters.md) and the exact
[dependency-neutral kernel target](../roadmap/Kernel-Evolution.md#dependency-neutral-kernel);
neither document changes the current fields described above.

## Implementation and Validation Entry Points

- `src/lib/graph/graph_model.*`
- `src/lib/graph/node.hpp`
- `src/lib/graph/graph_definition.hpp`
- `src/lib/graph/graph_document_reader.hpp`
- `src/lib/graph/graph_document_writer.hpp`
- `src/lib/graph/in_memory_graph_document_adapter.*`
- `src/lib/adapters/yaml/graph_definition_yaml.*`
- `src/lib/adapters/yaml/yaml_graph_document_adapter.*`
- `src/lib/adapters/yaml/parameter_value_yaml.*`
- `src/lib/adapters/yaml/yaml_cache_metadata_codec.*`
- `src/lib/core/cache_metadata_codec.hpp`
- `src/lib/core/parameter_value_text.*`
- `src/lib/graph/graph_io_service.*`
- `src/lib/core/ps_types.*`
- `src/lib/compute/tiled_input_normalizer.*`
- `src/lib/compute/compute_metrics_recorder.*`
- `tests/unit/test_graph_topology_boundaries.cpp`
- `tests/unit/test_graph_document_adapter.cpp`
- `tests/integration/test_graph_document_injection.cpp`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_stride_aware_compute_paths.cpp`
- `tests/integration/test_graph_document_errors.cpp`
