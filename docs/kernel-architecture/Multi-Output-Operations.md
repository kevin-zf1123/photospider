# Named outputs and Result ports

## Scope and ownership

A node can expose several named Result outputs. Each output has a static
Tensor-only schema, an input projection and its own selection. Workflow edges
use `WorkflowNodeOutput{node, port}`; public outputs use
`WorkflowOutput{name, node, port}`. The default port name is `value`.
Multiple node outputs are distinct from the named Tensor members within one
Result. `ExecutionResult::results` owns the selected named returns.

## Fixed lowering and selection

`OperationTraits::outputs` declares the output contracts.
`OperationLowering::requests` supplies one normalized TileRequest per output.
`OperationProgramBuilder` creates fixed tasks and explicit input reads, schema
reads, output writes and private values. Reads identify the input port, member,
tile and physical layout; they cannot expand during invocation.

The Plan retains every IR call and its output value declarations. Executable
tasks are the closure of selected returns, branch conditions, edge arguments
and mandatory effects. An unselected pure output may therefore have no tasks.
`ExecutionPlan::select_outputs` restricts existing coverage and rebuilds
dependencies, call membership, liveness and resource bounds without invoking
operation lowering again. All writes of a retained task remain part of that
task's fixed invocation.

A call's outputs can share producer tasks and private intermediate values.
Those dependencies are ordinary physical edges. A shared computation runs once
per block activation, and its storage is independent in another Run or loop
activation. Input projections constrain which ports an output may read;
explicit reads still determine the actual payload closure.

## Visibility and identity

Publication policies contribute to the compiled closure. `CompleteBundle`
can require sibling member tiles; `StablePrefix` can require an earlier
member-local prefix. The kernel preserves those static requirements without
runtime dependency discovery. Storage layout alone does not create a bundle.

A successful task completion makes its tiles available to internal successors.
Public call exports are delivered through `on_call_retired` only when the
CallGroup settles and reaches the IR retirement head. Failed and elided calls
export no values. Named RET values are projected after successful control-flow
completion and delivery settlement. Call exports and final returns own their
Results independently and need not share an ObjectId.

Output contracts, schemas, parameters, resources, fixed reads/writes and
physical selections participate in compiler identity. Active intermediate
state belongs to its Run. There is no completed-Result, checkpoint or block
cache across Runs. See [Global Results](Global-Results.md) and
[the execution contract](Compiler-and-Execution.md).

## Validation

Kernel behavior checks cover independent output selection, shared physical
producers, member tile closures, ordered retirement and final ownership.
Registrations in `cmake/KernelTests.cmake` identify the retained checks; a
registration is not evidence that a particular run passed. Bundled numerical
checkers follow [Testing and Validation](../development/Testing-and-Validation.md).
