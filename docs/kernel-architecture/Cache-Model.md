# Cache model

## Scope and ownership

The kernel keeps separate identities for compiler stages, plan cache keys, immutable input snapshots, completed structured Result reuse, checkpoint and block state, and native backend input-copy caches. Cache membership grants reuse eligibility only; active Results, snapshots, Tensor backing, and read windows retain their own owners and resource leases after an entry is cleared or evicted. Each Run executes a static plan, and the VM performs completed-result, checkpoint and block lookups directly for the instruction it is executing, with its own optional cache-work quota.

## Identity domains

| Identity | Domain tag | Contents | Changes when |
| --- | --- | --- | --- |
| IR content digest | `validated-ssa-ir-v26` | Program form and canonical SSA program: declarations, calls, values, blocks, terminators, parameters, traits, resource identities, schemas | Any program change, including renumbered calls or values |
| Physical plan digest | `static-tile-program-v27` | Actual input IR digest, execution mode, tile extent, steps and placements, output requests, and every block's instructions, grants, dependencies and terminator | Any instruction, grant or request change, or renumbering |
| Plan cache key | `plan-cache-key-v20` | Domain-separated wrapper of the plan digest | Same as the plan digest |
| Step structured template | `photospider.dependency-static.v4` | Tile extent, execution mode, backend, operation key, output index, traits, resource identities, parameters, input schemas | Contract, parameter, resource or input-schema change; not renumbering |
| Fixed-grant content key | `photospider.fixed-grant-cache.v2` | Step template, backend, member, tile, publication samples, descriptor-grant ports, granted input schemas and sample bytes | Any input byte or grant change; not renumbering |
| Fixed block-input proof | `photospider.fixed-block-inputs.v2` | Block contract plus the same grant content (without member and tile) | Same as the content key |
| Saved operation contract | `artifact-static-operation-contract-v2` | Registered traits and validator/report references of one operation key | Operation contract change |

The first four identities are compiler identities. They include program identifiers, so renumbering calls or values changes them even when the computation is equivalent; they are reproducibility aids, not security identities. The template and content identities contain no physical node, step or plan identifier. Two equivalent calls at different steps, a renamed or renumbered plan, or a plan reloaded from `.psxp` therefore share completed-result, checkpoint and block entries when their fixed inputs have the same content. Dependency records still belong to the current plan; reuse never imports another plan's records.

Descriptor-only grants enter the content key by port index. Editing a saved plan so that a descriptor grant names a different port changes the key, and the edited plan cannot reuse the original plan's cached value.

## Immutable Tensor snapshots

```cpp
#include "photospider/data/input_snapshot.hpp"

namespace ps {
Result<InputSnapshot> import_snapshot(const InputSnapshotStore& store,
                                      const Tensor& value) {
  return store.import_tensor(value);
}
}
```

`InputSnapshotStore` retains immutable rank-1 through rank-8 blocks for supported built-in numeric Tensor types. Import preserves every bit pattern and checks only the Tensor's descriptor and coverage; it does not interpret sample values. The store bounds block count, actual payload bytes across versions, and directory metadata separately. Caller-owned Tensors and snapshot metadata are outside the retained-payload byte limit.

Patches require matching type and shape and replace one exact nonempty Footprint. The store copies intersecting blocks and preserves earlier versions. Reads and patches accept cancellation and sample bounds. A cancelled read may have partially written the caller's destination; only success certifies the complete result.

`content_identity(region)` hashes dtype, shape, requested coordinates, and exact sample bits with canonical framing. Block geometry, origin, stride, and allocation do not affect identity. Signed zeros and distinct NaN payloads remain distinct. Snapshot/session identifiers describe provenance; deterministic operations cannot derive sample values or dependencies from identifier spelling.

## Purity and cacheability

The VM tracks two separate properties for every instruction and value:

- **Pure**: the operation is deterministic and side-effect-free, the instruction has no shared-stage reads and is not a shared stage, the attempt is not fallback-tainted, every payload input comes from a workflow input or a pure producer, the activation's control evidence is pure, and no payload input is a block parameter.
- **Cacheable**: pure, plus the operation's `cacheable` trait, cacheable producers and cacheable control evidence.

Checkpoint and block services require purity only. A pure instruction can therefore use them even when its own operation or an ancestor sets `cacheable = false`. Completed-result lookup and retention require cacheability. Block parameters, shared closures and fallback paths bypass all optional reuse; this is conservative, because a block parameter can carry a different value on each loop iteration.

## Completed Result reuse

A completed-result lookup computes the instruction's fixed-grant content key, reads up to eight candidates for that key, skips candidates with another backend, and rebinds a match with a new ObjectId and the current producer scope. The rebound Result is validated like a computed publication, including the output payload bound, and shares the cached physical backing. The Run installs it with dependency evidence rebuilt from the current plan's grants. Cache metadata, key hashing, sample reads and retained payload are charged to their Root and cache limits. Optional work exhaustion, metadata exhaustion, or insufficient retention capacity skips reuse or retention and lets computation proceed. A hit creates no `OperationTiming` record and adds no `computed_elements`.

Each execute call owns its immutable Result binding snapshot until callbacks retire. Direct and demand execution check plan currentness; graph replacement or destruction makes a borrowed-graph plan stale.

`ExecutionContextConfig::result_cache_bytes` defaults to 0, which disables completed-result retention and block reuse. `maximum_dependency_cache_metadata` defaults to 65536 proof units and is bounded to 1..1048576. `ExecutionOptions::maximum_dependency_cache_work` is a separate per-Run optional quota, defaults to 1048576, and zero disables completed-result, checkpoint and block reuse for that Run. Key construction, sample hashing, rebinding, checkpoint witnesses and block keys use it alongside mandatory Root work. Exhaustion skips reuse or retention and permits computation to continue; it does not refund Root work already consumed.

## Checkpoints and block state

A checkpoint scope is the fixed-grant content key with a `/checkpoint` suffix, so equivalent calls share checkpoint state independently of their physical step. Restoring a checkpoint restores state content only; the current instruction keeps its own fixed grants and evidence, and an old witness never replaces them. `ExecutionContextConfig::maximum_result_checkpoint_scopes` bounds live scopes, and each scope keeps at most 64 entries. Checkpoints work without a positive `result_cache_bytes`.

Block state reuse binds a block contract and the fixed block-input proof with the block kind, range, mode and incoming state. A GPU block enters the cache only when the compute callback actually dispatched native work during that block; the backend and the fixed-input proof are part of the key, so a GPU path without a native computation proof is never reused. See [Global Results](Global-Results.md#cross-output-block-state-sharing).

## Limits and current entry points

- Native GPU results remain isolated from CPU-exact result keys, because execution mode and backend are part of both the template and the content key. A fallback-tainted result does not enter any reuse cache, and approximate native results do not replace exact CPU results.
- `ResultCacheStatistics::native_retained_bytes` counts unique native allocations retained by the cache, including both retained GPU outputs and retained GPU block states. It is not a block-only capacity.
- Native input-copy reuse is separate from completed Result computation reuse. It can reuse transport for granted Result tensor backing, but it does not make an upstream producer computation cacheable.
- LRU eviction removes an entry from future lookup eligibility; callers holding a Result or read window keep its backing. `clear()` advances the cache epoch, so work from an earlier epoch cannot refill the cleared cache.

Current production image operation availability is listed in [Image operations](Image-Operations.md). Snapshot and generic cache behavior is exercised by `test_input_snapshot`, `test_generic_result_cache`, and `test_computed_scalar`. Content-identity reuse across edited descriptor grants and checkpoint sharing across physical steps are exercised by `test_static_compiler`. Result capture and owner release are covered by `test_result_image_contracts` and `test_result_objects`.

Run-shared stages are executed once per call, stage and block activation by the plan itself, independently of optional caches. They expose no checkpoint or block services and do not hold active state across Runs. Instructions with shared-stage reads bypass completed-result caching; see [Global Results](Global-Results.md#completed-result-cache). Snapshot block geometry is independent of plan tile extent.
