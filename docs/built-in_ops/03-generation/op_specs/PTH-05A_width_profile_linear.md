---
spec_schema_version: 1
id: PTH-05A
parent_id: PTH-05
function: width_profile_linear
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.width_profile_linear_v1_strict
  - path.width_profile_linear_v1_accelerated_apple_silicon
  - path.width_profile_linear_v1_accelerated_x86_64
---

# PTH-05A: width profile linear

Inherits [PTH-05](PTH-05_width_profiles_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.width_profile_linear_v1_strict`, `path.width_profile_linear_v1_accelerated_apple_silicon`, `path.width_profile_linear_v1_accelerated_x86_64`.

s[N], associated source lengths/domain selection,stops[K],widths[K]; values[N].

### Parameters and domain

Explicit pathset_normalized_arclength/subpath_normalized_arclength/subpath_arclength_px; K>=2, increasing stops, finite nonnegative widths. Normalized stops cover0..1; px stops cover source length. Source indices and mappings use path_width and path_source_map from the shared geometry contract.

Ordered inputs: `s`, the explicitly selected source length, `stops`, `widths`. `domain` determines whether the exact rational query is normalized. The source-selection workflow supplies the corresponding length; it is not discovered through ObjectId. The linear interpolation is rounded once.

## Mathematics

Choose correct source whole/subpath length L and source s. Normalize exactly as u=s/L (L0 uses0) or use u=s. Hits copy/convert width; otherwise interpolate full rational formula then RN. No hidden extrapolation/clamping. Actual domain endpoints and zero-length association behavior must match the formal binding.

## Behavior

Support class: Regional. Each query depends on the complete profile controls.

## Acceptance

Source lengths100/300 distinguish the three domains; trimmed25..75 retains widths4..8 for original2..10 profile; finite nonnegative widths are assumed.
