---
spec_schema_version: 1
id: PTH-12F
status: Accepted
implementation_status: implemented
registration_status: registered
operation_keys:
  - path.boolean_grid_float64_v1_strict
---

# PTH-12F: Explicit-grid Boolean with Float64 output

Inherits [PTH-12](PTH-12_boolean_offset_contract.md),
[GEN-common](GEN_common_contract.md) and [PTH geometry](PTH_geometry_contract.md). This
is a distinct registered versioned member. Same
version/profile means bit-identical public output; no backend substitution changes that
contract.

Inputs and quantized Boolean semantics match PTH-12D. Outputs are `path`, a Float64 CoreVerbs
PathSet and both quantization/publication diagnostics. Parameters include the same grid
origin/scale and algorithm identity, plus a positive publication displacement budget and
separate hard work/capacity limits.

Execute the fixed grid algorithm, map grid coordinates to exact geometric coordinates
using the declared grid origin/scale, then round once to Float64 and accumulate the
publication displacement bound during construction using the same exact
squared-error and RU64 rule as PTH-12E, measured in geometric pixel coordinates
after applying origin/scale. Finite representability, the chosen
publication budget, preserved incidence/topology and distinct required grid points
remaining distinct in Float64 are unchecked caller preconditions. No output scan or
publication gate checks them; report topology_status=0 (unverified). Do not claim
original-input exactness. Canonical output must agree with an
explicit PTH-12D result followed by exact `origin + spacing * integer` evaluation and one RN64 per coordinate. The same exact-cycle order, duplicate snapped vertices and degenerate contours are retained; no post-publication reclip or canonicalization changes them.

Complete-input lowering uses fixed tasks, explicit private intermediate Results and
fixed dependencies; a failed call exports no successful Result. Tests include large grid indices,
non-power-of-two scale, valid topology-preserving publication, scale/origin identity,
capacity exhaustion and cancellation. Invalid publication geometry has no promised
runtime diagnostic. `path_boolean.cpp`, the grid codec and `generation::grid_polygon_schema` implement the frozen path. `pth12/grid.py::publish_grid` is its exact independent publication reference. The quantization stage equals PTH-12D; the publication stage is RU64 of the square root of the maximum exact squared coordinate displacement.

The strict CPU implementation is registered. The acceptance list defines required behavior; finite validation does not establish every valid-input success domain or a GPU backend.
