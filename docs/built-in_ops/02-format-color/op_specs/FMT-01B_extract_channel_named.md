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
verification_status: focused_ctest_installed_consumer_and_example_passed
operation_keys:
  - channel.extract_named_strict
  - channel.extract_named_accelerated_apple_silicon
  - channel.extract_named_accelerated_x86_64
---

# FMT-01B: extract a cell axis by name or role

Inherit [FMT-01](FMT-01_channel_extraction_contract.md) and the exact sample
mapping, output schema, layout and lifecycle rules of [FMT-01A](FMT-01A_extract_channel_index.md).
B resolves one static metadata selector at compile time, then runs the same
indexed Result mapping as A. It performs no color conversion or sample scan.

## Static lookup

In addition to A's parameters, B requires static String `match` (`name` or
`role`) and static String `selector`. The selector must be nonempty strict UTF-8
and at most 128 bytes. Matching is exact and case-sensitive, with no trimming,
normalization, aliases, wildcard or pattern matching. Name and role are separate
namespaces.

The effective TensorDescription must carry a channel axis and a channel table
with one entry for each channel position. B scans the selected name or role field
once during static preparation and requires exactly one match. Zero or multiple
matches return `InvalidArgument` / `InvalidDomain`. A selector in one namespace
does not match a value in the other namespace.

`metadata_mode=raw` is rejected because raw mode does not interpret channel
labels. An undescribed input can use override mode with an explicit channel axis
and table. A selector and resolved index remain static for the compiled plan.

## Output and demand

After resolving index k, B uses A's coordinate mapping. It preserves batch
coordinates and the source schema identity, tensor key, descriptor dtype and
layout where the projected Result supports them. Applicable selected-component
metadata is projected; B does not promise to carry opaque or unrelated
annotations. The selected samples are copied bit-for-bit, without sample-domain
validation.

The Result Need requests exact selected-channel Data support plus the source
Descriptor, with role mask 9 and no Validation or Control role. Dirty propagation
matches A: changes on the selected source channel invalidate mapped output
coordinates, while other channels do not. Result caching is disabled. Empty
output demand returns an empty Result without reading source payload.

## Acceptance fixture and validation

Given the described B/A/R/G table, `match=name, selector=R` and
`match=role, selector=red` both resolve to position 2 and return red sample bytes.
The selector `name=A` resolves to the coverage-role position but does not validate
that its samples lie in `[0,1]`. A duplicated role makes only that role selector
ambiguous; a unique name can still resolve.

The current `test_channel_extraction` integration test passed named selector,
metadata, exact-byte, layout and dependency coverage as part of its Result test
suite. The public workflow validates the index-based C composition; the named
case is covered by the integration test. No current Result performance figure is
claimed. The linked [performance workload](../../../../examples/channel_extraction_performance/README.md)
contains measurements for the former Value/planar path only.
