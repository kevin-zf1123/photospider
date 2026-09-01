# Compute Boundaries

| Owner | Owns | Does not own |
| --- | --- | --- |
| `GraphContext` | copied source document, monotonic revision, snapshot currentness | compiler result, worker, device lane, result registry |
| `Compiler` | validation, semantic/optimized IR, local plan, typed digests/key | runtime allocation, callbacks, queue state, daemon lifecycle |
| `ExecutionRun` | one call's dependencies, ready order, intermediate Values, backend labels, cancellation, diagnostics | shared pools, persistent result, public identity |
| `ExecutionContext` | bounded CPU/GPU callback pools, frozen operations, modeled-byte ledger | source mutation, daemon Jobs, durable state |
| operation/data definition | copied traits/schema and invocation-local callback work | capacity, publication authority, mutation after freeze |

A graph revision is not a Session. A private ExecutionRun is not a Job. A
ready step is not an external scheduler item. Estimated bytes are not a lease
until the ledger admits them. A backend enum is not a native device handle.
Completion is not publication until cancellation/currentness checks pass.

The installed public API exposes compile/plan/execute values. It does not
expose private queue entries, ledger leases, DSO records, or internal callback
owners. The daemon consumes this same public surface.
