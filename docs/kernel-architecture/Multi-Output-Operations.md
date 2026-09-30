# Named outputs and mixed Result ports

## Scope and ownership

One operation node may declare multiple named outputs. Each output has its own port kind, descriptor or Result schema, input projection, and execution demand. A workflow edge names both the node and output port. The compiler specializes the selected output before execution.

An output can be a numeric `Value` or a structured `Result`. A Result schema can combine typed image slots and primitive fields. Nodes may mix Result image inputs, numeric Value controls, and different output kinds. All image semantics and ownership pass through `ResultRef`; `PlanarImage` is backing inside a Result image slot.

```cpp
struct WorkflowNodeOutput final {
  std::uint64_t source_node = 0;
  std::string source_port = "value";
};

struct WorkflowOutput final {
  std::string name;
  std::uint64_t node_id = 0;
  std::string port = "value";
};
```

The excerpt omits unrelated fields. Public execution places named Value outputs in `ExecutionResult::values` and structured outputs in `ExecutionResult::results`.

## Output selection and dependency

`OperationTraits::outputs` declares available outputs. `select_operation_output` resolves the selected output's traits and index; `ResultProgramQuery::output_index` carries that selection into a structured callback. The selected output contract, query, schema, and input projection define its demand and identity.

The coordinator resolves only inputs in the selected projection. A Result callback may request a numeric Control sample, inspect it in the next stage, and then request the Data image samples selected by that value. The Result relation records consumed Control and Data support, and dirty transpose uses the same relation. Inputs outside the projection are not evaluated for that output.

`ExecutionOptions::enable_joint` enables a registered contract for sharing physical work across eligible requested outputs. Each output keeps its own schema, query, relation, cache identity, and error. Joint execution does not authorize an unrequested output or merge unlike output semantics.

## Identity and resource effects

A structured producer's identity includes the selected output, output contract, captured query, parameters, and ordered input bundle. Physical tile dimensions do not change semantic output identity. Relation traversal, reads, publication payload, continuation state, and retained input owners consume the same execution-root budget.

A selected output failure returns a typed error for the request. An eligible joint group may share a continuation only when its registered contract permits the requested members. Unrequested output work does not start. Result owners remain live until their last owning reference is released.

## Executable fixture

[`examples/unified_result_workflow`](../../examples/unified_result_workflow/README.md) uses public workflow APIs and minimal example operations to select a Result image output and a numeric output from the same node. The focused `test_multi_output_execution` and `test_unified_result_images` tests cover output selection, mixed ports, and image execution. The production operation catalog and image limitations are described in [Image operations](Image-Operations.md).
