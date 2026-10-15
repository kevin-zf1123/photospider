# Logical regions and Result image samples

## Scope & Ownership

`Region` describes logical samples in a descriptor domain. A `Footprint` stores an exact set of such samples, including disjoint boxes and holes. Storage origin, byte offsets, strides, padding, planar pages, and tile geometry describe physical access; they do not redefine logical coordinates or valid coverage.

For images, the descriptor domain is extended by the tensor's batch axes, such as frame and layer. `ResultRef` owns the image structure, descriptor, published coverage, dependency relation, and lifetime; color and alpha meaning come from operation parameters. `PlanarImage` backs a Result image slot and answers authorized sample reads. The planner determines requested samples, the operation declares input support, and the coordinator admits only the requested reads and publication.

## Data Layout & Memory

```cpp
struct ResultTensorSpec final {
  ResourceLease metadata_owner;
  ResourceString key;
  ResourceVector<std::uint64_t> batch_axes;
  std::uint32_t atomic_trailing_axes = 0;
  ValueDescriptor descriptor;
  ResultTensorLayout layout;
  std::vector<std::uint64_t> sample_shape() const;
  Result<Footprint> close_samples(const Footprint& samples,
                                  const FootprintLimits& limits = {}) const;
};
```

An image is a tensor slot whose `layout.spatial` is true. Its logical sample shape is `batch_axes` followed by the cell descriptor, for example `{N,L,H,W}` or `{N,L,H,W,C}`: `N` and `L` are batch axes such as frame and layer, `H/W` select a spatial sample, and optional `C` selects a channel. The cell descriptor has rank two or three. A four-channel pixel can be stored as separate planes; logical tuple completeness does not make the bytes interleaved.

`close_samples` expands a request over the `atomic_trailing_axes` trailing cell axes. With `atomic_trailing_axes = 1` on an HWC image, a request for one channel of a pixel receives every channel of that pixel, including alpha when alpha is stored on the channel axis. Structural layout groups, the schema id, and the channel count do not expand the sample set. A slot with `atomic_trailing_axes = 0` follows its descriptor sample coordinates exactly. Padding, plane gaps, and unpublished samples remain inaccessible. Storage views retain their backing owner, while Result coverage and its captured descriptor authorize what a callback may read.

Ordinary Value descriptors and structured Result image descriptors have distinct contracts. A Value has nonzero extents; its request Footprint may be Empty while retaining the nonzero descriptor. A Result may have zero rows for a dynamic structured field, and an image slot may have an Empty requested footprint. Neither empty case authorizes fabricated samples or removes descriptor/count obligations.

## Demand, Execution & State

The compiler resolves each named output's requested `Q`. For an image output, the Result continuation receives the captured output footprint and image slot. Its callback can request staged input needs: first Control samples, then Data image samples chosen from those control values. Each Need is checked against the Result schema, slot, logical sample domain, and the input's published coverage. A callback cannot synchronously fetch a hole or sample outside its current capability.

Whole operations keep Whole dependencies. Only `atomic_trailing_axes` participates in sample closure; channel roles and color interpretations are operation parameters and do not change dependency support. Physical `PlanarImageLayout::groups` do not silently create dependency edges. An input port bound to a `requires_complete` validator needs a Whole Need, so its operation cannot use a tile Need on that port.

Dirty means potentially affected, not numerically different. Footprint transformations are bounded and charged to the execution root; exceeding the work or metadata limit returns an error.

## Tile projection arithmetic

For a positive tile extent `T` and logical extent `E`, the tile count is:

$$
C = \left\lceil \frac{E}{T} \right\rceil,
\qquad
C = E / T + (E \bmod T \ne 0).
$$

The integer form avoids evaluating `E + T - 1`, which can overflow. Tile projection groups exact requested samples for physical scheduling; it does not broaden their dependency support or coverage.

## Limits & Error Handling

Out-of-domain regions, unauthorized reads, incomplete publication, and overlapping image writes fail validation. Requests expand over the atomic trailing axes before access; an `atomic_trailing_axes` value larger than the cell rank or a closure outside the slot domain fails validation. Resource exhaustion, cancellation, stale binding replacement, sticky callback failure, and protocol errors retain their execution statuses; they do not become Empty coverage. Admitted callbacks retire before the coordinator reuses borrowed storage.

An empty output demand performs no image sample work, while descriptor and control obligations remain explicit. A successful image publication must cover the captured output demand and provide relation evidence. Previously published Result prefixes remain immutable; a later producer failure does not expand an older descriptor's authorization.

The C11 fixture verified runtime Field rows, including replacement between zero and nonzero rows. Cross-frame support and independent slot dirtiness are covered by `test_result_image_contracts`.
