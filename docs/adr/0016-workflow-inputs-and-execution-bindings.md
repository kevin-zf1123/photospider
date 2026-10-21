# ADR 0016: Bind Workflow Inputs to Each Execution

- Status: Accepted

## Decision

Workflow declarations provide a name and Result schema. Each Run binds an
owning `ResultRef` under that exact name. The compiler fixes input reads,
physical layouts, memory domains, COPY tasks and control flow before execution.
Payload owners remain outside compiler artifacts and identity.

The current operation module and fixed invocation interfaces are ABI 8.
Operations lower to physical tasks and prepare immutable kernels. Invocation
borrows the Plan's fixed input/output views and scratch; payload cannot add
producer tasks or widen a compiled read.

## Declaration and binding contract

`WorkflowDocument` schema 7 stores declarations, operation nodes and named
outputs. Declaration IDs are unique and nonzero. Names are unique,
case-sensitive printable ASCII strings of 1 to 128 bytes with no spaces. A
document contains at most 4096 declarations. Every declaration is bound exactly
once, including declarations unused by the graph.

A declaration fixes all named static Tensor members, dtype, cell shape,
batch/atomic axes and topology. Structural input changes require preparation
and compilation again. The following excerpts identify the binding fields;
the public headers define the complete API:

```cpp
struct WorkflowInputDeclaration final {
  std::uint64_t id = 0;
  std::string name;
  std::shared_ptr<const SchemaTemplate> result_schema = {};
};

struct ExecutionBinding final {
  std::string name;
  ResultRef result;
};

struct ExecutionBindings final {
  std::vector<ExecutionBinding> inputs;
};
```

The vector preserves duplicate names so preflight can reject them. Missing,
extra, duplicate or malformed bindings fail with `InvalidArgument`.
Incompatible Result schemas fail with `TypeMismatch`. The bound payload must
already cover the compiled tile views and match their physical layout and
domain. Layout adaptation requires explicit compiled COPY tasks. Preflight
performs no numerical sample validation or implicit repacking.

`ExecutionContext::prepare` binds a Plan to concrete kernels and devices without
executing payload work. `submit(prepared, bindings, cancellation, options)`
copies the binding snapshot and returns a `RunHandle`; callers must not mutate
the binding container during this copy. Concurrent Runs may share a
`PreparedPlan` while retaining different immutable bindings. Imported Plans
use the same preflight and the frozen registry to which they were linked.

## Results and ordered observation

`WorkflowDocument` outputs select operation-node outputs. Direct
`IRProgram` frontends may return matching SSA values, including inputs and
block parameters. Named returns refer to the physical CFG's return values and
the Plan return signature.

A task's completed tile can satisfy an internal successor before its call
retires. Each `CallGroup` retires in IR call order after its tasks settle.
`ExecutionOptions::on_call_retired` receives an owning `CallRecord` on a
separate serial lane. Its `values` map is keyed by physical value ID and
contains the call's declared exports on success; failed and elided calls have
no values. A callback can retain those Result owners beyond Run or context
lifetime. Callback exceptions close delivery and abort the Run after accepted
work drains. A callback must not join its own Run or destroy its context.

`RunHandle::join` waits for task quiescence and delivery settlement. Successful
named returns appear in `ExecutionResult::results` only after the final
cancellation and source-currentness checks. Earlier retired call exports stay
valid when a later call fails. Observation and final RET ownership are
independent; their Results need not have identical ObjectIds.

## Identity and lifetime

Schemas, traits, normalized selections, fixed reads, layouts, placements and
immutable resource identities participate in the corresponding compiler
identities. Payload addresses and binding owners do not. Each Run retains its
binding snapshot and admitted owners until accepted CPU/GPU work completes,
including cancellation drain. Every member has its own tile grid as described
by [ADR 0023](0023-track-dependencies-between-fixed-tiles.md).

Cancellation stops new issue and drains accepted work; it does not cancel a
different Run using the same Plan. Outputs and owning read windows retain their
backing and resource accounting after context destruction. The last owner
releases those resources. Execution has no cross-Run Result or checkpoint cache.

## Boundaries

- Declarations do not load files or retain caller payload.
- Bindings have no optional/default values or implicit schema conversion.
- Runtime input data is not a compile-time parameter or literal.
- Logical sample authorization is independent of backing storage layout.
- Preparation cannot retry a failed fixed placement on another backend.
- This ADR defines C++ workflow binding and ownership; the operation ABI is
  specified by the public fixed invocation and module headers.
