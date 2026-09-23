---
spec_schema_version: 1
id: FMT-01B
parent_id: FMT-01
function: extract_channel_named
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
proposed_operation_keys:
  - channel.extract_named_strict
  - channel.extract_named_accelerated_apple_silicon
  - channel.extract_named_accelerated_x86_64
repository_branch: ops-specs
repository_commit: d49d1840
inspection_commit: d49d1840
---

# FMT-01B: extract a channel by exact name or role

Runtime update: the CPU registrations and public split helper are implemented.
See the [implementation and runnable workflow](../../../kernel-architecture/Channel-and-Color-Operations.md#fmt-01-channel-extraction)
for current storage behavior, validation commands and the measured performance scope.
Proposed is retained as the specification decision status.


Inherit [FMT-01](FMT-01_channel_extraction_contract.md) and the same-dtype,
bit-exact extraction mapping of [FMT-01A](FMT-01A_extract_channel_index.md).
B changes compile-time selector resolution; it adds no color conversion or
sample-domain validation. Its proposed primitive identity preserves the selected
metadata dependency explicitly; no current registered entry is claimed.
Registration targets the default operation registry. Inherit the family's support
matrix, reference/optimized stages and performance acceptance plan; the metadata
lookup algorithm and its additional cost/fixtures are specialized below.

## Ports and static selector

Input `input`, output `values`, and shape/dtype support match A. In addition to
the family parameters, require static String `match` (`name` or `role`) and
static String `selector` (nonempty strict UTF-8, at most 128 bytes). Neither has
a guessed default. They occupy separate namespaces: name R does not silently
mean role red, and role coverage does not fall back to a channel named A.
Matching is exact and case-sensitive, with no trimming, Unicode normalization,
pattern matching, hierarchy parsing, aliases or automatic first-match selection.

The effective metadata supplies an ordered channel table aligned with the
resolved channel axis, with exactly input.shape[axis] entries. A selector reads
the corresponding name/role field in each entry, resolves exactly one position
k, and then applies A's mapping with that constant index. Zero matches or more
than one match is InvalidArgument/InvalidDomain at compile/preflight. Unrelated
duplicate roles do not make a different uniquely matched selector ambiguous;
the shared codec's structural validity requirements still apply.

An untagged source needs an explicit metadata override with axis and channel
table. In respect mode, an axis assertion must match the attached designation.
In override mode, resolve against the effective replacement description, not
the old channel table. Raw named lookup is undefined because it removes the
interpretation needed to resolve a name; reject it before payload reads. Use
A for positional raw extraction. The source and its other consumers are unchanged.

## Output metadata, dependencies and resources

Output shape follows keepdims exactly as A. Preserve the selected component's
name/role/unit and applicable source interpretation through the family projector,
without marking it as a complete color or asserting sample validity. Resolving
role coverage only determines which samples to copy; it does not assert [0,1].

Descriptor demand includes the effective channel axis and selector field across
the table because uniqueness can depend on any entry. Output descriptor demand
also includes fields retained for the chosen component. A metadata edit can
change the selected position or introduce ambiguity and requires reinference.
After resolution, sample Data/dirty mappings are exactly A's, with no runtime
Control input, spatial halo, whole-source read or complete-color validation.

The descriptor scan costs O(C+L), where C is channel count and L is the total
number of selector-field bytes examined; use one linear scan and constant
match/count scratch, apart from the owned validated metadata representation.
Check admitted metadata sizes before scanning and cancellation/work at most
every 1024 entries/bytes of long scans. Pixel work, layout availability, owner
retention and result-cache policy inherit A/FMT-01; do not rescan names per pixel.

## Acceptance and minimal workflow

Use source [1,2,4] with table:

| Position | Name | Role | Two stored values |
| --- | --- | --- | --- |
| 0 | B | blue | 30,31 |
| 1 | A | coverage | .25,.5 |
| 2 | R | red | 10,11 |
| 3 | G | green | 20,21 |

`match=name, selector=R` and `match=role, selector=red` both resolve k=2 and
produce shape [1,2], values [10,11] with keepdims=false. Selecting name A
returns [.25,.5]. Name r fails unless explicitly present. If another channel
also has role red, role selection fails; unique name R remains resolvable.
An override moving name R to position 0 yields [30,31] only for that invocation;
a second consumer without override still yields [10,11]. No samples are converted.

Conceptual workflow: `bound described tensor -> FMT-01B -> named result`.
The implementation must provide actual public compile/execute calls and a
separate oracle that scans a fixture table and enumerates raw source bytes.
Acceptance includes every A mapping/layout case plus missing/table-length errors,
duplicate matches, explicit namespaces, UTF-8 names, invocation-local overrides,
metadata-only replanning, unchanged other consumers, and no color sample-domain
validation triggered by name/role lookup. Verify match failure reads no payload.
No implementation or runtime tests were run for this specification.
