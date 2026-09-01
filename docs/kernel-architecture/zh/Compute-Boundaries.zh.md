# Compute 边界

| Owner | 拥有 | 不拥有 |
| --- | --- | --- |
| `GraphContext` | copied source document、monotonic revision、snapshot currentness | compiler result、worker、device lane、result registry |
| `Compiler` | validation、semantic/optimized IR、local plan、typed digest/key | runtime allocation、callback、queue state、daemon lifecycle |
| `ExecutionRun` | 单次调用的 dependency、ready order、intermediate Value、backend label、cancellation、diagnostic | shared pool、persistent result、public identity |
| `ExecutionContext` | bounded CPU/GPU callback pool、frozen operation、modeled-byte ledger | source mutation、daemon Job、durable state |
| operation/data definition | copied trait/schema 与 invocation-local callback work | capacity、publication authority、freeze 后 mutation |

Graph revision 不是 Session。Private ExecutionRun 不是 Job。Ready step 不是 external
scheduler item。Estimated bytes 只有在 ledger admission 后才是 lease。Backend enum 不是
native device handle。Completion 只有通过 cancellation/currentness 检查后才是 publication。

Installed public API 暴露 compile/plan/execute value，不暴露 private queue entry、ledger
lease、DSO record 或 internal callback owner。Daemon 使用相同 public surface。
