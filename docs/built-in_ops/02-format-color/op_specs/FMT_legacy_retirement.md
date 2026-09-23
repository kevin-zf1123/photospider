---
spec_schema_version: 1
id: FMT-legacy-retirement
kind: implementation_retirement_record
category: 02-format-color
status: Retired
implementation_status: legacy_removed
inspection_commit: 1b403fb9
package_version: 0.20.0
---

# Format/color legacy implementation retirement

The maintainer requested removal of the existing format/color implementations
before rebuilding them from the clarified specs. Package 0.20.0 removes these
13 default-registry operations and their dedicated source implementations:

- channel.extract, channel.merge, channel.swizzle;
- alpha.associate, alpha.unassociate;
- color.assign, color.rgb_to_xyz, color.xyz_to_rgb, color.xyz_to_lab,
  color.lab_to_xyz, color.rgb_to_ycbcr420;
- numeric.cast and numeric.encode_range, historically stored in 01-numeric but
  assigned to format/encoding responsibilities in the new specification.

The 02-format-color plugin implementation directory is removed. Build/registration
lists, dedicated old tests and directly dependent example paths are updated.
No compatibility aliases, failure-only registered stubs or substitute FMT
implementations are provided. Lookup, direct invocation and workflow compilation
of an old key return NotFound through the normal registry/compiler path.
The [public regression](../../../../tests/integration/test_format_color_retirement.cpp)
also runs a remaining numeric.add_strict workflow and is used by the installed
consumer. Kernel inferred-image rejection remains covered by an independent
fixture in the planar workflow test, without looking up an old channel operation.

Shared color_array/semantic/profile data contracts and mathematical helpers used
by maintained NUM/CRV operations remain. Closed operation-inference enums and
channel-index parsing also remain public generic ABI facilities; they do not
execute the deleted pixel algorithms. This change removes operations, not all
color-related data vocabulary or non-FMT operators. Public ABI/schema/trait
versions do not change; the package version identifies the breaking registry
surface change. Prior compiled workflows using the removed keys must be rebuilt
without them. No new spec implementation is implied.

NUM shaper construction no longer uses numeric.cast for Float32 zero/one literals;
it uses the existing one-element sequence generator. The LUT baking Float32 source-cast scenario and its oracle cases are removed
until a replacement format conversion is implemented. Neither path introduces
a replacement cast API.

FMT-01..08 specifications retain their target semantics and inspection history;
source descriptions inspected at 1b403fb9 are historical after this removal.
FMT-07 remains retired. [The FMT-09..18 review](FMT-09-18_scope_review.md) lists
the completed active-family scope and remaining implementation prerequisites. New operator
specifications remain Proposed/not implemented. Runtime validation performed
for this deletion is reported separately from future spec conformance.
