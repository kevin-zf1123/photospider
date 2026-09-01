# Cache 模型

当前 kernel 只暴露一种 cache identity type，不提供 cache service。

`ExecutionPlan::cache_key()` 返回从完整 physical-plan digest 派生的 domain-separated
`PlanCacheKey`。该 key 只是 non-security lookup aid，不授予 validity。Embedding 即使保留
plan，也必须要求 graph revision 仍为 current；missing、malformed 或 stale entry 必须重建。

Active tree 没有 graph-local runtime cache、filesystem cache、serialized Value format、
cache codec、retention policy 或 recovery path。Caller 可丢弃全部 derived plan，再从
`WorkflowDocument` 与 frozen operation set 重新编译。

每个通过 schema validation 的 Float64 parameter 都会贡献 copied IEEE-754 binary64 value
中存在的精确 bit。特别是，`+0.0` 与 `-0.0` 会产生不同的 semantic/optimized/plan digest，
进而产生不同的 `PlanCacheKey`；cache 不能为一个 signed-zero workflow 复用另一个的 plan。

Plan/result digest 与 timing 是普通 reproducibility diagnostic，不是 signature、durable
identity、receipt 或 release evidence。
