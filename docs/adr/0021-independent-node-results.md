# ADR 0021: Route Named Results and Join Ready Atomic Work

- Status: Accepted
- Reader mirror: [Chinese](zh/0021-independent-node-results.zh.md)

## 1. Core summary (TL;DR)

One operation node can produce several named results, each with its own metadata, dependencies, observation contract and failure path. The compiler and executor identify every result by `ValueRef`, so an unused sibling does not add work to a selected result. An optional CPU joint session can advance compatible, already-ready Atomic observations together while preserving an outcome for each member.

## 2. Mental model and intuition

The compiler resolves all output descriptors and facets from static input metadata and parameters. Execution then follows only demanded result ports and their relevant input edges. If joint execution is enabled, the coordinator can combine ready members that share an operation instance and snapshot; each member still owns its reads, state, certificate, cache identity and terminal outcome.

```text
                       one operation node
                      /        |         \
                  output A  output B   output C
                     |          |          |
                demand A    demand B    unselected
                     |          |       no execution
                  Atomic     Atomic
                     \          /
                 ready CPU joint poll
                  /                  \
              Need A                 B outcome
                |                 /           \
       resolve A inputs      success B       error B
                |                 |              |
          another poll         publish B   fail only B dependents
```

`ValueRef` is `(node_id, output_index)`. It routes a result independently of its physical plan-step index. `RequestRecord` is a terminal whole-query observation, not a sample-level dependency. `Atomic` observations represent restriction-stable values or errors, usually one generic sample or one complete image pixel.

## 3. Formal contracts and APIs

```cpp
struct SemanticOutput {
  std::string key;
  ValueDescriptor descriptor;
  std::vector<ValueFacet> facets;
  bool effective_atomic = true;
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
  ObservationKind observation_kind = ObservationKind::Atomic;
  FailureDelivery failure_delivery = FailureDelivery::RequestFailureOnly;
  std::uint32_t atomic_trailing_axes = 0;
};

struct OperationTraits {
  std::vector<OperationOutputTraits> outputs;
  std::uint32_t joint_contract = 0;
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

The compiler computes each output's effective observation kind from its declared kind and the ancestry of its relevant inputs. An Atomic result remains Atomic only when its selected ancestors are Atomic. `atomic_trailing_axes` can group complete trailing dimensions into one generic observation tuple; image pixels retain all logical channels. A consumer's Atomic requirement is checked against only the selected result and relevant ancestry, so an unrelated sibling does not change that result's legality.

Planning creates a single-output `PlanStep` for each demanded result. An unselected pure output adds no execution demand. The same node may therefore contribute separate steps when callers request multiple outputs, while each step carries that result's contract and identity. C++ invocation and the C output sink receive the selected declaration-order `output_index`.

`FailureDelivery::RequestFailureOnly` reports one failure for an observation and does not permit batching multiple Atomic observations. `FailureDelivery::PerAtomOutcome` is available to staged Atomic operations that implement a complete outcome protocol. A joint poll returns one `Need`, success or member-local failure for every supplied member. Missing, duplicate or unknown outcomes are protocol errors. The public `execute` call still returns one aggregate success or error for its complete requested output set.

`OperationTraits::joint_contract` is an optional CPU execution capability; singleton dependency callbacks remain required. Contract 1 groups distinct Atomic outputs from the same node. Contract 2 groups distinct `AtomKey` observations and can include multiple coordinates from one output. The coordinator groups only known ready demands that share the operation, static parameters, input snapshot and CPU backend. It does not wait for future requests or join groups across Runs. `ExecutionOptions::enable_joint` controls grouping and defaults to true.

Each joint member has an independent waiter and terminal state. Member-level errors affect that member and its actual consumers. The coordinator charges one shared continuation and scratch reservation, plus each member's adapter, output and workspace requirements; backing storage is charged once per owner. The joint session releases borrowed member services when each poll returns. A contract-1 joint admission or execution failure that can be retried releases joint reservations and evaluates unfinished members through singleton callbacks. Protocol, cancellation, stale and group/run-scoped failures remain failures. Contract 2 treats an enclosing group failure as terminal because its members form one coordinate batch.

An in-tree example operation, `image.split_horizontal`, declares `full`, `left` and `right` outputs. It requires an image and an integer `split_x` strictly inside the input width. `full` retains the full shape; `left` has width `split_x`; `right` has width `input_width - split_x`. Each output preserves the image facet and requests only its source coordinates. The callback can publish an owner-backed view of the requested source fragment. The maintained example source currently documents that its typed image binding still needs planar migration, so it is an API illustration rather than proof of installed planar workflow execution.

## 4. Non-goals and explicit boundaries

- Multiple outputs do not imply shared evaluation, shared cache keys or one callback that publishes several Values. Each demanded result follows its declared execution contract.
- An unselected pure sibling does not execute solely because another result from its node is selected. Side-effecting operations remain singleton roots under their declared contract.
- A fetch union is not a substitute for per-output Data, Control, Validation and Descriptor associations or their certificates.
- Joint execution is an optional physical optimization. It does not promise one poll, parallel member execution, or a common failure for independent members.
- Joint grouping is CPU-only and limited to compatible staged Atomic contracts. It does not combine RequestRecord observations or unrelated nodes.
- Dynamic output counts, late-added output ports and automatic channel pruning are outside the fixed declaration-ordered output table.

## 5. Consequences

Output metadata and result identity include the selected output contract, its resolved descriptor and facets, static parameters and its actual input dependencies. Changing an output role, shape, relevant-input projection or observation contract changes its identity. Unrelated node ids and graph-wide identity do not substitute for the selected output's content dependencies.

Each published result owns its bytes or references its backing owner. A cache hit, flight join or joint member receives result-specific validated evidence. Cancellation of one waiter does not cancel other active waiters; shared work stops when no member has a live waiter. Retained views keep their storage owners alive and consume budget until the final reference retires.

Joint execution adds member bookkeeping and shared state and can require more peak memory than evaluating one output at a time. When reservation or admission does not fit, the coordinator falls back to singleton execution where the contract permits it. Callers can disable grouping, inspect `joint_groups`, `joint_polls` and `joint_fallbacks`, and budget for retained named outputs. Structural protocol errors, cancellation and stale work are surfaced rather than retried as independent computations.
