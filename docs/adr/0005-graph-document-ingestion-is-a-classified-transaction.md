# ADR 0005: Graph Document Ingestion Is a Classified Transaction

## Status

Accepted and implemented for initial graph load and existing-session reload.

## Context

Graph document ingestion crosses public `ps::Host`, the embedded translation
layer, `InteractionService`, `Kernel`, `GraphIOService`, and `GraphModel`.
Those layers previously disagreed about missing paths, parser failures, node
schema failures, and topology validation. Initial load caught complete
document failures as an empty optional, so the Host reported the generic
`InvalidParameter` code. An explicitly supplied missing path could also leave
an older session-local `content.yaml` in place and load it, or publish an empty
session when no local file existed. Reload exposed more detail but did not
share a frozen matrix with initial load.

The public `GraphErrc` numeric values, Host signatures, exact IPC status
serialization, graph-map ownership, scheduler reservation transaction, and
`GraphModel::replace_nodes()` prepare-before-swap mechanism already existed.
The missing decision was how those pieces compose into one document contract.

## Decision

Graph document ingestion uses the following stable classification:

| Condition | Public result |
| --- | --- |
| explicit source missing, unreadable, or uncopyable | `GraphErrc::Io` |
| YAML syntax or representation failure | `GraphErrc::InvalidYaml` |
| non-sequence root, duplicate node id, or node-schema failure | `GraphErrc::InvalidYaml` |
| unresolved graph input | `GraphErrc::MissingDependency` |
| cyclic graph topology | `GraphErrc::Cycle` |
| unexpected non-resource failure | `GraphErrc::Unknown` |
| resource exhaustion | propagate `std::bad_alloc` unchanged |

Operation lifecycle validation retains precedence. Initial load of a duplicate
session is `InvalidParameter`. Reload first resolves the session, so absence is
`NotFound`; an empty reload path for an existing session is
`InvalidParameter`.

An empty initial-load path is an omission sentinel, not an explicit file. If
`<root>/<session>/content.yaml` exists, Kernel loads it. Otherwise Kernel
intentionally publishes an empty graph. Every nonempty path is explicit and
never falls back to local content. Kernel maps source/session filesystem
inspection and copy failures to `Io`.

`GraphIOService` owns document classification because it can distinguish file
access, YAML representation, definition conversion, and topology validation.
It converts GraphDefinition translator/adapter schema-detail failures into
document-schema `InvalidYaml`, while preserving topology `GraphError` values.
`InteractionService` is the defensive final embedded boundary: it preserves
`GraphError`, maps residual file/YAML exceptions, converts other standard and
non-standard failures to `Unknown`, and always rethrows `std::bad_alloc`.

Initial load is a prepare-then-publish transaction. Kernel prepares the runtime
and schedulers and validates the complete document before inserting the
session into its graph map. Failure destroys unpublished ownership and returns
scheduler reservations. Directory and copied-file scratch side effects may
remain because they are not published session ownership.

Reload parses a detached `GraphDefinition` into temporary ownership. The
in-memory adapter stages every private node before its single
`GraphModel::replace_nodes()` call; that call validates dependencies/cycles and
constructs replacement adjacency before swapping nodes and topology. Failure
therefore preserves nodes, adjacency, topology generation, runtime graph state,
and session identity. Successful replacement is the only commit that resets
runtime graph state and advances topology generation.

IPC adds no second taxonomy. It serializes the exact Host status and rolls
back a reserved session name when Host load fails.

## Consequences

- Initial load and reload now agree on every document category while retaining
  their different session-lifecycle results.
- Callers that supplied a missing explicit path now receive `Io` instead of
  stale-content or empty-session success.
- Callers that treated all initial document rejection as `InvalidParameter`
  must branch on the precise stable graph code.
- Resource exhaustion remains distinguishable from recoverable graph-domain
  failures.
- The public API and `GraphErrc` numeric values do not change.
- Filesystem scratch cleanup remains best effort and is not part of graph-map
  atomicity.
- `test_graph_document_errors` is the durable public-Host/direct-model CTest
  owner for the matrix and transaction invariants. Existing scheduler-budget
  and IPC protocol tests retain reservation and registry rollback coverage.

## Rejected Alternatives

### Treat every empty path as an error

Rejected because initial load already uses omission to create a new empty
session or consume session-local content. Reload has no corresponding creation
semantics, so its empty path remains invalid.

### Keep explicit-source fallback for compatibility

Rejected because a caller-selected source must not silently consume unrelated
stale content or report success without consuming the request.

### Normalize only in the public Host adapter

Rejected because direct internal callers and reload LastError would still
observe different exception classes and categories.

### Convert `std::bad_alloc` to `Unknown`

Rejected because memory exhaustion requires process-level policy and status
construction may itself allocate. The language exception remains the reliable
resource-exhaustion channel.
