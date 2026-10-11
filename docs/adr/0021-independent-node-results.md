# ADR 0021: Route Named Results Independently

- Status: Accepted
- Reader mirror: [Chinese](zh/0021-independent-node-results.zh.md)

## 1. Core summary (TL;DR)

One operation node can produce several named results, each with its own metadata, dependencies and failure path. The compiler and executor identify every result by `ValueRef`, so an unused sibling does not add work to a selected result.

## 2. Mental model and intuition

The compiler resolves all output descriptors and facets from static input metadata and parameters. Execution then follows only demanded result ports and their relevant input edges. Each demanded result owns its tasks, reads, dependency records, cache identity and outcome.

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

`ValueRef` is `(node_id, output_index)`. It routes a result independently of its physical plan-step index. Each result is computed and tracked in tiles or as one Whole unit, as defined by [ADR 0023](0023-track-dependencies-between-fixed-tiles.md).

## 3. Formal contracts and APIs

```cpp
struct SemanticOutput {
  std::string key;
  ValueDescriptor descriptor;
  std::vector<ValueFacet> facets;
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

These C++ declarations show the routing fields; surrounding types and validation are omitted. `OperationTraits` contains a declaration-ordered output vector.  Every output has a unique key and a separately inferred descriptor and facet set. The C operation descriptor has the same ordered model, with at most 64 output records. Multiple outputs require deterministic, side-effect-free behavior. Singleton operations explicitly declare `value`.

Workflow edges select an exact producer port by name. A caller-visible `WorkflowOutput` names a selected node and port, then assigns a unique result label. The compiler resolves output names to declaration-order indices and records each result as `ValueRef`. Static inference receives complete input metadata; an output's `input_indices` projection controls its executable input ancestry and callback view. Projected C callback inputs retain their original schema indices.

Planning creates a single-output `PlanStep` for each demanded result. An unselected pure output adds no execution demand. The same node may therefore contribute separate steps when callers request multiple outputs, while each step carries that result's contract and identity. C++ invocation and the C output sink receive the selected declaration-order `output_index`.

A failure of one result's task fails that result and its actual consumers. The public `execute` call still returns one aggregate success or error for its complete requested output set.

## 4. Non-goals and explicit boundaries

- Multiple outputs do not imply shared evaluation, shared cache keys or one callback that publishes several Values. Each demanded result follows its declared execution contract.
- An unselected pure sibling does not execute solely because another result from its node is selected. Side-effecting operations remain singleton roots under their declared contract.
- The coordinator does not group several results or observations into one shared callback.
- Dynamic output counts, late-added output ports and automatic channel pruning are outside the fixed declaration-ordered output table.

## 5. Consequences

Output metadata and result identity include the selected output contract, its resolved descriptor and facets, static parameters and its actual input dependencies. Changing an output role, shape or relevant-input projection changes its identity. Unrelated node ids and graph-wide identity do not substitute for the selected output's content dependencies.

Each published result owns its bytes or references its backing owner. A cache hit receives the result's validated tile records. Cancellation stops the Run's own work. Retained views keep their storage owners alive and consume budget until the final reference retires.
