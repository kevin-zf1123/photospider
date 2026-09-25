---
spec_schema_version: 1
id: FMT-01C
parent_id: FMT-01
function: split_channels
kind: composite_workflow
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: d49d1840
inspection_commit: d49d1840
---

# FMT-01C: split all channels through graph expansion

Runtime update: the CPU registrations and public split helper are implemented.
See the [implementation and runnable workflow](../../../kernel-architecture/Channel-and-Color-Operations.md#fmt-01-channel-extraction)
for current storage behavior, validation commands and the measured performance scope.
Proposed is retained as the specification decision status.


Inherit [FMT-01](FMT-01_channel_extraction_contract.md). This member is an explicit
compile-time authoring composition of [FMT-01A](FMT-01A_extract_channel_index.md),
not a registered multi-output operation, native dispatcher or special tensor
collection schema. It exposes one independent workflow reference per channel.

## Interface and expansion

The proposed explicit authoring helper `split_channels` receives a graph, the input tensor edge and its known static
descriptor/effective metadata, the common FMT-01 parameters, and one selected
NUM profile (`strict`, `accelerated_apple_silicon`, `accelerated_x86_64`). The
helper default profile is strict. Resolve the channel axis a once and let
C=input.shape[a]. Expand exactly C single-output A nodes, in index order,
with the same input edge and resolved common arguments; node k uses index=k.
Verify any caller-supplied description against actual inference before execution.

Return C authoring handles named c0,c1,...,c(C-1), mapped to those nodes' `values`
ports. Names are zero-based decimal without leading zeros and independent of
channel labels, avoiding duplicate-name and rename ambiguity. Preserve labels
in component metadata. These are compile-time edge handles, not runtime sample
arrays and not automatically exported workflow roots. No matrix is allocated
by expansion and no input producer executes to determine C.

For keepdims=false, each output removes the source channel axis; for true, each
has extent 1 there. Rank-one input therefore requires true for every output.
All outputs retain input dtype, their own projected component description and
the selected profile's exact bits. C=1 is valid and generates one extraction.
Changing axis/count/metadata requires re-expansion/reinference as applicable.

## Graph limits and failure behavior

Reuse the collision-aware ID-allocation rules of the existing
[NUM authoring support](../../../../include/photospider/numeric/workflow_authoring.hpp),
including forward references. Stage the expansion and handles before appending;
on failure leave the caller's graph unchanged. Do not silently omit channels,
reuse occupied IDs or fall back to an opaque Whole split.

The [current compiler](../../../../src/lib/compiler/compiler.cpp) accepts at most
65536 graph nodes and 4096 exported outputs. Check available graph slots before
allocating C nodes; a large tensor legal for A may exceed C's expansion capacity.
Returning handles does not consume all root-output slots: callers choose which
ones to export or connect. Exporting more roots than the host supports fails
under its ordinary limits. This composition does not change the native 64-output
operation limit or promise unlimited channel count. Metadata/capacity limits may
fail earlier. The conceptual expansion adds no new dtype or operation ABI.

## Runtime demand, storage and cost

Each consumed handle is an ordinary A output. Requests, exact source-channel
mapping, dirty propagation, raw/override behavior and view/materialize policy are
independent. An unused A node has no runtime payload demand. There is no joint
bundle validation requiring unrequested channels or siblings; normal preflight
still checks the declared graph. A failure does not revoke already completed
independent observations; ordinary fail-fast collection follows the host API.

Expansion costs O(C) nodes/handles plus copied parameter/descriptor bytes. Each
active output k incurs A's costs for its requested element count Nk and fragments
Fk: total logical copied sample volume sum(Nk*d), with mapping/copy work
O(sum(Nk*r+Fk*r)). Charge actual shared retained owners once per applicable
accounting domain and all real temporary allocations. Each materialized image
reserves its full output virtual layout and provides the requested page union;
logical copied bytes are not its backed-page capacity. Views share the source
image owner, including all produced pages retained by that owner. Source read sharing or
common metadata storage is permitted but not a semantic or performance promise.
Requesting all channels can require all their samples; requesting only c2 must
not make c0/c1/... active. Inherit A's resource/cancellation/error rules.

## Public composition fixture and oracle

For shape [1,2,3], axis=2 and input `[[[1,10,100],[2,20,200]]]`, expansion exposes:

| Handle | Referenced A index | Shape with keepdims=false | Values |
| --- | --- | --- | --- |
| c0 | 0 | [1,2] | [1,2] |
| c1 | 1 | [1,2] | [10,20] |
| c2 | 2 | [1,2] | [100,200] |

Conceptually connect c1 to a numeric multiply or export only c2. An output
request at c2 coordinate (0,1) reads source (0,1,2) only. Changing channel 1
does not dirty c2. Returning all three and assembling them in source order must
recover the original numeric samples bit-for-bit; until FMT-02 exists, use an
independent host interleaving oracle rather than claim a runnable merge node.

Implementation acceptance must supply a real graph-expansion helper and public
compile/execute workflow, inspect the generated A nodes/parameters and compare
each output with an independent integer-coordinate/byte oracle. Exercise C=1,
C>64 within graph limits, budget/node/root-output limits, ID collisions and
forward references, no partial graph mutation, subset output requests, differing
ROIs, offset/edge tiles, metadata propagation, overrides, lifetime and cancellation.
No implementation or runtime tests were run for this specification.

The family support matrix and performance plan apply to the generated A nodes.
C has no separate pixel kernel or numerical optimization: the expansion algorithm
above is its reference, and permitted runtime copy/view optimizations belong to A.
Its independent acceptance additionally checks graph construction and rollback.
