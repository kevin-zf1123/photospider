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

Plan/result digests and timing are ordinary reproducibility diagnostics. They
are not signatures, durable identities, receipts, or release evidence.
