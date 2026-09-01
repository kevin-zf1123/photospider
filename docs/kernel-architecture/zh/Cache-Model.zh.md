# Cache 模型

当前 kernel 只暴露一种 cache identity type，不提供 cache service。

`ExecutionPlan::cache_key()` 返回从完整 physical-plan digest 派生的 domain-separated
`PlanCacheKey`。该 key 只是 non-security lookup aid，不授予 validity。Embedding 即使保留
plan，也必须要求 graph revision 仍为 current；missing、malformed 或 stale entry 必须重建。

Active tree 没有 graph-local runtime cache、filesystem cache、serialized Value format、
cache codec、retention policy 或 recovery path。Caller 可丢弃全部 derived plan，再从
`WorkflowDocument` 与 frozen operation set 重新编译。

Plan/result digest 与 timing 是普通 reproducibility diagnostic，不是 signature、durable
identity、receipt 或 release evidence。
