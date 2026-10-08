# ADR 0014: Compiler Stages and Compatibility Versions Stay Distinct

- Status: Accepted

## 1. Core Summary (TL;DR)
The compiler builds distinct immutable semantic, optimized, and physical-plan values from a versioned workflow document. Document schema, plugin ABI, package API, planner behavior, and daemon IPC versions describe different contracts. Keeping them separate avoids implying compatibility where none is promised.

## 2. Mental Model & Intuition

```text
WorkflowDocument -> SemanticGraphIR -> OptimizedGraphIR -> ExecutionPlan
       |                    |                  |                |
 document schema       semantic digest    optimized digest   plan digest/key
```

Each transition validates its input and returns a complete value or an error. The plan targets local capabilities; it does not contain a remote device handle. A frozen registry identity protects stage use at runtime but is not canonical semantic content.

## 3. Formal Contracts & APIs

```cpp
// Excerpt; compiler and value types are declared by the public headers.
class Compiler {
 public:
  Result<SemanticGraphIR> analyze(const GraphSnapshot&, ResourceBindings) const;
  Result<OptimizedGraphIR> optimize(const SemanticGraphIR&) const;
  Result<ExecutionPlan> plan(const OptimizedGraphIR&, const PlanningOptions&) const;
};

struct SemanticGraphDigest { std::string value; };
struct OptimizedGraphDigest { std::string value; };
struct ExecutionPlanDigest { std::string value; };
struct PlanCacheKey { std::string value; };
```

The current WorkflowDocument schema is 5, OperationTraits version is 24, package version is 0.32.0, and the operation-plugin C table is Result ABI 2. Digests identify canonical content in separate domains; the plan cache key identifies disposable derived lookup. The compiler encodes the closed document fields, normalized parameters, copied operation traits, static preparation identity fields, output/input demands, optimizer identity, and target capability facts in their corresponding stage domains.

Each stage also carries a private weak identity for the exact frozen operation registry; optimizer, planner, and executor reject a foreign-registry stage even if operation keys match. That runtime identity is excluded from canonical digests and serialized data. Runtime addresses, allocation IDs, timings, cancellation, queue state, and daemon identifiers are excluded. Float parameter identity preserves copied binary64 bits in fixed little-endian order, including signed zero and non-finite payloads accepted by schema validation.

Thus sign-sensitive operations do not collide across semantic, optimized, plan, or cache identities. Static prepared state and library pointers remain outside digest bytes; their resolved metadata and declared workspace bounds participate where specified.

The independent axes include WorkflowDocument schema, OperationTraits, operation-plugin C ABI, semantic IR, optimizer rules, physical planner, installed package/API, and daemon IPC. WorkflowDocument schema 5, OperationTraits 24, package 0.32.0, and Result ABI 2 identify their current contracts; changing one does not assert compatibility in another.

Internal IR and plan objects are in-memory contracts, not daemon wire formats. Their registry identity prevents use with an incompatible frozen operation set. A matching cache key never replaces plan validation and stale checks. Embedding-provided cache hits are revalidated; malformed or stale entries become misses. See [Compiler and execution](../kernel-architecture/Compiler-and-Execution.md) for current identity fields and stage validation.

## 4. Non-Goals & Explicit Boundaries
- Digests are not cryptographic signatures, authorization, attestations, or durable identities.
- The kernel does not migrate persisted documents; consumers own document storage and migration policy.
- Internal compiler stages have no cross-release reader compatibility promise.

## 5. Consequences
Consumers must version each public boundary they actually expose and rebuild disposable plans when source, traits, rules, or target capabilities change. A stale or invalid cache entry is rejected or treated as a miss. Package API changes can be breaking even when workflow schema is unchanged.
