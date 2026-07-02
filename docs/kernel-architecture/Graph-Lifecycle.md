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

Graph load, reload, and edit failures should be visible through kernel and
interaction-facing APIs. Frontends should not need to infer failure from a
partially changed graph.
