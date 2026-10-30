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
operation_keys:
  - channel.extract_named_strict
  - channel.extract_named_accelerated_apple_silicon
  - channel.extract_named_accelerated_x86_64
---

# FMT-01B: extract a cell axis by name or role

Inherits [FMT-01](FMT-01_channel_extraction_contract.md) and the exact sample mapping, output schema and layout rules of [FMT-01A](FMT-01A_extract_channel_index.md). B resolves one static selector against its explicit `input_description` at compile time, then applies the same indexed mapping as A. It performs no color conversion or sample scan.

## Interface

In addition to A's parameters, B requires static String `match` (`name` or `role`) and static String `selector`. The selector must be nonempty strict UTF-8 and at most 128 bytes. Matching is exact and case-sensitive, with no trimming, normalization, aliases, wildcard or pattern matching. Name and role are separate namespaces; a selector in one namespace does not match a value in the other.

The required `input_description` parameter must carry a channel axis and a channel table with one entry for each channel position. B scans the selected name or role field once during static preparation and requires exactly one match. Zero or multiple matches return `InvalidArgument` / `InvalidDomain`. A node without `input_description` is rejected, because there are no channel labels to interpret.

## Behavior

After resolving index k, B uses A's coordinate mapping. It preserves batch coordinates, schema identity, tensor key, descriptor dtype and layout where the selected Result supports them. The output carries no description. The selected samples are copied bit-for-bit, without sample-domain validation. Auto/materialize copy; a nonempty forced view fails. Empty requests emit no payload.

## Acceptance

Given an `input_description` with the B/A/R/G table, `match=name, selector=R` and `match=role, selector=red` both resolve to position 2 and return red sample bytes. The selector `name=A` resolves to the coverage-role position but does not validate that its samples lie in `[0,1]`. A duplicated role makes only that role selector ambiguous; a unique name still resolves.
