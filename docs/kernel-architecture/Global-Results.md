# Structured Results and image slots

## Scope & Ownership

`ResultRef` is the single semantic, publication, ownership, and input/output path for image data. A Result may contain typed image slots, packed primitive fields, or both. `PlanarImage` is the standard storage backing inside an image slot; it does not provide a parallel image result or execution route. Its creation, import, views, reads, writes, and publication are private to Result owners. Ordinary non-image numeric `Value` remains a separate valid storage form.

Current package version is 0.29.0, workflow schema is 4, and semantic operation traits are version 21. The numeric C operation table remains ABI 11; the Result operation table is ABI 1. These version numbers describe independent contracts.

The compiler copies immutable schema into the plan. The execution coordinator owns producer state and schedules callbacks and I/O. A `ResultRef` owns its schema, monotone descriptor facts, certified backing, dependency relations, and retained input Result owners. Copies of a reference share this ownership. An external reference or read window keeps required backing alive after the execution context ends.

```cpp
struct ResultImageSpec final {
  ResourceString key;
  std::uint64_t frames = 1, layers = 1;
  ValueDescriptor descriptor;
  PlanarImageLayout layout;
  std::vector<ValueFacet> facets;
};

struct SchemaTemplate final {
  ResourceString id;
  std::uint32_t version = 1;
  PublishPolicy publication = PublishPolicy::CompleteBundle;
  ResourceVector<ResultFieldSpec> fields;
  ResourceVector<ResultImageSpec> images;
  ResourceVector<ResultExtent> domain;
  ResourceVector<ResultFacet> metadata;
};

struct WorkflowInputDeclaration final {
  std::uint64_t id;
  std::string name;
  std::shared_ptr<const SchemaTemplate> result_schema;
};

struct ExecutionBinding final {
  std::string name;
  ResultRef result;
};
```

The excerpt omits ordinary Value declarations, default member values, and unrelated binding alternatives. A workflow input with `result_schema` binds an owning `ResultRef`; execution returns named Result outputs in `ExecutionResult::results`. There is no separate image output map.

## Data Layout & Memory

`SchemaTemplate` has at most 16 combined field and image slots, up to eight domain axes, and bounded semantic metadata. A `ResultFieldSpec` describes a primitive element type, a row-count rule, and a record shape of rank zero through seven with positive extents. Row counts can be fixed, input-derived, tied to earlier field rows, or discovered at runtime. Fields are packed primitive records; they are not image storage, and image bytes are never encoded as primitive field records. A `ResultImageSpec` has its own key, descriptor, layout, facets, and frame/layer identity, and owns planar backing for each frame/layer pair.

An image slot has logical coordinates `{frame, layer, descriptor axes...}`. The descriptor preserves its declared axis order; `PlanarImageLayout` maps height, width, and optional channel axes to those coordinates. For example, shape `{6, 2, 4}` can map channel to axis 0, height to axis 1, and width to axis 2. `frames` and `layers` are positive, and their product is at most 4096 backing pairs. The image descriptor accepts rank 2 or 3; frame and layer remain separate axes. Facets may impose additional axis rules. The semantic Image facet requires height and width on axes 0 and 1; the ColorArray facet places its channel tuple on the final descriptor axis. A TDM-only slot can use another mapping, such as CHW. Layout groups alone do not require peer channels or alpha samples.

Sample authorization preserves the existing tuple contract for both the legacy semantic image facet and a validated `ColorArray` facet. For a rank-3 semantic image, a spatial request closes over its complete channel tuple, including alpha when alpha is a channel. For `ColorArray`, the closure includes every channel in the declared tuple. TDM-only facets and structural layout groups do not add peer-channel or alpha demand. A typed slot without either tuple facet follows its descriptor sample coordinates and gains no inferred peer closure. Logical tuple completeness is independent of whether channel bytes are interleaved; planar storage may keep channels in separate planes.

`ResultBuilder` assigns an object id at construction, before runtime counts are known. The semantic key includes canonical schema and a length-framed execution scope. The scope binds the operation contract, parameters, ordered inputs, and captured query. Physical page size, file offsets, descriptor revision, tile dimensions, and resource limits do not change semantic identity. `ResultRef::capture()` snapshots the certified descriptor, field/image relations, descriptor basis, and dependency bundle at one revision. The host actor publishes only this captured view, so observers cannot see a prefix from a later revision paired with earlier evidence. The Result’s ordered source association is separate live state: it retains consumed source owners and may grow monotonically as the producer consumes more inputs. A captured descriptor never gains later authorization. `ResultRef::read_image()` requires that descriptor and reads only certified samples. `ResultImageInput` carries the explicit image Need, slot, descriptor, and authorized sample set into a callback; the borrowed phase and its views expire when that poll returns.

Dynamic row counts belong to structured fields. When a callback consumes a `ResultObjectNeed`, the runtime binds the Field domain to the captured Result's actual certified row count before field access. A zero-row Result is valid and has no field payload page; its descriptor relation still records count, basis, or validation support. Ordinary `Value` storage requires a nonzero extent and rejects the structural image facet. Numeric, Signal, and LUT Values remain ordinary Values when their descriptor/facet contracts permit them. An empty requested Value footprint can describe no requested samples over an otherwise nonzero Value domain; it does not create a zero-extent Value.

## Scheduling & Publication

Each selected named output has its own resolved output contract and captured query. `ResultProgramQuery::image_outputs` contains the requested footprint for the selected image slot, and `output_index` identifies the selected output. An absent image footprint requests the full slot domain. The query, output index, static parameters, and image slot participate in producer identity; tile height, tile width, and page size are physical choices.

The operation starts one host-owned `ResultContinuation`. Each poll returns a bounded `ResultProgramNeed` or a publication. Image input needs identify input index, slot, logical samples, and roles. The coordinator validates the need against the Result input schema and published coverage, then supplies a capability-limited `ResultImageInput`. A callback can first request Control samples, inspect them on the next poll, and then request the Data image support those values select. The returned `ResultRelation` records both support stages.

`ResultBuilder` owns publication. The producer binds descriptor support, writes image regions through `publish_image`, and supplies a relation and finality for each publication. Image regions and their support are checked against the slot's flattened logical sample domain. Overlap, out-of-domain access, incomplete finality, or failed resource admission retires the producer with an error. Sealing checks every field and image relation before completing the Result.

Publication follows the schema policy. `CompleteBundle` hides output until seal. `StablePrefix` and `IndependentChunks` expose only ordered, irrevocable certified field prefixes/ranges; the current publisher does not expose arbitrary out-of-order chunk publication. The callback's `ResultFinality` is an algorithm-author obligation covering data, control, validation, and descriptor facts; the host checks completeness of the declared obligations but does not prove arbitrary callback reads. A later operational failure does not revoke an already certified prefix, and an older descriptor snapshot does not acquire newly published coverage.

When a callback consumes input Results, the published Result retains the ordered input object association and strong owners. Associations may be extended monotonically as additional input owners are consumed; an already claimed object cannot be dropped. This keeps the ancestry needed for later reads and support queries alive. Result resources retain schema-declared owners, including ICC/OCIO owners named by typed image facets. The compiler selects nested resources from the supplied bindings, and runtime bindings are admitted again under the execution resource root. Public `ResourceMap` and `ResourceVector` containers also carry their allocator and managed ownership; copied results, relations, names, and observations retain root-accounted metadata until those containers and any contained `ResultRef` owners are released. Consumers must budget these copies along with payload and relation work. The policy is conservative, so live owners can retain more backing than the final output alone needs.

## Dependency Evidence & Identity

`ResultRelation` is immutable, bounded evidence mapping each flattened output observation to input support. A support address names the input index, target (`Value`, `Field`, `Image`, or `Descriptor`), slot, sample interval, and roles (`Data`, `Control`, `Validation`, `Descriptor`). Roles and targets are part of dirty matching. A descriptor observation is separate from field/image sample coordinates; an empty data set can still depend on a runtime count or semantic basis.

`Exact` means the declared support is complete for the registered operation contract. It does not mean that changed values necessarily change output bits. `Conservative` permits broader support. `Unknown` is unresolved and cannot prove an output clean. Dirty queries compare the captured relation with changed samples and report potential change, not numeric inequality.

Semantic schema identity includes typed slots and semantic metadata, but excludes physical page/tile geometry, storage offsets, object ids, and descriptor revisions. The execution plan includes physical choices when they affect scheduling. Result keys also include the operation contract, parameters, ordered input identities, and captured query. Changing static schema or descriptor facts that require recompilation requires a new plan.

## Limits & Failure Handling

The execution root accounts for payload/backing, work, stages, I/O, relation/map construction, continuations, queue entries, metadata, and retained owners under their configured resource dimensions. Bounds are finite. Oversized relation traversal, payload, or owner admission fails with a resource error rather than returning an incomplete witness. This managed-capacity model is not an RSS bound.

Result publication errors are sticky. Cancellation and stale execution retire active state only after admitted callbacks have returned. A waiter cancellation does not cancel a shared producer while another waiter remains. A failed append or image publication cannot be repaired by a later successful callback; a previously certified prefix remains immutable and readable under its captured descriptor. The last owning Result/read-window release retires backing and retained associations.

All 13 focused checks passed, including `test_result_execution` paging 8192 rows and prefix/cancellation/shared behavior, and `test_result_image_contracts` covering immutable captured facts, relation guarantees, owner retirement, cross-frame support, tuple closure, and semantic-alias diamond rebind. `test_execution_dependencies`, `test_multi_output_execution`, and `test_generic_result_cache` also passed, verifying existing numeric workflows, alias root-cache hits with actual result 14, correct dirty propagation, and finite proof work. Native Metal tests passed affine and broadcast packed transfers plus root-work rejection at a 500-unit limit. The C11 Result fixture and native Result fixture passed; the latter read back float value 4. All five installed consumers passed: unified workflow, C++, Result contracts, C11, and native GPU. C coverage also verifies runtime Field-domain binding from consumed `ResultObjectNeed` rows and Descriptor/Field replacement between zero and nonzero rows. Insufficient `source_support()` budget returns a typed ResourceExhausted result.
