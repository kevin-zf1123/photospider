# Graph Lifecycle and Mutation Semantics

This document describes the current ownership, publication, mutation, and
failure behavior of graph sessions. It records implemented behavior, including
known boundary limitations; proposed persistence abstractions belong in the
kernel evolution roadmap.

## Ownership

`Kernel` owns a map from graph names to `GraphRuntime` instances. Each runtime
owns one `GraphModel`, one `GraphStateExecutor`, event and scheduler state, and
platform runtime resources.

```text
Kernel
  graph name -> GraphRuntime
                  -> GraphStateExecutor
                  -> GraphModel
```

The public `ps::Host` returns copied `GraphSessionId` values. A session id is a
label, not a graph or runtime handle. Graph-state mutation and visible compute
enter the same per-graph exclusive access boundary.

## New Session Load

`Kernel::load_graph()` constructs a runtime outside the published graph map,
starts its configured schedulers, and then loads
`<root>/<session>/content.yaml` when that file exists. An explicitly supplied
source file is first copied to that session path when the source exists.

Graph document loading is transactional with respect to ordinary
parse/validation failure and session publication:

1. `GraphIOService` parses all YAML sequence entries into a temporary node map.
2. Duplicate ids, dependencies, and cycles are validated before replacement.
3. `GraphModel::replace_nodes()` installs nodes and topology together, resets
   graph runtime metadata, and advances topology generation.
4. `Kernel` inserts the runtime into its map only after the load completes.

A parse, node-construction, or topology failure therefore publishes neither a
partial graph nor a new session. Resource exhaustion propagates. Other handled
load failures return a failed Host result. Directory creation and file-copy
side effects that happened before parsing are not rolled back.

The current source-path boundary has three important limitations:

- directory creation performed by the `GraphRuntime` constructor is outside
  Kernel's best-effort copy block. A filesystem failure propagates as an I/O
  load failure and no session is published, but directories already created by
  the constructor are not rolled back;
- the later session-directory setup and source/config copy block suppresses
  failures other than `std::bad_alloc` before the document load step;
- when the caller supplies a non-empty YAML path that does not exist, it does
  not replace the session-local target. Kernel loads an older
  `content.yaml` if one is already present; otherwise it prints a warning and
  publishes an empty session instead of returning an I/O failure.

An omitted YAML path is different: Kernel uses an existing session-local
`content.yaml` when present, otherwise it intentionally publishes an empty
session. These cases are not yet represented by one frozen load-error matrix.

## Existing Session Reload

`Host::reload_graph()` first distinguishes a missing session, then submits the
reload through `GraphStateExecutor`. `GraphIOService` builds and validates a
temporary replacement exactly as it does for initial loading. Until
`replace_nodes()` succeeds, the visible node map, topology, topology
generation, cache, timing, and dirty/planning state remain unchanged.

Successful reload replaces the whole graph, resets model runtime state, and
advances topology generation even when node ids are reused. Runtime-owned
mirrors such as `RealtimeProxyGraph` observe that generation boundary and
discard stale per-node state.

## Existing Session Save

`Host::save_graph()` admits the session against concurrent close, requires the
session map entry, and serializes the visible node snapshot through the same
`GraphStateExecutor` used by graph mutation and compute. A missing or closing
session is `GraphErrc::NotFound`. Destination access, node serialization, and
YAML emission failures for an existing session are normalized to
`GraphErrc::Io`.

## Node Replacement and Structural Edits

`Host::set_node_yaml()` admits the session against concurrent close. Required
node lookup, candidate parsing, forced replacement-id assignment, and
`GraphModel::replace_node()` execute in one graph-state work item, so clear or
reload cannot enter between lookup and mutation. A missing or closing session,
or a missing requested node, is `GraphErrc::NotFound`. Parsing and complete
candidate-topology validation for an existing target are
`GraphErrc::InvalidYaml`.

Replacement copies the current node map, validates the complete candidate
topology, and only then swaps it into visible state. Parse,
missing-dependency, or cycle validation failure leaves the previous node map
and topology unchanged. The current implementation does not claim an
all-exception strong guarantee: allocation failure while rebuilding the
already-validated topology may occur after the candidate node map has been
moved into the model.

`add_node()`, `remove_node()`, and input-rewire methods follow the same
candidate-map pattern. A successful structural edit rebuilds the adjacency
index, advances topology generation, and clears the cached full task graph.

## ROI Projection

`Host::project_roi()` and `Host::project_roi_backward()` admit the session
against close and perform both endpoint lookup and propagation in one
graph-state work item. Missing or closing sessions and missing source/target
nodes are `GraphErrc::NotFound`. When both endpoints exist but the ROI is empty,
the path is unreachable, or propagation produces no valid rectangle, the Host
returns `GraphErrc::InvalidParameter`.

Existing-session propagation exceptions continue to update Kernel's
best-effort `LastError` mirror, but the current Host result comes directly from
the same required operation. It is never reconstructed by reading shared
diagnostic state after the operation.

## Clear

`GraphModel::clear()` performs a model reset, not only node deletion. It clears:

- nodes and topology adjacency;
- timing and accumulated I/O state;
- dirty snapshots, generations, and source commit state;
- compute-plan history and full-task-graph cache;
- disk-cache diagnostics and skip-save state.

Clear advances topology generation and leaves the model in quiet mode. It does
not close or destroy the owning `GraphRuntime`; the session remains loaded. It
also does not delete disk-cache files, clear runtime-owned event/trace rings,
directly clear `RealtimeProxyGraph`, or clear Kernel-owned `LastError`.
`RealtimeProxyGraph` invalidates itself when its next synchronization observes
the advanced topology generation.

## Close and Lifetime

Embedded Host close first marks the session closing. New compute, scheduler,
required save, node-YAML replacement, and ROI projection admissions fail, while
close waits for admitted synchronous calls and caller-visible async status
publication. Kernel then stops the runtime through the same
`GraphStateExecutor` and removes the map entry.

Concurrent close callers serialize through the Host lifecycle gate. A runtime
stop failure retains the runtime and diagnostic state, clears the closing
marker, and reopens admission. `NotFound` is reserved for a session that is
actually absent.

`photospiderd` owns daemon session identity, job admission, Host serialization,
and shutdown drainage around this embedded Host contract. Its exact mapping,
lease, socket, and shutdown rules are defined in
`../codebase-structure/IPC-Protocol-v1.md`; they are not graph-kernel ownership.

## Current Error Surface

| Operation | Current public behavior |
| --- | --- |
| initial load, duplicate session | failed load result, currently classified as `InvalidParameter` by the embedded Host |
| initial load, runtime directory creation failure | no session publication; reported as `GraphErrc::Io`; already-created filesystem side effects are not rolled back |
| initial load, document parse/topology failure | no session publication; detailed backend category currently collapses to the same load failure |
| initial load, explicit missing source | loads an older session-local `content.yaml` when present; otherwise warns and publishes an empty session |
| reload, missing session | `GraphErrc::NotFound` |
| reload, unreadable source or YAML syntax parser failure | `GraphErrc::Io` |
| reload, non-sequence or duplicate-id document | `GraphErrc::InvalidYaml` |
| reload, dependency/cycle validation | the corresponding backend `GraphErrc` |
| reload, uncategorized YAML conversion exception | `GraphErrc::Unknown` through the stored last-error path |
| save, missing or closing session | `GraphErrc::NotFound` |
| save, destination access, serialization, or YAML emission failure | `GraphErrc::Io` |
| node replacement, missing/closing session or missing requested node | `GraphErrc::NotFound` |
| node replacement, existing target with malformed input, missing dependency, or cycle | `GraphErrc::InvalidYaml`; previous graph state remains visible |
| forward/backward ROI projection, missing/closing session or missing endpoint | `GraphErrc::NotFound` |
| forward/backward ROI projection, existing endpoints with no valid projection | `GraphErrc::InvalidParameter` |
| clear or close, missing session | `GraphErrc::NotFound` |

`OperationStatus` exposes an error domain, signed code, stable name, and
diagnostic message. Callers branch on the domain and code, not diagnostic text.
The initial-load inconsistencies above are current limitations, not a general
graph-document contract.

## Implementation and Validation Entry Points

- `src/lib/runtime/kernel.cpp`
- `src/lib/runtime/kernel_io_cache_facade.cpp`
- `src/lib/runtime/kernel_inspection_facade.cpp`
- `src/lib/runtime/kernel_dirty_roi_facade.cpp`
- `src/lib/graph/graph_io_service.cpp`
- `src/lib/graph/graph_model.cpp`
- `src/lib/host/embedded_host.cpp`
- `tests/integration/test_host_adapter.cpp`
- `tests/integration/test_ipc_daemon.cpp`
- `tests/integration/test_kernel_contracts.cpp`
