# Paged four-connected components and area index

[Chinese reader version](zh/Paged-Components.zh.md).

## 1. Scope and ownership

The `components4.labels`, `components4.area`, and `components4.filter` CPU operations compute a four-connected partition, derive a sorted area index, and threshold labels by area. The labels producer owns a disk-backed union-find table during the full raster scan. Result publication owns the completed labels/table together; area and filter Results retain the exact input ObjectIds and backing needed to interpret their rows.

## 2. Data layout and memory

```cpp
enum class ComponentIdScheme : std::uint32_t { MinPixel = 1, CompactMinOrder = 2 };
struct ComponentsSpec final {
  std::uint64_t height = 1, width = 1, maximum_count = 1048576;
  ComponentIdScheme ids = ComponentIdScheme::MinPixel;
};
Result<SchemaTemplate> component_area_schema(const ComponentsSpec& spec);
Result<SchemaTemplate> component_filter_schema(const ComponentsSpec& spec);
Result<OperationDefinition> make_component_operation(
    ComponentOperation operation, const ComponentsSpec& spec);
```

The current operation factory supports `MinPixel`: positive HW dimensions, checked `H*W <= (INT64_MAX-4095)/32`, and `maximum_count <= INT64_MAX`. Input is a facet-free UInt8 HW mask; any nonzero byte is foreground. Background label is zero. A foreground component ID is one plus the minimum row-major foreground pixel position. IDs are deterministic for one input snapshot; a later edit that splits or merges regions can change them.

| Operation | Input and output |
| --- | --- |
| `components4.labels` | UInt8 HW -> CompleteBundle labels and `(id,area,min_position)` rows |
| `components4.area` | Complete Components -> CompleteBundle sorted `(id,area)` rows with RuntimeCount |
| `components4.filter` | Components plus its associated area index -> CompleteBundle UInt8 HW mask |

The area index schema retains the Components basis. The filter schema retains the same basis and takes a required positive Int64 `minimum_area` in its operation identity. For each pixel it emits one iff `label!=0 && area[label]>=minimum_area`. `maximum_count` limits final component count, not workspace for all N labels and N provisional union records. K=0 is valid even when N>0; outputs then contain all-zero labels/mask and zero table rows.

## 3. Execution and state machine

```text
UInt8 source -> labels producer -> complete Components Result
                                     |                 |
                                     v                 |
                               area producer           |
                                     |                 |
                                area index             |
                                     +--------+--------+
                                              v
                                           filter
                                              |
                                         binary mask
```

Labels reads bounded source strips, then completes all private union writes and the second output scan before publishing the CompleteBundle. Area waits for a complete validated Components result. Filter requires both the exact Components Result and its area index; it verifies their association and equal counts, compares every index row against the complete Components table, then evaluates pixels. The index association mismatch is `TypeMismatch` with `InvalidAssociation`, `Association` scope, and the index ObjectId. A nonzero label with no associated area also fails; it is not treated as area zero.

All three operations use CompleteBundle and Conservative(All) support. Descriptor support is separate from data rows, including for K=0. Structural validation checks counts and basis, but does not prove connectivity of an imported label map. Only the labels operation's construction establishes four-connectivity for its output.

The labels operation declares 8192 bytes of callback workspace; area and filter each declare 4096 bytes. Current windows are capped by `min(user_page_bytes,1024)`; labels needs at least 32 bytes, and area/filter need at least 24 bytes. The root charges temporary backing, windows, work, I/O, and stages. Dirty-page replacement writes a victim before reading its replacement. If resources run out before complete publication, the operation releases private state and publishes no partial labels/table.

## 4. Algorithms and math

The labels producer creates one private temporary file and extends it by checked `32*N` bytes. Each pixel occupies four 64-bit words: background `[0,0,0,0]`; foreground position `i` as `[i+1,0,i,1]` for parent address, rank, minimum position, and area. Parent addresses start at one; minimum positions start at zero.

The producer visits pixels in row-major order and considers only left and top edges. For each edge it finds roots using bounded paged parent reads. It skips a shared root; otherwise it links by rank, adds disjoint areas, and retains the smaller position. Both cached records are updated before the producer visits another neighbor. The current foreground pixel remains a root before its first edge because earlier pixels visit only smaller indices. Once all edges are processed, a second scan finds roots, writes label `minimum+1`, and appends `(id,area,minimum)` only at the minimum pixel. This emits unique sorted table rows without a resident sort table.

Every four-connected grid edge is visited once by its left/top orientation. Union cannot join distinct connected components; the scan includes every edge within each component. Root minimum and area are preserved by minimum and disjoint-set addition. Rank-only parent chains have O(log N) depth; the implementation checks record addresses and caps root traversal at 64 hops. It does not use path compression.

Label generation and final root discovery cost `O(N log N)`. Area-index copy and verification cost `O(K)`, and filtering costs `O(N log K)`. Representation validation additionally scans N labels for each of K rows and binary-searches the table for nonzero labels, costing `O(NK + N log K)` work. Paged alternation can reload field windows, so actual I/O exceeds a calculation based only on final payload size.

## 5. Limitations and non-goals

- Only `ComponentIdScheme::MinPixel` is supported by these factories; `CompactMinOrder` is a representation enum, not a factory capability.
- The validator checks exact labels/table membership and basis, not connectivity of arbitrary imported labels.
- `maximum_count` limits final K; it does not limit provisional N records. K=0 is valid for N>0. Exceeding the count limit fails before publication as an invalid domain.
- All N label records, N provisional records, I/O, work, and output capacity still require root admission. Page, disk, and stage exhaustion return resource failures.
- Managed-capacity accounting does not bound process RSS.

See [the components workflow](../../examples/components_workflow/README.md) for the public entry point and runnable usage.
