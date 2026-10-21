# ADR 0021: Route Named Results Independently

- Status: Accepted
- Implementation boundary (2026-10-20): Named output/schema semantics remain. The PlanStep/instruction and storage-reuse descriptions below are historical. Package 0.39 preserves semantic calls in physical Call records, executes selected Task/Tile closure and publishes independent owning Results without a legacy Plan table; see [multiple outputs](../kernel-architecture/Multi-Output-Operations.md) and the [physical Plan contract](../kernel-architecture/Compiler-and-Execution.md).
- Reader mirror: [Chinese](zh/0021-independent-node-results.zh.md)
- Historical boundary: per-output facet sets of the original decision are superseded; each output carries a resolved structural Result schema. See [Data Model](../kernel-architecture/Data-Model.md), [Structured Results and tensor slots](../kernel-architecture/Global-Results.md), and [Tensor descriptions](../kernel-architecture/Tensor-Semantic-Metadata.md).

## Historical implementation: declarations and static instructions

The static compiler preserves every call and output declaration, including unused
siblings. `ExecutionPlan::steps()` is the complete physical contract table;
`blocks()[i].instructions` contains executable tile work. An unselected pure
output can therefore have a step and no instruction. Input projections delimit
which ports a static mapper may grant; they do not replace explicit read edges.
This refines the demanded-step/task description in the original decision below.

SSA values, block parameters and explicit terminators route control flow. A Run
executes one instruction at a time on the selected CFG path; an instruction
failure stops later work and the aggregate call returns an error. Already
certified observer results remain valid. No partial successful output set is
returned. Shared stages are unique per call activation and are cleared on block
re-entry. Conditions and mandatory effects remain in empty output selections.
Named returns have synthetic records indexed by `plan.output(name).record_index`,
separate from the step declaration index. See
[Multi-output operations](../kernel-architecture/Multi-Output-Operations.md).

## 1. Core summary (TL;DR)

One operation node can produce several named results, each with its own metadata, dependencies and failure path. The compiler and executor identify every result by `ValueRef`, so an unused sibling does not add work to a selected result.

## 2. Mental model and intuition

The compiler resolves every output's Result schema from static input metadata and parameters. Execution then follows only demanded result ports and their relevant input edges. Each demanded result owns its tasks, reads, dependency records, cache identity and outcome.

```text
                       one operation node
                      /        |         \
                  output A  output B   output C
                     |          |          |
                demand A    demand B    unselected
                     |          |       no execution
                 A tasks     B tasks
                     |        /     \
                 publish A  success B  error B
                                         |
                              fail only B dependents
```

`ValueRef` is `(node_id, output_index)`. It routes a result independently of its physical plan-step index. Each result is computed and tracked in member tiles, as defined by [ADR 0023](0023-track-dependencies-between-fixed-tiles.md).

## 3. Formal contracts and APIs

```cpp
struct SemanticOutput {
  std::string key;
  std::shared_ptr<const SchemaTemplate> result_schema;
};

struct WorkflowNodeOutput {
  std::uint64_t source_node = 0;
  std::string source_port = "value";
};

struct WorkflowOutput {
  std::string name;
  std::uint64_t node_id = 0;
  std::string port = "value";
};

struct OperationOutputTraits {
  std::string key = "value";
  std::optional<std::vector<std::uint32_t>> input_indices;
};

struct OperationTraits {
  std::vector<OperationOutputTraits> outputs;
};

class Compiler {
 public:
  Result<ExecutionPlan> plan(const OptimizedGraphIR&,
                             const PlanningOptions&) const;
};

class ExecutionContext {
 public:
  Result<ExecutionResult> execute(const ExecutionPlan&, ExecutionBindings,
                                  const CancellationToken& = {},
                                  const ExecutionOptions& = {});
};
```

The class excerpts omit unrelated members.

These C++ declarations show the routing fields; surrounding types and validation are omitted. `OperationTraits` contains a declaration-ordered output vector.  Every output has a unique key and a separately inferred Result schema. The C operation descriptor has the same ordered model, with at most 64 output records. Purity constrains optional caching and retry; side-effecting operations declare mandatory shared stages.

Workflow edges select an exact producer port by name. A caller-visible `WorkflowOutput` names a selected node and port, then assigns a unique result label. The compiler resolves output names to declaration-order indices and records each result as `ValueRef`. Static inference receives complete input metadata; an output's `input_indices` projection controls its executable input ancestry and callback view. Projected C callback inputs retain their original schema indices.

Planning creates a single-output `PlanStep` for each demanded result. An unselected pure output adds no execution demand. The same node may therefore contribute separate steps when callers request multiple outputs, while each step carries that result's contract and identity. C++ invocation and the C output sink receive the selected declaration-order `output_index`.

A failure of one result's task fails that result and its actual consumers. The public `execute` call still returns one aggregate success or error for its complete requested output set.

## 4. Non-goals and explicit boundaries

- Independent outputs retain separate projections and tile demands. Explicit prepared shared stages may compute joint results once per original node/stage within a Run. Their definition, projection and schema cannot come from the first requester. Mandatory stages and fixed publication bundles may enlarge required work. Dynamic output counts remain unsupported.

## 5. Consequences

Output metadata and result identity include the selected output contract, its resolved Result schema, static parameters and its actual input dependencies. Changing an output role, shape or relevant-input projection changes its identity. Unrelated node ids and graph-wide identity do not substitute for the selected output's content dependencies.

Each published result owns its bytes or references its backing owner. A cache hit receives the result's validated tile records. Cancellation stops the Run's own work. Retained views keep their storage owners alive and consume budget until the final reference retires.
