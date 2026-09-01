# Cache Model

The current kernel exposes one cache identity type and no cache service.

`ExecutionPlan::cache_key()` returns a domain-separated `PlanCacheKey` derived
from the complete physical-plan digest. The key is a non-security lookup aid.
It does not grant validity: an embedding that keeps plans must still require a
current graph revision and must rebuild on a missing, malformed, or stale
entry.

The active tree has no graph-local runtime cache, filesystem cache, serialized
Value format, cache codec, retention policy, or recovery path. A caller may
discard every derived plan and compile again from `WorkflowDocument` plus the
frozen operation set.

Every schema-valid Float64 parameter contributes the exact bits present in the
copied IEEE-754 binary64 value. In particular, `+0.0` and `-0.0` produce
different semantic/optimized/plan digests and therefore different
`PlanCacheKey` values; a cache cannot reuse one signed-zero workflow for the
other.

Plan/result digests and timing are ordinary reproducibility diagnostics. They
are not signatures, durable identities, receipts, or release evidence.
