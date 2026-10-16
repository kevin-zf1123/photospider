# Named outputs and Result ports

## Scope and ownership

A node may expose several named Result outputs. Each output has a static Tensor-only schema, an input projection, a visibility policy and its own demand. A workflow edge names the node and output port. `ExecutionResult::results` holds named Result owners; there is no separate numeric port kind.

`WorkflowNodeOutput` identifies `source_node` and `source_port`; `WorkflowOutput` identifies the public `name`, `node_id` and `port`. The default port name is `value`. A Result may itself contain multiple named static Tensor members. These are distinct from multiple node outputs.

## Output selection and dependency

`OperationTraits::outputs` declares outputs. `select_operation_output` selects their traits; `ResultProgramQuery::output_index` identifies the output in a callback. Tile Needs separately select each Result member's `tensor_slot` and local indices.

Only prepared input projections are executable for an output. Shared-stage declarations have their own prepared projections and dependencies. A stage is identified within its Run by the original node and stage, independent of the first requesting output or tile. An algorithm needing joint computation can publish a multi-member stage Result, from which output actors publish authorized tile views. A pure unrequested stage does not start; mandatory stages follow the Run's effect-root rules.

All sample reads, including control values, require ordinary payload Needs. Consumers inherit frozen evidence from the shared commits they consume and add their local Needs. Inherited evidence does not authorize reading the stage's inputs.

## Identity and resource effects

Output contracts, stage declarations, parameters, resources and input schemas enter prepared operation identity. An ordinary actor's key includes its selected output, member and tile. Active shared state is never reused across Runs. Optional completed-result, checkpoint and block caches have separate purity and actor eligibility rules; see [Global Results](Global-Results.md).

Every tile commits payload, coverage and dependency evidence transactionally. `CompleteBundle` uses a prepared fixed member scope and drives all required siblings; `StablePrefix` follows explicit member-local tile order. A bundle can enlarge the work triggered by one request. Default storage layout does not create a bundle.

## Executable fixture

Current behavior registrations are defined in `cmake/KernelTests.cmake`; retained regressions include shared-stage and member-tile execution cases. These registrations are not evidence of a particular run. Test lifetime follows [Testing and Validation](../development/Testing-and-Validation.md).
