# Named outputs and Result ports

## Scope and ownership

A node may expose several named Result outputs. Each output has a static Tensor-only schema, an input projection, a visibility policy and its own demand. A workflow edge names the node and output port. `ExecutionResult::results` holds named Result owners; there is no separate numeric port kind.

`WorkflowNodeOutput` identifies `source_node` and `source_port`; `WorkflowOutput` identifies the public `name`, `node_id` and `port`. The default port name is `value`. A Result may itself contain multiple named static Tensor members. These are distinct from multiple node outputs.

## Output selection and dependency

`OperationTraits::outputs` declares outputs. `select_operation_output` selects their traits; `ResultStaticQuery::output_index` and `ResultProgramQuery::output_index` identify the output in a mapper and a callback. Mapped reads separately select each Result member's `tensor_slot` and local tile indices.

The plan separates declarations from executable work. `ExecutionPlan::steps()` contains one `PlanStep` for every output of every call, whether or not anything reads it. Instructions exist only for the tiles that a root (a named return in the selected request, a branch condition, an edge argument, a side-effecting call or a mandatory stage) reaches through mapped reads. An unselected sibling output therefore keeps its step declaration and has no instructions, and filtering with `select_outputs` can remove the instructions of one sibling while keeping the other's.

Only prepared input projections are readable for an output. Shared-stage declarations have their own prepared projections and dependencies. All outputs of one call share its stages: the planner canonicalizes a stage unit to the call's first step, so the outputs of one call reach the same stage instructions and the VM keeps one stage state per call, stage and block activation. An algorithm needing joint computation can publish a multi-member stage Result, from which output tile instructions publish authorized tile views. A pure stage that no mapper names produces no instructions; mandatory stages are compiled as effect roots.

All sample reads, including control values, require compiled payload grants. Consumers inherit frozen evidence from the shared commits they consume and add the evidence of their own grants. Inherited evidence does not authorize reading the stage's inputs. Visibility to a consumer follows certification: an independent output tile becomes readable once its instruction is certified, while a `CompleteBundle`, `StablePrefix` or complete-validation output becomes readable only after its certification closure has executed.

## Identity and resource effects

Output contracts, stage declarations, parameters, resources and input schemas enter prepared operation identity. An ordinary tile instruction's key includes its selected output, member and tile. Active shared state is never reused across Runs. Optional completed-result, checkpoint and block caches have separate purity and instruction eligibility rules; see [Global Results](Global-Results.md).

Every tile commits payload, coverage and dependency evidence transactionally. `CompleteBundle` uses a prepared fixed member scope, and the planner compiles all required sibling tiles into the reader's closure; `StablePrefix` follows explicit member-local tile order, and a read of tile `k` depends on tiles `0..k`. A bundle can enlarge the work compiled for one request. A named return is projected from the call's certified value; the projected return object and an object delivered earlier through `result_publication` need not share an `object_id`, so consumers compare coverage, content and owner semantics instead. Default storage layout does not create a bundle.

## Executable fixture

Current behavior registrations are defined in `cmake/KernelTests.cmake`; retained regressions include shared-stage and member-tile execution cases. These registrations are not evidence of a particular run. Test lifetime follows [Testing and Validation](../development/Testing-and-Validation.md).
