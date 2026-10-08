# ADR 0016: Bind Workflow Inputs to Each Execution

- Status: Accepted

## 1. Core Summary (TL;DR)

Workflow declarations provide a name and Result schema; each execution binds an owning `ResultRef` under that name. This keeps tensor shape and layout in the Result schema while keeping payload owners out of compiler identity. Every operation executes through the staged Result continuation protocol.

## 2. Mental Model & Intuition

```text
WorkflowDocument                         each execute call
  names + Result schemas                       Result bindings
          |                                         |
          v                                         v
       analyze -> optimize -> plan           schema/name preflight
                                      +----------+-----------+
                                      |          |           |
                                  Result A   Result B   Result C
                                      |          |           |
                                      +----------+-----------+
                                                 v
                                      Result continuations
                                                 |
                                   certified outputs or failure
```

A declaration fixes the input name and Result schema expected at a workflow port. Each `execute` call supplies a `ResultRef` with the corresponding schema. The Result retains its tensor backing and resource owners while the execution or any returned view needs them; concurrent calls can reuse one plan with different immutable bindings.

## 3. Formal Contracts & APIs

```cpp
struct WorkflowInputDeclaration final {
  std::uint64_t id = 0;
  std::string name;
  std::shared_ptr<const SchemaTemplate> result_schema = {};
};

struct ExecutionBinding final {
  std::string name;
  ResultRef result = {};
};

struct ExecutionOptions final {
  std::function<Status(ValueRef, const ResultRef&)> result_publication = {};
};

struct ExecutionResult final {
  ResourceMap<ResultRef> results = {};
};
```

These excerpts show the public fields relevant to binding and publication. The current `WorkflowDocument` schema is 5. It stores declarations, operation nodes, and named outputs. Declaration ids are unique and nonzero; names are unique, exact, case-sensitive printable ASCII strings of 1 to 128 bytes with no spaces. A document contains at most 4096 declarations, and each declaration is bound exactly once, including declarations unused by the graph.

A declaration's `result_schema` supplies typed tensor slots, fields, domain, and semantic metadata. Compiler input metadata carries that schema; its Value descriptor and facets stay empty. Tensor shape, batch axes, facets, and physical layout come from each `ResultTensorSpec`. Value may back a typed tensor internally, but it is not an operation input or output contract. Static schema changes require recompilation. Input payload bytes, pointers, and allocation addresses are excluded from semantic and physical-plan identity; schema, traits, normalized output demand, and other compile-time facts determine those identities.

`ExecutionBinding` matches one declaration by name and owns a Result reference. The executor validates names as a multiset, so duplicate bindings remain visible and fail validation rather than being silently overwritten. It validates Result schema compatibility before scheduling operation work. The Run retains its binding snapshot and any admitted input owners until callbacks retire. `ExecutionContext::execute` returns named Results in `ExecutionResult::results` after callbacks retire and cancellation and graph-currentness checks allow completion.

`ExecutionOptions::result_publication` is a per-call observer for certified Result prefixes. Each caller has an independent serialized notification stream. A caller may retain the owning `ResultRef` beyond the callback. A prefix is not complete execution success: observer failure stops the Run, while previously certified prefixes remain valid. Callback exceptions are converted to typed execution failures; cancellation and callback retirement follow the Run's existing ordering.

## 4. Non-Goals & Explicit Boundaries

- Workflow declarations do not load files or retain caller payloads.
- Bindings have no optional/default values and receive no implicit schema conversion, resize, or layout repacking.
- Runtime input data does not become compile-time parameters or literal nodes.
- A workflow output selects an operation-node output; direct input passthrough is not a workflow output form.
- Result tensor backing layout does not change logical sample authorization. Planar storage is not interleaved storage.
- The Result C operation table remains ABI 2. This ADR describes the C++ workflow binding contract, not a separate C++ Value operation protocol.

## 5. Consequences

Malformed declarations and missing, extra, duplicate, or invalid binding names fail with `InvalidArgument`; an incompatible Result schema fails with `TypeMismatch`. These checks happen before operation callbacks. A caller can reuse a compiled plan with independent Result bindings, but it must not mutate its binding container while `execute` copies it.

Cancellation is cooperative. The executor waits for entered callbacks to retire before releasing borrowed phases and owners. A frozen Result producer may remain owned by the context while a live peer still needs it; one waiter's cancellation does not cancel shared work required by another waiter. Input Results retain their own backing owners, while Root-accounted working state, outputs, and transfers remain charged until their last owner retires.
