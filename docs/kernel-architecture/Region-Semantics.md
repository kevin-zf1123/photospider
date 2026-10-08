# Logical regions and Result image samples

## Scope & Ownership

`Region` describes logical samples in a descriptor domain. A `Footprint` stores an exact set of such samples, including disjoint boxes and holes. Storage origin, byte offsets, strides, padding, planar pages, and tile geometry describe physical access; they do not redefine logical coordinates or valid coverage.

For images, the descriptor domain is extended by Result frame and layer axes. `ResultRef` owns image meaning, descriptor, published coverage, dependency relation, and lifetime. `PlanarImage` backs a Result image slot and answers authorized sample reads. The planner determines requested samples, the operation declares input support, and the coordinator admits only the requested reads and publication.

## Data Layout & Memory

```cpp
struct ResultImageSpec final {
  ResourceString key;
  std::uint64_t frames = 1, layers = 1;
  ValueDescriptor descriptor;
  PlanarImageLayout layout;
  std::vector<ValueFacet> facets;
  std::vector<std::uint64_t> sample_shape() const;
  Result<Footprint> close_samples(const Footprint& samples,
                                  const FootprintLimits& limits = {}) const;
};
```

The snippet omits fields and ownership helpers. The logical image shape is `{N,L,H,W}` or `{N,L,H,W,C}`: `N` selects a frame, `L` selects a layer, `H/W` select a spatial sample, and optional `C` selects a channel. The current implementation requires positive `N` and `L` with `N*L <= 4096`, and a per-frame/layer descriptor of rank two or three. A four-channel pixel can be stored as separate planes; logical tuple completeness does not make the bytes interleaved.

A request for a semantic image sample preserves the legacy image tuple closure across its complete channel axis, including alpha when alpha is a channel. A `ColorArray` request closes over the complete tuple described by its validated facet. TDM-only facets and structural layout groups do not expand the sample set to include peer channels or alpha. A typed image slot without either tuple facet follows its descriptor sample coordinates. Padding, plane gaps, and unpublished samples remain inaccessible. Storage views retain their backing owner, while Result coverage and its captured descriptor authorize what a callback may read.

Ordinary Value descriptors and structured Result image descriptors have distinct contracts. A Value has nonzero extents; its request Footprint may be Empty while retaining the nonzero descriptor. A Result may have zero rows for a dynamic structured field, and an image slot may have an Empty requested footprint. Neither empty case authorizes fabricated samples or removes descriptor/count obligations.

## Demand, Execution & State

The compiler resolves each named output's requested `Q`. For an image output, the Result continuation receives the captured output footprint and image slot. Its callback can request staged input needs: first Control samples, then Data image samples chosen from those control values. Each Need is checked against the Result schema, slot, logical sample domain, and the input's published coverage. A callback cannot synchronously fetch a hole or sample outside its current capability.

`ResultRelation` records output-to-input support using flattened logical sample coordinates. It distinguishes input port, target, slot, and role. Data, Control, Validation, and Descriptor support remain distinct. Descriptor support covers count, basis, or semantic descriptor facts; static schema changes require recompilation. Frame and layer coordinates participate in the flattened image identity, so dirty edits can target a particular frame/layer sample.

After immutable bindings are replaced in a demand handle, the coordinator compares the new bindings with source samples consumed by captured evidence. It computes potential dirty output coverage from the old relation, commits the new binding generation atomically, then a new request discovers and records support under the new control values. Thus a changed, consumed Control value can invalidate an old branch and expose newly selected Data samples. An unrelated unconsumed control or tile remains clean. The evidence object itself is immutable: prior dependency queries continue to describe their original generation.

Tiles are scheduling units, not logical dependency proofs. Tile projection may group samples for I/O, but relation support remains sample-based. Whole operations keep Whole dependencies. Regional or dependency operations report their actual support; no tile grid is substituted for those relations. Tensor semantic metadata such as channel roles and ColorArray tuples participates in validation and sample closure where the contract declares it; physical `PlanarImageLayout::groups` do not silently create dependency edges.

`Exact` support can prove clean only when the captured relation is complete. `Conservative` support may mark a wider dirty set. `Unknown` remains unresolved and cannot be converted into an empty clean answer. Dirty means potentially affected, not numerically different. Relation traversal and footprint transformations are bounded and charged to the execution root; exceeding the work or metadata limit returns an error.

## Tile projection arithmetic

For a positive tile extent `T` and logical extent `E`, the tile count is:

$$
C = \left\lceil \frac{E}{T} \right\rceil,
\qquad
C = E / T + (E \bmod T \ne 0).
$$

The integer form avoids evaluating `E + T - 1`, which can overflow. Tile projection groups exact requested samples for physical scheduling; it does not broaden their dependency support or coverage.

## Limits & Error Handling

Out-of-domain regions, invalid frame/layer products, unauthorized reads, incomplete publication, and overlapping image writes fail validation. Semantic image and ColorArray requests expand to their complete channel tuples before access; an invalid tuple description or a closure outside the slot domain fails validation. Resource exhaustion, cancellation, stale binding replacement, sticky callback failure, and protocol errors retain their execution statuses; they do not become Empty coverage. Admitted callbacks retire before the coordinator reuses borrowed storage.

An empty output demand performs no image sample work, while descriptor and control obligations remain explicit. A successful image publication must cover the captured output demand and provide relation evidence. Previously published Result prefixes remain immutable; a later producer failure does not expand an older descriptor's authorization.

The C11 fixture verified runtime Field rows, including replacement between zero and nonzero rows. Cross-frame support and independent slot dirtiness are covered by `test_result_image_contracts`.
