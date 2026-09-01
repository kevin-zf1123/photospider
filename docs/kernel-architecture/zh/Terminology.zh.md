# Kernel 术语

| 术语 | Canonical 含义 |
| --- | --- |
| `WorkflowDocument` | Caller-owned、format-neutral source graph。 |
| `GraphContext` | 一个 copied source document 加 monotonic revision/currentness；绝不是 daemon Session。 |
| `GraphSnapshot` | Coherent source/revision capture；replace/destruction 后 currentness 变为 false。 |
| `SemanticGraphIR` | 带 copied trait 与 inferred output descriptor 的 deterministic topological node。 |
| `OptimizedGraphIR` | 独立的 semantics-equivalent stage；当前为 conservative no-op。 |
| `ExecutionPlan` | Dependency-ordered local step、backend label、estimated bytes 与 output mapping。 |
| `ExecutionContext` | Bounded CPU/GPU callback pool、frozen operation 与 byte ledger 的 owner。 |
| `ExecutionRun` | 一次同步 execute 调用的 private state；绝不是 daemon Job 或 public identity。 |
| `Value` | Immutable dense descriptor、Region、strided layout、bounded facet 与 owned shared bytes。 |
| `Region` | Value shape 的 rank-general logical subset；绝不是 buffer/storage object。 |
| operation trait | Copied compiler-visible input count、effect、backend/fallback、type/shape/Region rule 与 estimated bytes。 |
| operation/data-definition DSO | Startup-configured trusted in-process extension。 |
| digest/cache key | Non-security reproducibility 或 disposable lookup identity。 |
| cancellation | 防止 late result publication 的 cooperative observation。 |
| fallback | Optional GPU unavailable/failure 后，由 trait 允许的 CPU attempt。 |

`SessionId`、`JobId`、Job status/result release 与 daemon process lifecycle 只属于
`photospider-daemon`。Network service、tenant isolation、durable work、worker process、policy
plugin、native-code security product、durable result identity 与 release evidence 已删除或
不在范围内。
