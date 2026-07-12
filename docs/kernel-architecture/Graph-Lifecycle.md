# Graph Lifecycle and Mutation Semantics

This document defines graph/runtime ownership and failure behavior for graph
load, reload, edit, and clear operations.

## Runtime Ownership

`Kernel` owns a map of graph names to `GraphRuntime` instances. Each
`GraphRuntime` owns exactly one `GraphModel`, graph-state executor, event
service, scheduler map, and platform context.

```text
Kernel
  graph name -> GraphRuntime
                  -> GraphModel
```

Graph and runtime should be treated as a one-to-one ownership unit.

For embedded Host concurrency, a close admission gate marks one session closing
before backend removal. New compute and scheduler admissions fail, while close
waits accepted synchronous calls and caller-visible async status publication.
Kernel then submits runtime stop through the same per-graph
`GraphStateExecutor` as compute and scheduler information/replacement before it
erases the map entry. This ordering keeps both the runtime and its scheduler
owners alive for every already-admitted operation. Concurrent close callers
claim the close marker one at a time: after an earlier attempt completes, each
waiter rechecks and performs its own existence/close attempt. A runtime-stop
failure retains the runtime and diagnostic state, clears the marker, and
reopens admission so inspection or a later close retry remains possible;
`NotFound` is reserved for an actually absent map entry.

## Daemon-Owned Session Identity

`photospiderd` owns one embedded `ps::Host`; clients never own its
`GraphRuntime` lifetimes. The version 1 router preserves the caller's safe
`session_name` in `GraphLoadRequest.session`, so the existing
`<root>/<session>` and cache-directory semantics remain unchanged. It returns a
separate 128-bit opaque IPC session id and keeps a private bidirectional mapping
to the exact Host-returned `GraphSessionId`.

Loading is transactional across the registry and Host boundary: reserve an
opaque id, call Host, and publish the opaque, exact Host-id, and display-name
indexes only after Host success. A Host exception removes the reservation; a
publication failure removes it before a best-effort compensating Host close.
The Host contract requires that a thrown load leave no newly published
session. The embedded Kernel/Host satisfies this contract by preallocating
result identity before publication and committing it with a `noexcept` move.
`graph.list` reconciles committed mappings with `Host::list_graphs()` and
reports an invariant error instead of exposing an untracked Host name. A
client disconnect never calls `close_graph`; another client can list and
inspect the daemon-owned session.

`graph.close` uses the same daemon lifecycle gate as compute admission. It
atomically marks the mapping closing, rejects every new session-scoped Host or
compute admission with Graph `not_found`, and waits for already admitted Host
calls plus every queued/running job for that session. Status/result/release for
an already accepted job remain available because they are job-scoped and do
not enter Host. Only after those counts reach zero does the daemon acquire its
Host mutex and invoke `Host::close_graph()`. Success removes the mapping; Host
`NotFound` removes a stale mapping while preserving the failure; any other Host
failure atomically reopens admission and retains the mapping.

The public Host contract does not promise thread safety. The daemon therefore
uses one dedicated mutex around every Host call, including read-only listing
and inspection. Protocol validation plus `daemon.ping`/`daemon.version` do not
take that mutex, and socket IO never occurs while it is held. Signal shutdown
stops session, compute, and snapshot admission plus new output leases; closes
the listener; wakes and joins client workers; drains accepted jobs; and joins
the sole compute worker. It then removes terminal job ownership, clears stable
collection snapshots, stops output publication, waits for active delivery
leases to release or expire, and identity-cleans/closes the OutputStore before
attempting to close active Host sessions. Only after registry and Host cleanup
does it remove the exact socket while the persistent lifecycle lock remains
held, release that lock, and destroy Host state. The lock file remains for
stable cross-process synchronization. The complete wire and socket contract is
maintained in
`docs/codebase-structure/IPC-Protocol-v1.md`.

## New Graph Load

Loading a new graph should create a new `GraphRuntime` and expose it through
`Kernel` only after YAML validation succeeds.

If any node in the YAML is invalid, missing required fields, or creates a cycle,
the load must return an error and must not expose a partially loaded graph.

The desired behavior is:

```text
parse YAML -> validate all nodes/topology -> rebuild adjacency -> create/commit runtime
                                      \-> on failure: return error, expose none
```

Partially keeping valid nodes before the failing node is not desired.

## Existing Graph Reload

Reloading an existing graph is more sensitive because it operates on a graph
name that may already be visible. The desired direction is to avoid half-cleared
or partially rebuilt model state on reload failure.

Chosen behavior: failed reload preserves the previous graph. Reload validates
the replacement model and rebuilds topology adjacency before committing it to
the visible `GraphModel`. A successful replacement advances topology generation
even when node ids are reused, so runtime-owned mirrors such as
`RealtimeProxyGraph` reset stale per-node state before the next compute.

## Node YAML Replacement

Node YAML replacement should preserve the old node and graph if validation
fails.

At minimum, replacement must parse the new node and keep the old node on parse
or field validation failure. Topology validation should also happen before
commit so replacement cannot introduce cycles or broken dependencies.

Replacement validates the candidate topology before commit. If parsing,
dependency validation, or cycle validation fails, the previous node and graph
remain visible.

## GraphModel Clear

`GraphModel::clear()` should reset model-level runtime state, not only erase
`nodes`.

Clear should reset:

- node map
- topology adjacency index
- topology generation
- timing results
- accumulated IO time
- skip-save state
- other per-run model state that could affect a subsequent load or compute

This makes reload and clear behavior easier to reason about and avoids stale
metadata attached to an empty graph. Runtime-owned state keyed by node id must
treat the generation change as an invalidation boundary rather than preserving
entries for reused ids.

## Error Surface

Graph load, reload, and edit failures are visible to frontends through public
`ps::Host` status and error values. In embedded mode, the Host adapter maps
internal `Kernel` and `InteractionService` failure diagnostics into that public
surface. Frontends neither call those internal facades nor infer failure from a
partially changed graph.

In daemon mode, the same `OperationStatus` preserves its domain-complete
`OperationErrorDomain`, signed code, stable name, and diagnostic message.
Host failures use the `graph` domain with explicit `GraphErrc` number/name
pairs. Framing, envelope, and parameter errors remain in the `protocol`
domain; local socket failures remain `OperationErrorDomain::Transport`.
Diagnostic text is not a branching contract, and no transport failure is
rewritten as graph IO.
