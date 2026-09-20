---
spec_schema_version: 1
id: NUM-03B
parent_id: NUM-03
function: broadcast
operation_family: numeric.broadcast
proposed_operation_keys:
  - numeric.broadcast_strict
  - numeric.broadcast_accelerated_apple_silicon
  - numeric.broadcast_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-impl
repository_commit: current working tree
---

# NUM-03B: broadcast

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

The maintainer selected a separate broadcast operator that expands an array to
a target shape through an explicit axis mapping. Constant scalar filling has
its own NUM-03A specification. This draft records only that selected boundary;
shape/mapping constraints, storage, metadata, demand and error requirements.
Follow the common three-version NUM rule; copying must preserve
the strict logical output bits.

The selected axis map has one entry for each input axis, specifying a distinct
output axis. Arbitrary axis order is allowed, so a map can combine permutation
and expansion. It is injective and never merges two input axes into one output
axis. For example, input shape `[3]` maps to output `[2,3,4]` with axis map `[1]`.

Each mapped input extent must equal its target extent or be one. Length-one
axes expand; equal non-singleton axes preserve their coordinate. Periodic tiling,
merging/dropping axes and implicit alignment are excluded. The target inherits
constant's static positive rank-1..8 shape, total elements <=2^40, all-four-dtype
bit preservation, and static view/dense layout with view default.

## Interface and exact mapping

One input `input` is a UInt8/Int64/Float32/Float64 array of rank 1..8. One output
`values` has the requested static shape and exactly the input dtype. Both ranks
and shapes are known at compilation; output rank cannot be smaller than input
rank because the map is injective and includes every input axis.

Required static String parameters are `shape`, `axis_map`, and `layout`.
Shape uses NUM-03A's canonical comma-separated positive decimal extents. Axis
map uses one comma-separated nonnegative output-axis index per input axis,
without duplicates, negative indices, omitted axes or implicit inference. Layout
is view/dense; the constructor writes view by default. Direct nodes provide all
parameters. For input shape I, output shape O and map a, output coordinate o
reads source coordinate s:

```text
s[j] = 0            if I[j] == 1
s[j] = o[a[j]]      otherwise
values[o] = bitcopy(input[s])
```

Unmapped output axes replicate the same source. Static validation checks every
`0<=a[j]<rank(O)`, injectivity and `I[j]==1 || I[j]==O[a[j]]` before reading data.
No multiplication/expansion arithmetic is applied to the stored values. Generic
NaN payloads, infinities, signed zeros and integer extrema are preserved exactly.

The output has empty facets. Input metadata and actually read typed fragments
retain the existing recognized semantic validators; copying cannot strip an
invalid input's semantic obligations to make it valid. For example, image-v2
source reads require the relevant complete-pixel channel validation support.
Any additional required validation region must be declared and retained in the
witness. Opaque facets are not interpreted as a new image/array type and are
not copied onto the reshaped generic output.

## Whole demand, dirty mapping and representation

Every nonempty request reads and validates the complete input, then publishes
the complete selected representation before projection. Any source change
invalidates all output observations; returned dirty coverage is restricted to
the consumer's actual Region. Empty reads no payload. Parameters and complete
input metadata still validate before execution. Full typed validation may fail
because of data outside the mapped coordinates of a partial consumer request.

View requires one original Value covering the full input demand. Mapped
non-singleton axes retain its byte strides; expanded singleton/unmapped axes
have zero stride. Source offset, unaligned and negative/zero strides remain
valid. The output retains the source owner with no new payload, and does not
expand a zero-stride source before the callback. If no covering input Value
exists, explicit View returns InvalidArgument/InvalidDomain ViewUnavailable with
Domain/Run scope before the callback. It does not synthesize a multi-owner output.

Dense may collect multiple input owners, then packs all target elements in
row-major order. Partial consumers still retain full output capacity. Neither
mode changes raw bits, dtype or generic output identity. Outputs have empty
facets; retained upstream/resource owners remain accounted. Published owners
survive context destruction, and failure/cancellation releases unpublished work.

## Work, resources and numerical profiles

For N total outputs and rank r<=8, dense copying uses O(N*r) coordinate work
and N*b output bytes. View uses O(r) layout work and retains actual source
backing, independently of the target logical count. Complete input collection
and typed validation are additional work. A valid view does not reserve N*b.

All three keys preserve identical logical element bits. Permuted/view strides
follow the same selected layout contract; no approximate math, NaN conversion
or numeric tolerance is needed. SIMD is a byte-copy optimization and must not
change payload bits or read bounds. Unsupported platform keys return
BackendUnavailable instead of silently selecting another operator.

Use host workers, allocator/admission and work/stage budgets. Reserve dense
bytes before allocation, and account referenced owners and metadata separately
from new payload. Do not reserve M*b merely to construct a valid view. Poll
cancellation before reads, between mapping/refinement blocks, at least every
4096 dense elements and before publishing. Sticky host failures cannot turn
into success via a different layout or fallback. Cache keys include operation/
profile, shape, axis map, layout and exact witnessed input data/metadata. Cache-off
does not alter ownership, values or validation. No disk backing or private pool
is required and no RSS bound is claimed.

## Errors and acceptance

Malformed/missing shape, layout or axis map and total shape size above 2^40
use InvalidArgument at compile/direct preflight. Input dtype/rank, map length,
incompatible extents or a source dimension that would need dropping use
TypeMismatch. Duplicate/out-of-range map indices are InvalidArgument. Preserve
recognized semantic validation, upstream identity, cancellation/stale and
ResourceExhausted code/reason/scope. Generic floating NaN/Inf is not an error.

| Test | Independent expected behavior |
| --- | --- |
| B01 | `[10,20,30]` shape `[3]` -> `[2,3,4]`, map `[1]`: output `[i,j,k]` equals source[j]; only 3 unique source values are needed for the full result |
| B02 | Input `[2,3]` -> output `[3,4,2]`, map `[2,0]`: output `[j,k,i]` equals input[i,j] |
| B03 | Input `[1,3]` -> `[4,3]` succeeds; `[2,3]` -> `[4,3]` under the same map fails without tiling |
| B04 | Duplicate map, map length/range errors, rank shrink, empty/invalid shapes and product limits reject before data reads |
| B05 | Four dtypes, arbitrary NaN payloads, infinities, signed zeros and integer extrema match bitwise across view/dense and platform profiles |
| B06 | Negative/zero source strides, offset origin, unaligned storage retain a single source owner; multiple owners reject View |
| B07 | Nonzero/disjoint Q still requires complete input and typed validation |
| B08 | Any changed source element invalidates all selected output observations |
| B09 | Empty request reads nothing; warm/cache-off, cancellation and failed allocation/work paths retain correct witnesses and release unpublished owners |
| B10 | View retains source bytes after original/context destruction; final owner releases them; dense has correct independent packed payload |
| B11 | Large view versus insufficient dense budget, fragmented source/set budget exhaustion, and unsupported accelerated-platform behavior |

The public target workflow binds a small indexed integer matrix, creates the
selected broadcast key with shape/map/layout, compiles and requests full plus
nonzero/disjoint outputs. Check logical indices, strides/owners, actual source
reads and dirty support. Use the explicit coordinate formula as an independent
oracle. The current implementation's runnable target and command are recorded
in the workflow README and implementation table.

## Current implementation status

The three keys are registered with per-node metadata specialization. The
specializer validates `shape`, the explicit injective `axis_map`, extent
compatibility, layout and source dtype, then produces a compact static
dependency mapping. `broadcast_node` in `photospider/numeric/arrays.hpp` is the
public authoring helper and emits these parameters without implicit alignment.

The implementation preserves source owners for view output, uses zero strides
for expanded axes and inherited strides for mapped axes, and packs only the
requested region for dense output. Local Clang execution checked `[3]` to
`[2,3,4]` with `axis_map=[1]`, exact sparse source support and dirty
replication, a `[274877906944,3]` view backed by three Int64 samples, and a
structured consumer reading the giant view through an 8-byte result view.
`examples/numeric_workflow/mappings.cpp` compares compact mapped certificates
with explicit rows over 64 queries, 8 dirty subsets and 3 roles, including
large replication and bounded materialization.

The following dated implementation/acceptance record predates Whole.
Manual acceptance on 2026-09-14 passed local AppleClang 21 strict/Apple profiles
and Ubuntu WSL Clang 18 strict/x86 profiles, plus an installed public consumer.
It includes negative-stride unaligned permutation, independent owners, image Data
versus complete-pixel Validation, invalid unselected alpha, bit patterns, Empty,
collector cancellation, StageLimit, capacity/schema/profile failures, final source
owner release after context destruction, and warm
cache reuse after an unobserved edit versus refresh after an observed edit.
Dense execution gathers at most 32 bytes into charged continuation scratch,
then uses the selected memcpy/NEON/AVX2 copy path with exact bounded tails.
Diagnostics identify the concrete ISA path. Independent mapped/row set comparison
includes bounded canonical box expansion.
No performance benchmark or new integration-test registration is included. This
does not change the Proposed status of this specification.

- [NUM-03 category](../core.md).
- [NUM-03A constant](NUM-03A_constant.md).
- [Existing shape/region traits](../../../../include/photospider/plugin/operation_registry.hpp).

Current Whole validation and timing: [array Whole](../arrays-whole.md).
