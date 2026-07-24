# Graph Lifecycle and Mutation Semantics

This document describes the current ownership, publication, mutation, and
failure behavior of graph sessions. It records implemented behavior, including
known boundary limitations. The in-memory GraphDefinition seam, injected YAML
document/cache adapters, and the dependency-disabled unavailable persistence
adapters are current behavior.

## Ownership

`Kernel` owns a map from graph names to `GraphRuntime` instances. Each runtime
owns one `GraphModel`, one graph-state executor, one private compute-request
executor, one latest-wins request coordinator, copied HP/RT execution-route
bindings, event/execution-trace state, and platform runtime resources.

```text
Kernel
  graph name -> GraphRuntime
                  -> GraphStateExecutor
                  -> compute-request lane
                  -> ComputeRequestCoordinator
                  -> GraphModel
```

The public `ps::Host` returns copied `GraphSessionId` values. A session id is a
label, not a graph or runtime handle. Each live `GraphModel` additionally owns a
private strong non-reused `GraphInstanceId` and checked nonzero
`GraphRevision`. Visible capture, mutation, commit validation, and publication
enter the graph-state lane. Long-running operation execution uses a
request-owned snapshot outside it. The coordinator publishes checked
supersession generations and coalesces one pending owner per exact key; the
existing compute-request lane worker remains the only logical active runner
while that lane also serializes execution-route inspection/replacement.

## New Session Load

`Kernel::load_graph()` configures the Host-lifetime fixed worker count when
needed, constructs a runtime with copied route bindings outside the published
graph map, validates the complete graph document, and only then inserts the
runtime. Source selection distinguishes omission from an explicit request:

- an empty `yaml_path` uses `<root>/<session>/content.yaml` when that file
  exists and otherwise intentionally creates an empty session;
- every nonempty `yaml_path` is explicit. Kernel requires it to exist and be
  copyable into the session path unless it already denotes that same file. An
  explicit-source failure is `GraphErrc::Io` and never falls back to older
  session content.

Worker requests are valid from zero through eight; zero resolves to
`min(max(1, hardware_concurrency()), 8)` and explicit positive values remain
exact. The first configuration or load that selects the fixed service freezes
that process count; later zero/equal requests preserve it and a conflicting
positive request is rejected. Each runtime copies one closed-vocabulary
`cpu`, `serial_debug`, or `gpu_pipeline` route id for HP and RT with generation
one. Graph load constructs no physical executor, policy context, scheduler,
plugin, or ledger reservation. Each Run is admitted later with its complete
resource vector through the common ready/policy/reserved-start boundary.

Graph document loading is a prepare-then-publish transaction:

1. `GraphIOService` asks its injected `GraphDocumentReader` for an ordered,
   detached `GraphDefinition`. The configured `YamlGraphDocumentAdapter` owns
   filesystem access, YAML parsing, and translation; static parameters and
   optional output parameters are deep-owned `ParameterValue` trees with no
   YAML aliases.
2. The YAML adapter classifies parser, root-shape, parameter representation,
   and node-field failures while producing the definition. Non-map parameter
   values, unsupported tags, normalized key collisions, and Int64/Double
   overflow are document failures.
3. `InMemoryGraphDocumentAdapter` rejects duplicate ids and empty required
   parameter-edge names while staging the complete temporary node map.
4. The adapter calls `GraphModel::replace_nodes()` exactly once. That call
   validates dependencies and cycles, builds replacement adjacency, installs
   nodes and topology together, resets graph runtime metadata, and advances
   topology generation plus authoritative `GraphRevision`.
5. `Kernel` inserts the runtime into its map only after the load completes.

A path, parser, definition conversion, topology, unexpected, or resource failure
therefore publishes neither a partial graph nor a new session.
`std::bad_alloc` propagates unchanged; other failures use the stable matrix
below. Directory creation and file-copy side effects that happened before
parsing are not rolled back, but they are not graph-map publication. A failure
before publication destroys the unpublished runtime and its copied route
bindings; it has no Graph-owned execution reservation to return. A fixed worker
pool already configured for the Host remains available for later sessions.

The classification and transaction rationale is fixed by
[ADR 0005](../adr/0005-graph-document-ingestion-is-a-classified-transaction.md).

## Existing Session Reload

`Host::reload_graph()` first admits the session against concurrent close, then
distinguishes a missing session and submits the reload through
`GraphStateExecutor`. A missing or closing session is `NotFound` even when the
supplied path is empty; an empty path for an existing admitted session is
`InvalidParameter`. The admission precedes session lookup and remains live
through backend LastError translation, so close cannot erase the runtime or its
diagnostic state after accepting the reload. Every nonempty source uses the
same IO, syntax/schema, topology, unexpected-failure, and resource-exhaustion
classification as initial load.

`GraphIOService` builds a detached definition and the in-memory adapter stages
and validates a temporary replacement exactly as they do for initial loading.
Until the adapter's single `replace_nodes()` call succeeds, the visible node
map, topology, topology generation, authoritative revision, cache, timing,
dirty/planning state, runtime state, and Graph/session identity remain
unchanged. This guarantee covers handled failures and propagated
`std::bad_alloc`.

Successful reload replaces the whole graph, resets model runtime state, retains
the live Graph instance identity, and advances both topology generation and
authoritative revision even when node ids are reused. Runtime-owned mirrors
such as `RealtimeProxyGraph` observe the topology-generation boundary and
discard stale per-node state; staged compute also fails exact revision
validation rather than publishing into the replacement. Disk-cache diagnostic
reset crosses the same private no-throw store used by worker record and reader
snapshot. It occurs only in successful replacement publication; a failed reload
retains the prior complete diagnostic together with the rest of runtime state.

## Execution Route Replacement

Execution-route inspection and replacement are serialized with same-Graph
compute by the private per-Graph compute-request lane. Embedded Host inspection
first preserves missing/closing-session classification as `NotFound`, then
rejects an invalid `ComputeIntent` as `InvalidParameter` before the Kernel's
optional route lookup. A valid intent whose copied route is absent remains
`NotFound`; successful inspection returns only a copied route/statistics
snapshot.

For replacement, Kernel first rejects anything outside the closed `cpu`,
`serial_debug`, and `gpu_pipeline` vocabulary. Inside the lane,
`GraphRuntime` copies the candidate id before locking route state, verifies the
intent and checked generation, then publishes the copied id and next nonzero
generation together. Same-name replacement also advances the generation.

No replacement constructs a worker, device owner, policy context, plugin, or
resource reservation, so it needs no transient process headroom. Unknown
routes, invalid intents, generation exhaustion, or handled lane-lifecycle
failure preserve the previous binding and return `InvalidParameter`; a missing
or closing session remains `NotFound`. `std::bad_alloc` from candidate copying
propagates through the documented Host boundary without partial publication.

## Existing Session Save

`Host::save_graph()` admits the session against concurrent close, requires the
session map entry, and serializes the visible node snapshot through the
`GraphStateExecutor` used by graph mutation and compute capture/commit.
`GraphIOService` first
captures a detached definition in ascending node-id order through the
in-memory adapter and excludes all runtime state, then passes it to the
injected writer. The configured YAML adapter emits the complete representation
before destination open. A missing or closing session is
`GraphErrc::NotFound`, and resolution stops before destination access. For an
existing session, recoverable definition capture, YAML emission, and
destination preparation/open/write/flush/close failures are normalized to
`GraphErrc::Io`. Resource exhaustion remains the exact `std::bad_alloc`
exception channel rather than becoming an `Io` status.

Save is an owner-state read transaction. Success, a returned failure, and
propagated resource exhaustion leave the graph topology, topology generation,
authoritative revision, cache/timing/dirty/planning/runtime state, Graph
instance identity, and session identity unchanged. A caller may retry the same
admitted session after any reported failure; the IPC client sends each
mutation once and does not retry it automatically.

The destination has a deliberately narrower guarantee. Save writes directly
to the supplied path and does not use a temporary file plus atomic replacement.
A failure before the destination is successfully opened preserves existing
bytes. Once open succeeds, a write, flush, close, or later resource failure may
leave a created, truncated, or partially written destination. Destination
rollback is therefore not part of the graph-owner transaction.

## Node Replacement and Structural Edits

`Host::set_node_yaml()` admits the session against concurrent close. Required
node lookup, candidate `NodeDefinition` parsing through the injected reader,
forced replacement-id assignment, in-memory materialization, and
`GraphModel::replace_node()` execute in one graph-state work item, so clear or
reload cannot enter between lookup and mutation. `get_node_yaml()` uses the
inverse single-node capture through the injected writer. These public method
names remain for ABI stability; private Kernel/Interaction methods and
GraphIO are format-neutral. `Node` exposes no YAML conversion methods. A
missing or closing session, or a missing requested node, is
`GraphErrc::NotFound`. Parsing and complete candidate-topology validation for
an existing target are `GraphErrc::InvalidYaml`.

Replacement copies the current node map, validates the complete candidate
topology, prepares successor topology/revision generations, and only then uses
no-throw container swaps plus scalar stores to publish visible state. Parse,
missing-dependency, cycle, allocation, or generation-overflow failure therefore
leaves the previous node map, topology, generations, revision, and runtime state
unchanged. Publication begins only after all throwing structural preparation
has completed.

`add_node()`, `remove_node()`, and input-rewire methods follow the same
candidate-map pattern. A successful structural edit rebuilds the adjacency
index, advances topology generation and authoritative revision, and clears the
cached full task graph. A rejected candidate advances neither value.

Explicit disk, memory, combined, and transient-memory cache clears use a
different failure boundary because filesystem deletion and multi-node clearing
can succeed partially before a later operation throws. The graph-state work
item first computes the checked successor revision. Overflow therefore leaves
the Graph, caches, and files unchanged. After that pure preparation succeeds,
it publishes the successor revision without throwing and only then enters the
cache side effects. A later clear failure is returned through the existing
facade contract, but the revision is never rolled back: every Run captured
before the clear intent remains stale even when the cache root was removed but
could not be recreated, or only part of memory cache was released. This is
revision-safe invalidation, distinct from issue #73 cooperative cancellation.
A cache clear does not request cancellation; stale and cancelled Runs converge
only at the rule that neither may publish request-owned staged output.

## ROI Projection

`Host::project_roi()` and `Host::project_roi_backward()` admit the session
against close and perform both endpoint lookup and propagation in one
graph-state work item. Missing or closing sessions and missing source/target
nodes are `GraphErrc::NotFound`. When both endpoints exist but the ROI is empty,
the path is unreachable, or propagation produces no valid rectangle, the Host
returns `GraphErrc::InvalidParameter`.

Host requests and results plus private graph, propagation, dirty, and planning
state carry `PixelRect` and `PixelSize`. An OpenCV adapter or provider may
construct library geometry only locally at an actual matrix operation; that
representation does not enter the graph-state work item or its retained state.

Existing-session propagation exceptions continue to update Kernel's
best-effort `LastError` mirror, but the current Host result comes directly from
the same required operation. It is never reconstructed by reading shared
diagnostic state after the operation.

## Injected Persistence Lifetime

Persistence dependencies are composed once per `Kernel`. The embedded product
root supplies a shared `ImageArtifactCodec`, a shared
`YamlCacheMetadataCodec`, and one shared `YamlGraphDocumentAdapter` viewed
through the reader and writer contracts. `GraphCacheService` retains both
codec owners; `GraphIOService` retains the reader and writer. Kernel,
GraphCache, and GraphIO reject absent owners at construction and provide no
configured fallback.

These dependencies are not graph state: reload, clear, and close do not replace
them. `Kernel::~Kernel()` explicitly clears the owned runtime map before
ordinary member teardown reaches cache, traversal, diagnostic, IO, or ROI
collaborators. Each `GraphRuntime` therefore stops and drains compute-request
work while graph-state is available, then drains graph-state before releasing
Graph-local route values and platform state, all while those borrowed Kernel
services and injected owners remain alive; only later service destruction
releases them. The owning Host must stop
external Kernel-call admission before Kernel destruction, because the private
graph map is not a concurrent-destruction API. Codec/document `GraphError`
values retain their documented categories, and `std::bad_alloc` propagates
unchanged.

## Clear

`GraphModel::clear()` performs a model reset, not only node deletion. It clears:

- nodes and topology adjacency;
- timing and accumulated I/O state;
- dirty snapshots, generations, and source commit state;
- compute-plan history and full-task-graph cache;
- disk-cache diagnostics and skip-save state.

Clear advances topology generation and authoritative revision, then leaves the
model in quiet mode. It does not close or destroy the owning `GraphRuntime`; the
session remains loaded. It also does not delete disk-cache files, clear
runtime-owned event/trace rings, directly clear `RealtimeProxyGraph`, or clear
Kernel-owned `LastError`. `RealtimeProxyGraph` invalidates itself when its next
synchronization observes the advanced topology generation; an older staged
compute is rejected by revision validation. Diagnostic record, snapshot, reset,
clone, and staged exchange all use one encapsulated no-throw mutex contract, so
clear can overlap worker diagnostic traffic without unsynchronized
optional/path/string access.

## Close and Lifetime

One completely constructed `GraphRuntime` owns a stable
`GraphLifetimeAnchor` and a preallocated monotonic close coordinator. Kernel
registers the non-reused `GraphInstanceId` and anchor only after load is ready
to publish. A lifecycle candidate retains an anchor lease from its first
admission check through atomic standalone/realtime-bundle installation or
rollback, so close cannot miss planning that began before its marker.

Embedded Host close first selects or joins that one close generation and marks
the public session closing. New compute, execution, reload, required save,
node-YAML replacement, ROI projection, timing inspection, and all-cache
clearing admissions fail. Under the `RunLifecycleRegistry` fence, the owner
changes the exact Graph row `Open -> Closing`, rejects pending candidates,
denies visible commit, and requests `GraphClose` cancellation for every
installed Run. It releases the fence before waiting; a candidate either
installed before the marker or rolls back, and no late Run can enter the row.
Every live concurrent close caller joins the same generation result.

Owner selection alone is not the lifecycle linearization point. If that owner
fails before `Open -> Closing`, the coordinator publishes the exact
`std::exception_ptr` to every caller that already joined that generation. No
fresh owner may start while an old joiner can still observe that failure; the
last joiner consumes it before the coordinator returns to `Idle`, and the next
caller receives a fresh, non-reused generation. This generation-qualified
failure handoff prevents ABA and joiner hangs, while a destructor may retry the
ordinary close path. Once `Open -> Closing` linearizes, recoverable unwind is
forbidden and the existing fail-stop cleanup boundary applies.

Accepted synchronous calls finish result/status translation, accepted daemon
jobs keep status/result/release ownership, and already admitted Runs retain
only the ready, execution, completion, and graph-finalization paths needed to
settle. Finalization waits for terminal publication, physical callback
quiescence, commit/discard completion, exact root reservation and child-grant
release, Graph lifetime lease release, and registry unregistration. A
non-preemptible provider that never returns can therefore keep close blocked;
the implementation does not fabricate recovery.

After the Graph index and candidate count are empty, close performs the exact
irreversible tail:

1. remove the empty lifecycle-registry row;
2. stop coordinator and compute-request admission, wake capacity-blocked
   producers, retire parked tickets, drain accepted callbacks, and join the
   request worker while graph-state finalization remains available;
3. stop, drain, and join the graph-state lane;
4. mark the runtime retired, remove route ids/map ownership, and destroy Graph
   state.

There is no post-marker reopen or worker reconstruction. A valid direct Host
close succeeds; an absent Graph with no joinable generation returns
`NotFound`. Graph close never stops process-owned workers, routes, policy
bindings, or unrelated Graph Runs.

Destroying an embedded Host without explicit close first stops external Host
admission and invokes the same Graph close path for each live session. After
all Graphs settle, the composition root invokes idempotent process execution
shutdown exactly once. That shutdown changes the service to `Stopping` and all
remaining rows to `Closing`, drains every installed Run, then stops ready and
route admission, joins physical executors, retires policy invocations and
current/displaced bindings, and publishes `ServiceStopped` only with all
lifecycle/resource counters zero. Direct internal Kernel owners have the same
duty to stop concurrent callers before destruction. The joined boundaries are
also the lifetime fence for each live or staged `GraphModel` diagnostic store;
the store owns no thread or detached lifetime.

`photospiderd` owns daemon session identity, job admission, Host serialization,
and shutdown drainage around this embedded Host contract. Its exact mapping,
lease, socket, and shutdown rules are defined in
`../codebase-structure/IPC-Protocol-v2.md`; they are not graph-kernel ownership.

## Current Error Surface

| Operation | Current public behavior |
| --- | --- |
| initial load, duplicate session | `GraphErrc::InvalidParameter`; existing session remains unchanged |
| execution defaults, unknown route or worker request above eight/conflicting with the fixed pool | `GraphErrc::InvalidParameter`; future defaults remain unchanged |
| initial load, empty path | loads session-local `content.yaml` when present; otherwise intentionally publishes an empty session |
| initial load, explicit missing/unreadable/uncopyable source or session-path failure | `GraphErrc::Io`; no session publication or fallback; already-created filesystem scratch side effects are not rolled back |
| initial load, YAML syntax/representation, non-sequence root, duplicate id, parameter representation/overflow, or node-schema failure | `GraphErrc::InvalidYaml`; no session publication |
| initial load, missing dependency or cycle | exact `GraphErrc::MissingDependency` or `GraphErrc::Cycle`; no session publication |
| initial load, unexpected non-resource failure | `GraphErrc::Unknown`; no session publication |
| initial load, resource exhaustion | `std::bad_alloc` propagates; no session publication |
| reload, missing or closing session | `GraphErrc::NotFound` |
| reload, existing session with empty path | `GraphErrc::InvalidParameter`; prior graph and runtime state remain visible |
| reload, missing/unreadable source | `GraphErrc::Io`; prior graph and runtime state remain visible |
| reload, YAML syntax/representation, non-sequence root, duplicate id, parameter representation/overflow, or node-schema failure | `GraphErrc::InvalidYaml`; prior graph and runtime state remain visible |
| reload, missing dependency or cycle | exact `GraphErrc::MissingDependency` or `GraphErrc::Cycle`; prior graph and runtime state remain visible |
| reload, unexpected non-resource failure | `GraphErrc::Unknown`; prior graph and runtime state remain visible |
| reload, resource exhaustion | `std::bad_alloc` propagates; prior graph and runtime state remain visible |
| save, missing or closing session | `GraphErrc::NotFound` |
| save, existing session with recoverable serialization, YAML emission, or destination preparation/open/write/flush/close failure | `GraphErrc::Io`; graph/runtime/session-owner state remains unchanged; failure before successful open preserves existing destination bytes, while post-open failure may leave created, truncated, or partial output |
| save, existing session with resource exhaustion | `std::bad_alloc` propagates; graph/runtime/session-owner state remains unchanged; destination effects follow the same pre-open versus post-open boundary |
| node replacement, missing/closing session or missing requested node | `GraphErrc::NotFound` |
| node replacement, existing target with malformed input, missing dependency, or cycle | `GraphErrc::InvalidYaml`; previous graph state remains visible |
| forward/backward ROI projection, missing/closing session or missing endpoint | `GraphErrc::NotFound` |
| forward/backward ROI projection, existing endpoints with no valid projection | `GraphErrc::InvalidParameter` |
| execution inspection, missing/closing session or absent route | `GraphErrc::NotFound` |
| execution inspection, existing session with invalid intent | `GraphErrc::InvalidParameter` |
| execution replacement, unknown route, invalid intent, generation exhaustion, or handled lane failure | `GraphErrc::InvalidParameter`; old route binding remains published |
| execution replacement, missing or closing session | `GraphErrc::NotFound` |
| clear or close, missing session | `GraphErrc::NotFound` |

`OperationStatus` exposes an error domain, signed code, stable name, and
diagnostic message. Callers branch on the domain and code, not diagnostic text.
IPC serializes that exact status and rolls its reserved session name back when
Host load fails; it does not introduce a transport-only graph-document
taxonomy.

## Boundaries and Rationale

- Session identity is a copied label; graph and runtime ownership never cross
  the Host boundary.
- Prepare-before-publish load and prepare-before-swap reload keep incomplete
  topology out of the graph map and preserve the prior graph on classified
  failure.
- Detached definitions and the in-memory adapter keep format conversion,
  persistent values, model materialization, and topology publication as
  separate testable stages without changing the public path contract.
- The graph-owner transaction and destination-file side effects are separate:
  save preserves graph state but does not promise atomic destination replace.
- `GraphStateExecutor` serializes visible graph capture, mutation, exact commit
  validation, and publication. The private compute-request lane serializes
  same-Graph compute and execution-route access while long-running operation
  execution uses request-owned snapshots outside graph-state.
- Graph route bindings are copied, resource-neutral values; physical execution
  and policy contexts remain Host/process owned.

These boundaries make publication, mutation, and resource ownership testable
without using shared diagnostic state as a transaction log.
[ADR 0005](../adr/0005-graph-document-ingestion-is-a-classified-transaction.md)
governs the current ingestion contract. `GraphDefinition`,
`InMemoryGraphDocumentAdapter`, the injected graph-document contracts, and the
configured YAML filesystem adapter are now the current persistence boundary.
Issue #61 implements filesystem-adapter injection and format-neutral private
Host composition while preserving the public YAML-named ABI. Issue #62 moves
shared YAML value conversion and cache metadata behind adapter-owned
contracts, so runtime, graph, compute, inspection, and cache declarations are
YAML-neutral. Issue #63 completes the dependency-disabled product profile:
empty and in-memory sessions remain available without yaml-cpp discovery,
while an explicit graph-document or cache-metadata representation operation
uses an unavailable adapter and returns `GraphErrc::Io`.

The accepted
[ADR 0007](../adr/0007-compute-runs-and-process-execution-have-separate-owners.md)
now governs the implemented strong Graph identity/revision, staged compute,
exact revision/generation commit predicate, cooperative Run cancellation,
latest-wins supersession, request-owned realtime `RunGroup`, and separate
compute-request/graph-state lanes. The `ExecutionService`-owned
`RunLifecycleRegistry`, atomic Run-admission/Graph-close fence,
lifecycle-driven close/shutdown cancellation, exact settlement, and
source-private telemetry from issue #76 are current behavior. Issue #75's
pure-C policy ABI, Host-authored frontier, reserved-start admission, and
private execution-route replacement remain current. Public cancellation and
lifecycle telemetry are intentionally not exposed through Host, CLI, or IPC.

## Implementation and Validation Entry Points

- `src/lib/core/image_artifact_codec.hpp`
- `src/lib/adapters/opencv/image_artifact_codec_opencv.*`
- `src/lib/providers/configured_image_artifact_codec.*`
- `src/lib/providers/configured_persistence_adapters.*`
- `src/lib/runtime/kernel.cpp`
- `src/lib/runtime/kernel_io_cache_facade.cpp`
- `src/lib/runtime/kernel_inspection_facade.cpp`
- `src/lib/runtime/kernel_dirty_roi_facade.cpp`
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
- `src/lib/graph/graph_cache_service.*`
- `src/lib/graph/graph_io_service.cpp`
- `src/lib/graph/graph_revision.hpp`
- `src/lib/graph/graph_state_executor.cpp`
- `src/lib/graph/graph_model.cpp`
- `src/lib/compute/compute_commit_policy.hpp`
- `src/lib/compute/compute_service.*`
- `src/lib/compute/run_lifecycle_registry.*`
- `src/lib/compute/execution_lifecycle_telemetry.*`
- `src/lib/compute/realtime_proxy_graph.*`
- `src/lib/runtime/graph_runtime.*`
- `src/lib/host/embedded_host.cpp`
- `src/lib/runtime/kernel_compute.cpp`
- `tests/integration/test_host_adapter.cpp`
- `tests/integration/test_graph_document_errors.cpp`
- `tests/integration/test_graph_document_injection.cpp`
- `tests/integration/dependency_disabled_install_smoke.py`
- `tests/unit/test_graph_document_adapter.cpp`
- `tests/integration/test_ipc_daemon.cpp`
- `tests/unit/test_ipc_protocol.cpp`
- `tests/integration/test_disk_cache_diagnostic_concurrency.cpp`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/unit/test_compute_run.cpp`
