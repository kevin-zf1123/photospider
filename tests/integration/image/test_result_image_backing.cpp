#include <algorithm>
#include <iostream>
#include <utility>
#include <vector>

#include "support/result_image_fixture.hpp"

namespace {
using namespace ps::test_image;  // NOLINT(build/namespaces)
void numerical_tensor_windows() {
  ResourceBudget root;
  auto make = [&](ValueDescriptor descriptor, StridedLayout layout,
                  std::vector<std::uint8_t> bytes, std::uint32_t atomic = 0) {
    auto storage =
        take(Value::create(descriptor, Region::whole(descriptor.shape), layout,
                           std::move(bytes)))
            .storage();
    SchemaTemplate schema;
    schema.id = "test.tensor";
    ResultTensorSpec slot;
    slot.key = "samples";
    slot.descriptor = descriptor;
    slot.atomic_trailing_axes = atomic;
    schema.tensors.push_back(std::move(slot));
    require(schema.validate(true).ok(), "generic tensor schema");
    require(schema.canonical().size() == schema.canonical_size(),
            "tensor canonical exact size");
    auto builder = take(ResultBuilder::start(root, schema, "tensor.source"));
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
                .ok(),
            "tensor basis");
    const auto count = schema.tensors[0].sample_count();
    auto witness = take(ResultRelation::cartesian(
        root, count.ok() ? count.value() : UINT64_MAX, {0, 1, 0, 0}));
    require(builder
                .publish_tensor(0, Region::whole(descriptor.shape),
                                std::move(layout), storage, witness,
                                {true, true, true, true})
                .ok(),
            "affine tensor publication");
    auto result = take(builder.seal());
    auto window = take(result.acquire_tensor(take(result.descriptor()), 0,
                                             Region::whole(descriptor.shape)));
    require(window.storage_owner_token() == storage.get(),
            "affine keeps storage owner");
    return window;
  };
  SchemaTemplate canonical;
  canonical.id = "test.tensor";
  ResultTensorSpec scalar_spec;
  scalar_spec.key = "samples";
  scalar_spec.descriptor = {ElementType::Float64, {3}};
  canonical.tensors.push_back(scalar_spec);
  auto no_spatial_axes = canonical;
  no_spatial_axes.tensors[0].layout.channel_axis.reset();
  no_spatial_axes.tensors[0].layout.height_axis = 6;
  no_spatial_axes.tensors[0].layout.width_axis = 7;
  no_spatial_axes.tensors[0].layout.order = ImagePlaneOrder::Continuous;
  no_spatial_axes.tensors[0].layout.row_pitch_bytes = 256;
  require(
      canonical.validate(true).ok() && no_spatial_axes.validate(true).ok() &&
          canonical.same_schema(no_spatial_axes) &&
          canonical.canonical() == no_spatial_axes.canonical(),
      "nonspatial axes and physical preferences do not change semantic schema");
  const auto huge = static_cast<std::uint64_t>(INT64_MAX) + 2;
  auto reversed =
      make({ElementType::UInt8, {huge, 3}}, {2, {0, -1}}, {1, 2, 3});
  auto run = take(reversed.row_run({huge - 1, 0}));
  require(run.samples == 3 && run.bytes == 3 && run.sample_stride_bytes == -1 &&
              run.data[0] == 3 && run.data[-2] == 1,
          "negative stride in huge broadcast tensor");
  auto rectangle = take(reversed.rectangle_run({0, 0}));
  require(rectangle.rows == huge && rectangle.row_stride_bytes == 0,
          "zero row stride keeps huge logical extent");
  auto broadcast = make({ElementType::UInt8, {UINT64_MAX}}, {0, {0}}, {9});
  run = take(broadcast.row_run({UINT64_MAX - 2}));
  require(run.samples == 2 && run.bytes == 1 && run.sample_stride_bytes == 0 &&
              *run.data == 9,
          "UINT64_MAX tensor reads actual one-byte span");
  auto singleton = make({ElementType::UInt8, {1}}, {0, {INT64_MIN}}, {7});
  require(*take(singleton.row_run({0})).data == 7,
          "singleton INT64_MIN stride");
  auto eight = make({ElementType::UInt8, {1, 1, 1, 1, 1, 1, 1, 2}},
                    {0, {0, 0, 0, 0, 0, 0, 0, 1}}, {4, 5});
  require(eight.spec().sample_shape().size() == 8 &&
              *take(eight.row_run({0, 0, 0, 0, 0, 0, 0, 1})).data == 5,
          "rank eight has no synthetic axes");
  ResultTensorSpec tuple;
  tuple.descriptor = {ElementType::UInt8, {huge, 3}};
  tuple.atomic_trailing_axes = 1;
  auto wanted = take(Footprint::from_regions(
      tuple.sample_shape(), {Region({{huge - 1, 1}, {1, 1}})}));
  auto closed = take(tuple.close_samples(wanted));
  require(
      closed == take(Footprint::from_regions(
                    tuple.sample_shape(), {Region({{huge - 1, 1}, {0, 3}})})) &&
          tuple.observation_shape() == std::vector<std::uint64_t>{huge},
      "atomic tensor closure retains batch-free observation shape");
}
void spatial_tensor_batches() {
  ResourceBudget root;
  for (auto batches : {std::vector<std::uint64_t>{}, {2}, {1, 1, 2}}) {
    SchemaTemplate schema;
    schema.id = "test.spatial.tensor";
    ResultTensorSpec slot;
    slot.key = "samples";
    slot.descriptor = {ElementType::Float32, {2, 2}};
    slot.batch_axes.assign(batches.begin(), batches.end());
    slot.layout.spatial = true;
    slot.layout.channel_axis.reset();
    schema.tensors.push_back(slot);
    auto builder = take(ResultBuilder::start(root, schema, "spatial.batches"));
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
                .ok(),
            "spatial basis");
    const auto shape = slot.sample_shape();
    const auto count = take(slot.sample_count());
    std::vector<float> pixels(count);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
      pixels[i] = static_cast<float>(i);
    }
    require(
        builder
            .publish_tensor(
                0, Region::whole(shape),
                ByteView(reinterpret_cast<const std::uint8_t*>(pixels.data()),
                         pixels.size() * 4),
                take(ResultRelation::cartesian(root, count, {0, 1, 0, 0})),
                {true, true, true, true})
            .ok(),
        "spatial arbitrary batches publication");
    auto result = take(builder.seal());
    auto facts = take(result.descriptor());
    auto dims = Region::whole(shape).dimensions();
    for (std::size_t axis = 0; axis < batches.size(); ++axis) {
      dims[axis] = {0, 1};
    }
    if (!batches.empty()) {
      dims[batches.size() - 1] = {1, 1};
    }
    auto window = take(result.acquire_tensor(facts, 0, Region(dims)));
    std::vector<std::uint64_t> at;
    for (auto d : dims) {
      at.push_back(d.offset);
    }
    float first = 0;
    std::memcpy(&first, take(window.row_run(at)).data, 4);
    require(first == (batches.empty() ? 0.F : 4.F),
            "private batch index selects correct backing");
    if (!batches.empty()) {
      const auto payload = root.statistics().live[ResourceKind::Payload];
      auto complete =
          take(result.acquire_tensor(facts, 0, Region::whole(shape)));
      std::vector<uint64_t> coordinate(shape.size(), 0);
      for (uint64_t i = 0; i < count; ++i) {
        float sample = 0;
        std::memcpy(&sample, take(complete.row_run(coordinate)).data, 4);
        require(sample == static_cast<float>(i),
                "paged complete window selects exact full batch prefix");
        auto rectangle = take(complete.rectangle_run(coordinate));
        require(rectangle.rows == 2 - coordinate[batches.size()],
                "paged rectangle remains in selected physical batch");
        for (size_t axis = coordinate.size(); axis-- > 0;) {
          if (++coordinate[axis] < shape[axis])
            break;
          coordinate[axis] = 0;
        }
      }
      auto narrowed = Region::whole(shape).dimensions();
      narrowed.back() = {0, 1};
      auto restricted = take(result.acquire_tensor(facts, 0, Region(narrowed)));
      coordinate.assign(shape.size(), 0);
      coordinate.back() = 1;
      require(!restricted.row_run(coordinate).ok(),
              "multi-batch acquisition does not authorize spatial holes");
      require(root.statistics().live[ResourceKind::Payload] == payload,
              "multi-batch windows do not allocate or copy payload");
      CancellationSource cancel;
      cancel.cancel();
      const auto live = root.statistics().live.values;
      auto stopped =
          result.acquire_tensor(facts, 0, Region::whole(shape), cancel.token());
      require(!stopped.ok() && stopped.status().code == ErrorCode::Cancelled &&
                  root.statistics().live.values == live,
              "multi-batch acquisition cancellation releases all metadata");
    }
  }
  ResourceLimits limits;
  limits.maximum_work = 100000;
  ResourceBudget limited(limits);
  SchemaTemplate schema;
  schema.id = "test.tensor";
  ResultTensorSpec slot;
  slot.key = "samples";
  slot.descriptor = {ElementType::UInt8, {2}};
  schema.tensors.push_back(slot);
  auto builder = take(ResultBuilder::start(limited, schema, "limited.acquire"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(limited, 1, {0, 8, 0, 0})))
              .ok(),
          "limited basis");
  const std::uint8_t bytes[] = {3, 4};
  require(builder
              .publish_tensor(
                  0, Region::whole({2}), ByteView(bytes, 2),
                  take(ResultRelation::cartesian(limited, 2, {0, 1, 0, 0})),
                  {true, true, true, true})
              .ok(),
          "limited affine source");
  auto result = take(builder.seal());
  auto facts = take(result.descriptor());
  require(
      limited.consume({limits.maximum_work - limited.statistics().issued.work})
          .ok(),
      "exhaust window work budget");
  auto window = result.acquire_tensor(facts, 0, Region::whole({2}));
  require(!window.ok() &&
              window.status().code == ErrorCode::ResourceExhausted &&
              window.status().reason == FailureReason::WorkLimit,
          "affine acquire preserves typed work failure without throwing");
}
ResultRef paged_batch_source(const ResourceBudget& root) {
  SchemaTemplate schema;
  schema.id = "test.indexed.batches";
  ResultTensorSpec slot;
  slot.key = "samples";
  slot.descriptor = {ElementType::Float32, {1, 1}};
  slot.batch_axes = {64};
  slot.layout.spatial = true;
  slot.layout.channel_axis.reset();
  schema.tensors.push_back(slot);
  auto builder = take(ResultBuilder::start(root, schema, "indexed.batches"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
              .ok(),
          "indexed batch basis");
  float values[64];
  for (unsigned i = 0; i < 64; ++i)
    values[i] = static_cast<float>(i);
  require(builder
              .publish_tensor(
                  0, Region::whole(slot.sample_shape()),
                  ByteView(reinterpret_cast<const uint8_t*>(values),
                           sizeof(values)),
                  take(ResultRelation::cartesian(root, 64, {0, 1, 0, 0})),
                  {true, true, true, true})
              .ok(),
          "indexed batch publication");
  return take(builder.seal());
}
void indexed_batch_resources() {
  ResourceBudget root;
  auto source = paged_batch_source(root);
  auto facts = take(source.descriptor());
  const auto baseline = root.statistics().live.values;
  const auto start = root.statistics().issued.work;
  uint64_t acquire_work = 0;
  {
    CancellationSource cancellation;
    auto window = take(source.acquire_tensor(
        facts, 0, Region::whole({64, 1, 1}), cancellation.token()));
    acquire_work = root.statistics().issued.work - start;
    const auto reads = root.statistics().issued.work;
    for (uint64_t i = 0; i < 64; ++i) {
      float value;
      std::memcpy(&value, take(window.row_run({i, 0, 0})).data, 4);
      require(value == static_cast<float>(i), "indexed batch row value");
    }
    require(root.statistics().issued.work - reads < 64 * 16,
            "batch row lookup performs bounded logarithmic comparisons");
    cancellation.cancel();
    auto stopped = window.rectangle_run({63, 0, 0});
    require(!stopped.ok() && stopped.status().code == ErrorCode::Cancelled,
            "owning paged window observes cancellation after acquisition");
  }
  require(root.statistics().live.values == baseline,
          "indexed window retires every metadata lease");
  ResourceLimits limits;
  limits.maximum_work = UINT64_MAX;
  ResourceBudget limited(limits);
  auto bounded = paged_batch_source(limited);
  auto bounded_facts = take(bounded.descriptor());
  const auto used = limited.statistics().issued.work;
  require(used == start, "paged source work is deterministic");
  require(limited.consume({UINT64_MAX - used - acquire_work + 1}).ok(),
          "leave acquisition one unit short at the final index comparisons");
  const auto live = limited.statistics().live.values;
  auto rejected =
      bounded.acquire_tensor(bounded_facts, 0, Region::whole({64, 1, 1}));
  require(
      !rejected.ok() &&
          rejected.status().code == ErrorCode::ResourceExhausted &&
          rejected.status().reason == FailureReason::WorkLimit &&
          limited.statistics().live.values == live,
      "late directory/index work failure stays typed and rolls metadata back");
  ResourceBudget lookup(limits);
  auto lookup_source = paged_batch_source(lookup);
  auto lookup_facts = take(lookup_source.descriptor());
  auto window = take(
      lookup_source.acquire_tensor(lookup_facts, 0, Region::whole({64, 1, 1})));
  require(
      lookup.consume({UINT64_MAX - lookup.statistics().issued.work - 2}).ok(),
      "leave a partial binary lookup budget");
  const auto lookup_live = lookup.statistics().live.values;
  auto row = window.row_run({63, 0, 0});
  require(!row.ok() && row.status().code == ErrorCode::ResourceExhausted &&
              row.status().reason == FailureReason::WorkLimit &&
              lookup.statistics().live.values == lookup_live,
          "paged lookup charges actual comparisons without losing window "
          "ownership");
}
void tensor_writer_reentrancy() {
  for (bool spatial : {false, true}) {
    for (unsigned mode = 0; mode < 7; ++mode) {
      ResourceBudget root;
      SchemaTemplate schema;
      schema.id = "test.writer.reentrancy";
      schema.publication = PublishPolicy::StablePrefix;
      ResultTensorSpec slot;
      slot.key = "samples";
      slot.descriptor = {ElementType::Float32, {1, 3}};
      slot.layout.spatial = spatial;
      slot.layout.channel_axis.reset();
      schema.tensors.push_back(slot);
      auto builder = take(ResultBuilder::start(root, schema, "writer.reentry"));
      auto basis = take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0}));
      require(builder.bind_descriptor_relation(basis).ok(),
              "writer reentry basis");
      auto relation = take(ResultRelation::cartesian(root, 3, {0, 1, 0, 0}));
      const Region left({{0, 1}, {0, 1}}), middle({{0, 1}, {1, 1}}),
          right({{0, 1}, {2, 1}});
      const float one = 1;
      require(builder
                  .publish_tensor(
                      0, left,
                      ByteView(reinterpret_cast<const std::uint8_t*>(&one), 4),
                      relation, {true, true, true, true})
                  .ok(),
              "writer earlier prefix");
      auto reference = builder.reference();
      auto facts = take(reference.descriptor(false));
      auto storage =
          take(Value::create({ElementType::Float32, {1}}, Region::whole({1}),
                             {0, {4}}, std::vector<std::uint8_t>(4)))
              .storage();
      Status inner;
      std::optional<ResultBuilder> transferred;
      unsigned nested_calls = 0;
      auto outer = builder.publish_tensor_kernel(
          0, middle,
          [&](const auto& writers) {
            // This is a new acquisition of the immutable prefix while a
            // physical writer is live, rather than reuse of a pre-acquired
            // window.
            auto prefix = reference.acquire_tensor(facts, 0, left);
            if (!prefix.ok()) {
              return prefix.status();
            }
            float sample = 0;
            auto read = prefix.value().row_run({0, 0});
            if (!read.ok()) {
              return read.status();
            }
            std::memcpy(&sample, read.value().data, 4);
            require(sample == one &&
                        !reference.acquire_tensor(facts, 0, middle).ok(),
                    "callback queries only old immutable certified prefix");
            if (mode == 1) {
              inner =
                  builder.publish_tensor(0, right, {0, {0, 4}, {0, 2}}, storage,
                                         relation, {true, true, true, true});
            }
            if (mode == 2) {
              inner = builder.seal().status();
            }
            if (mode == 3) {
              inner = builder.publish_tensor_kernel(0, right,
                                                    [&](const auto&) {
                                                      ++nested_calls;
                                                      return Status::success();
                                                    },
                                                    relation,
                                                    {true, true, true, true});
            }
            if (mode == 4 || mode == 5) {
              if (mode == 5) {
                inner = {ErrorCode::OperationFailed, "callback domain failure",
                         FailureReason::InvalidDomain};
                inner.detail = {FailureOrigin::Domain,
                                FailureScope::ValidationDomain};
                builder.fail(inner);
              }
              transferred.emplace(std::move(builder));
            }
            if (mode == 6) {
              builder = take(ResultBuilder::start(root, schema, "replacement"));
            }
            auto write = writers[0].row_run({0, 1});
            if (!write.ok()) {
              return write.status();
            }
            const float two = 2;
            std::memcpy(write.value().data, &two, 4);
            return Status::success();
          },
          relation, {true, true, true, true});
      require(!nested_calls,
              "recursive writer is rejected before physical lock or callback");
      auto after = take(reference.descriptor(false));
      require(reference.acquire_tensor(after, 0, left).ok() &&
                  !reference.acquire_tensor(after, 0, right).ok(),
              "writer reentrancy cannot replace or revoke prior coverage");
      if (!mode) {
        require(outer.ok() && reference.acquire_tensor(after, 0, middle).ok(),
                "own-prefix query permits ordinary successful publication");
      } else {
        require(
            !outer.ok() && !reference.acquire_tensor(after, 0, middle).ok() &&
                after.revision() == facts.revision(),
            "failed/changed producer rolls back only the new tensor region");
        if (mode < 4) {
          require(inner.code == ErrorCode::InvalidArgument &&
                      outer.code == inner.code,
                  "same producer mutation is rejected with sticky protocol "
                  "failure");
        }
        if (mode == 4) {
          require(outer.code == ErrorCode::Stale && transferred &&
                      !transferred->seal().ok(),
                  "transferred producer is retained and safely rejected");
        }
        if (mode == 5) {
          require(
              outer.code == inner.code && outer.reason == inner.reason &&
                  outer.detail.origin == inner.detail.origin &&
                  outer.detail.scope == inner.detail.scope,
              "transfer after callback failure preserves complete first cause");
        }
        if (mode == 6) {
          require(
              outer.code == ErrorCode::Cancelled &&
                  builder.bind_descriptor_relation(basis).ok() &&
                  builder
                      .publish_tensor(
                          0, right,
                          ByteView(reinterpret_cast<const std::uint8_t*>(&one),
                                   4),
                          relation, {true, true, true, true})
                      .ok(),
              "entry owner failure never poisons an independent replacement "
              "builder");
        }
      }
    }
  }
}
void empty_tensor_writers() {
  for (bool spatial : {false, true}) {
    ResourceBudget root;
    SchemaTemplate schema;
    schema.id = "test.empty.tensor";
    ResultTensorSpec slot;
    slot.key = "samples";
    slot.descriptor = {ElementType::Float32, {1024, 1024}};
    slot.layout.spatial = spatial;
    slot.layout.channel_axis.reset();
    schema.tensors.push_back(slot);
    auto builder = take(ResultBuilder::start(root, schema, "empty.tensor"));
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
                .ok(),
            "empty tensor basis");
    auto witness =
        take(ResultRelation::cartesian(root, 1024 * 1024, {0, 1, 0, 0}));
    const auto before = root.statistics();
    std::uint32_t called = 0;
    require(builder
                .publish_tensor_kernel(0, Region({{0, 0}, {0, 1024}}),
                                       [&](const auto&) {
                                         ++called;
                                         return Status::success();
                                       },
                                       witness, {true, true, true, true})
                .ok(),
            "empty tensor kernel publication");
    auto result = take(builder.seal());
    require(!called &&
                root.statistics().live[ResourceKind::Payload] ==
                    before.live[ResourceKind::Payload] &&
                root.statistics().issued.io_bytes == before.issued.io_bytes &&
                root.statistics().issued.work - before.issued.work < 1024,
            "empty tensor demand skips callback/payload/sample work for every "
            "backing");
    require(take(result.descriptor()).tensor_coverage(0).empty(),
            "empty kernel certifies no samples");
  }
}
void lazy_spatial_backing() {
  for (int orientation : {0, 1, 2}) {
    const bool batched = orientation == 2;
    ResourceBudget root;
    SchemaTemplate schema;
    schema.id = "test.spatial.broadcast";
    ResultTensorSpec slot;
    slot.key = "samples";
    slot.descriptor = {ElementType::Float32,
                       batched ? std::vector<std::uint64_t>{1, 1}
                       : orientation == 0
                           ? std::vector<std::uint64_t>{UINT64_MAX, 1}
                           : std::vector<std::uint64_t>{1, UINT64_MAX}};
    if (batched) {
      slot.batch_axes = {UINT64_MAX};
    }
    slot.layout.spatial = true;
    slot.layout.channel_axis.reset();
    schema.tensors.push_back(slot);
    require(schema.validate(true).ok(), "huge spatial topology is valid");
    const auto shape = slot.sample_shape();
    require(
        ResultBuilder::start(root, schema, "spatial.bad.tile", {}, {}, 0, 32)
                    .status()
                    .code == ErrorCode::TypeMismatch &&
            ResultBuilder::start(root, schema, "spatial.bad.tile", {}, {}, 32,
                                 3)
                    .status()
                    .code == ErrorCode::TypeMismatch,
        "spatial physical configuration validates before backing choice");
    auto builder = take(ResultBuilder::start(root, schema, "spatial.lazy"));
    require(root.statistics().live[ResourceKind::Payload] == 0,
            "spatial start does not choose a payload backing");
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
                .ok(),
            "huge spatial basis");
    const auto storage =
        take(Value::create({ElementType::Float32, {1}}, Region::whole({1}),
                           {0, {0}}, std::vector<std::uint8_t>{0, 0, 0, 0x3f}))
            .storage();
    StridedLayout layout;
    layout.byte_strides.resize(shape.size(), 0);
    auto relation =
        take(ResultRelation::cartesian(root, UINT64_MAX, {0, 1, 0, 0}));
    require(builder
                .publish_tensor(0, Region::whole(shape), layout, storage,
                                relation, {true, true, true, true})
                .ok(),
            "huge spatial affine source publishes without dense reservation");
    auto result = take(builder.seal());
    std::vector<RegionDimension> dimensions;
    std::vector<std::uint64_t> at;
    for (auto n : shape) {
      dimensions.push_back({n - 1, 1});
      at.push_back(n - 1);
    }
    auto window = take(result.acquire_tensor(take(result.descriptor()), 0,
                                             Region(dimensions)));
    auto run = take(window.row_run(at));
    float sample = 0;
    std::memcpy(&sample, run.data, sizeof(sample));
    require(sample == .5F && window.storage_owner_token() == storage.get(),
            "huge spatial logical endpoint reads the small immutable owner");
    if (!batched) {
      auto materialized =
          take(ResultBuilder::start(root, schema, "spatial.write"));
      require(materialized
                  .bind_descriptor_relation(
                      take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
                  .ok(),
              "materialized huge spatial basis");
      const auto before = root.statistics().live[ResourceKind::Payload];
      bool called = false;
      const auto written = materialized.publish_tensor_kernel(
          0, Region({{0, 1}, {0, 1}}),
          [&](const auto& writers) {
            called = true;
            auto run = writers[0].row_run({0, 0});
            if (!run.ok())
              return run.status();
            const float value = .5F;
            std::memcpy(run.value().data, &value, 4);
            return Status::success();
          },
          relation, {true, true, true, true});
      require(written.ok() && called &&
                  root.statistics().live[ResourceKind::Payload] == before + 4,
              "huge spatial ROI write admits only its actual affine payload");
      auto output = take(materialized.seal());
      float produced = 0;
      require(
          output.read_tensor(take(output.descriptor()), 0, {0, 0}, &produced, 4)
                  .ok() &&
              produced == .5F,
          "huge spatial kernel output retains its logical schema");
      ResourceLimits constrained;
      constrained.capacity[ResourceKind::Payload] = 3;
      ResourceBudget limited(constrained);
      auto denied =
          take(ResultBuilder::start(limited, schema, "spatial.denied"));
      require(denied
                  .bind_descriptor_relation(
                      take(ResultRelation::cartesian(limited, 1, {})))
                  .ok(),
              "denied basis");
      called = false;
      const auto failed = denied.publish_tensor_kernel(
          0, Region({{0, 1}, {0, 1}}),
          [&](const auto&) {
            called = true;
            return Status::success();
          },
          take(ResultRelation::cartesian(limited, UINT64_MAX, {})),
          {true, true, true, true});
      require(failed.code == ErrorCode::ResourceExhausted && !called &&
                  limited.statistics().live[ResourceKind::Payload] == 0 &&
                  denied.reference().descriptor(false).status().code ==
                      ErrorCode::ResourceExhausted,
              "affine ROI backing checks actual reservation before callback");
    } else {
      auto sparse = take(ResultBuilder::start(root, schema, "spatial.sparse"));
      require(sparse
                  .bind_descriptor_relation(
                      take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
                  .ok(),
              "sparse batch basis");
      require(sparse
                  .publish_tensor_kernel(0, Region(dimensions),
                                         [&](const auto& writers) {
                                           auto run = writers[0].row_run(at);
                                           if (!run.ok()) {
                                             return run.status();
                                           }
                                           const float value = .25F;
                                           std::memcpy(run.value().data, &value,
                                                       sizeof(value));
                                           return Status::success();
                                         },
                                         relation, {true, true, true, true})
                  .ok(),
              "huge batch domain allocates only the requested physical batch");
      auto sparse_result = take(sparse.seal());
      auto sparse_window = take(sparse_result.acquire_tensor(
          take(sparse_result.descriptor()), 0, Region(dimensions)));
      std::memcpy(&sample, take(sparse_window.row_run(at)).data,
                  sizeof(sample));
      require(sample == .25F, "sparse physical batch has exact coordinate key");
    }
  }
}
void direct_tensor_writers() {
  for (bool huge : {false, true}) {
    ResourceBudget root;
    SchemaTemplate schema;
    schema.id = "test.direct.tensor";
    ResultTensorSpec slot;
    slot.key = "samples";
    slot.descriptor = {ElementType::UInt8,
                       huge ? std::vector<std::uint64_t>{UINT64_MAX, 2}
                            : std::vector<std::uint64_t>{300000}};
    schema.tensors.push_back(slot);
    const auto shape = slot.sample_shape();
    const Region region =
        huge ? Region({{UINT64_MAX - 1, 1}, {0, 2}}) : Region::whole(shape);
    auto builder = take(ResultBuilder::start(root, schema, "direct.tensor"));
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
                .ok(),
            "direct tensor basis");
    const auto count = slot.sample_count();
    auto witness = take(ResultRelation::cartesian(
        root, count.ok() ? count.value() : UINT64_MAX, {0, 1, 0, 0}));
    require(builder
                .publish_tensor_kernel(
                    0, region,
                    [](const auto& writers) {
                      std::vector<std::uint64_t> at;
                      for (const auto d : writers[0].region().dimensions()) {
                        at.push_back(d.offset);
                      }
                      auto run = writers[0].row_run(at);
                      if (!run.ok()) {
                        return run.status();
                      }
                      for (std::uint64_t n = 0; n < run.value().samples; ++n) {
                        run.value().data[n * run.value().sample_stride_bytes] =
                            0x7f;
                      }
                      return Status::success();
                    },
                    witness, {true, true, true, true})
                .ok(),
            "generic direct write publishes exact ROI");
    auto result = take(builder.seal());
    require(
        root.statistics().peak[ResourceKind::Payload] == (huge ? 2 : 300000) &&
            root.statistics().issued.io_bytes == 0,
        "direct tensor writer retains one payload allocation without packed "
        "copy");
    auto window =
        take(result.acquire_tensor(take(result.descriptor()), 0, region));
    std::vector<std::uint64_t> at;
    for (auto d : region.dimensions()) {
      at.push_back(d.offset);
    }
    auto run = take(window.row_run(at));
    require(run.samples == (huge ? 2 : 300000) && run.data[0] == 0x7f &&
                run.data[(run.samples - 1) * run.sample_stride_bytes] == 0x7f,
            "direct writer data survives unpublished buffer retirement");
    if (huge) {
      require(!result
                   .acquire_tensor(take(result.descriptor()), 0,
                                   Region({{0, 1}, {0, 2}}))
                   .ok(),
              "huge direct writer does not certify other coordinates");
    }
  }
}
void backing_independent_spatial_runs() {
  ResourceBudget root;
  SchemaTemplate schema;
  schema.id = "test.spatial.affine";
  ResultTensorSpec slot;
  slot.key = "samples";
  slot.descriptor = {ElementType::UInt8, {2, 4, 3}};
  slot.batch_axes = {1, 1};
  slot.layout.spatial = true;
  schema.tensors.push_back(slot);
  std::vector<std::uint8_t> bytes(24);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::uint8_t>(i);
  }
  auto storage = take(Value::create({ElementType::UInt8, {24}},
                                    Region::whole({24}), {0, {1}}, bytes))
                     .storage();
  for (bool affine : {false, true}) {
    auto builder = take(ResultBuilder::start(root, schema, "spatial.runs"));
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
                .ok(),
            "spatial runs basis");
    auto witness = take(ResultRelation::cartesian(root, 24, {0, 1, 0, 0}));
    auto status =
        affine ? builder.publish_tensor(0, Region::whole(slot.sample_shape()),
                                        {0, {0, 0, 12, 3, 1}}, storage, witness,
                                        {true, true, true, true})
               : builder.publish_tensor(0, Region::whole(slot.sample_shape()),
                                        ByteView(bytes.data(), bytes.size()),
                                        witness, {true, true, true, true});
    require(status.ok(), "both physical backings publish same tensor");
    auto result = take(builder.seal());
    auto window = take(result.acquire_tensor(
        take(result.descriptor()), 0, Region::whole(slot.sample_shape())));
    auto rectangle = take(window.rectangle_run({0, 0, 0, 0, 1}));
    require(window.sample_axis() == 3 &&
                window.row_axis() == std::optional<std::uint32_t>{2} &&
                rectangle.row.samples == 4 && rectangle.rows == 2,
            "logical spatial run axes are independent of backing");
    const auto last = rectangle.row.data +
                      3 * rectangle.row.sample_stride_bytes +
                      rectangle.row_stride_bytes;
    require(
        *rectangle.row.data == 1 && *last == 22,
        "affine and planar rectangle spans read same spatial/channel samples");
  }
  schema.tensors[0].batch_axes[0] = 2;
  const auto shape = schema.tensors[0].sample_shape();
  auto buffer = take(root.allocator().allocate(48));
  for (std::uint64_t i = 0; i < 48; ++i)
    buffer.data()[i] = static_cast<std::uint8_t>(i);
  auto multi =
      take(ResultBuilder::start(root, schema, "spatial.affine.batches"));
  require(multi
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
              .ok(),
          "multiple affine batch basis");
  require(multi
              .publish_tensor(
                  0, Region::whole(shape), {0, {24, 0, 12, 3, 1}},
                  std::move(buffer).freeze(),
                  take(ResultRelation::cartesian(root, 48, {0, 1, 0, 0})),
                  {true, true, true, true})
              .ok(),
          "affine multi-batch publication");
  auto result = take(multi.seal());
  const auto facts = take(result.descriptor());
  auto whole = take(result.acquire_tensor(facts, 0, Region::whole(shape)));
  require(*take(whole.row_run({0, 0, 0, 0, 0})).data == 0 &&
              *take(whole.row_run({1, 0, 0, 0, 0})).data == 24,
          "affine full-batch window reads each logical batch address");
  auto narrow = take(result.acquire_tensor(
      facts, 0, Region({{0, 1}, {0, 1}, {0, 2}, {0, 4}, {0, 3}})));
  require(
      !narrow.row_run({1, 0, 0, 0, 0}).ok(),
      "affine window cannot expand beyond its captured batch authorization");
}
void window_failures() {
  ResourceBudget root;
  auto shape = schema(true);
  shape.publication = PublishPolicy::StablePrefix;
  shape.tensors[0].batch_axes[0] = shape.tensors[0].batch_axes[1] = 1;
  shape.tensors[0].descriptor.shape = {2, 2};
  const Region left({{0, 1}, {0, 1}, {0, 2}, {0, 1}});
  const Region right({{0, 1}, {0, 1}, {0, 2}, {1, 1}});
  auto builder = take(ResultBuilder::start(root, shape, "concurrent.windows"));
  auto basis = take(ResultRelation::cartesian(root, 1, {0, 1, 0, 0}));
  require(builder.bind_descriptor_relation(basis).ok(), "concurrent basis");
  auto relation = take(ResultRelation::cartesian(root, 4, {0, 1, 0, 0}));
  const float samples[] = {.1F, .2F};
  require(builder
              .publish_tensor(
                  0, left,
                  ByteView(reinterpret_cast<const std::uint8_t*>(samples), 8),
                  relation, {true, true, true, true})
              .ok(),
          "concurrent prefix");
  auto reference = builder.reference();
  auto facts = take(reference.descriptor(false));
  std::mutex mutex;
  std::condition_variable ready;
  bool writing = false, release = false;
  auto writer = std::async(std::launch::async, [&] {
    return builder.publish_tensor_kernel(
        0, right,
        [&](const auto&) {
          std::unique_lock<std::mutex> lock(mutex);
          writing = true;
          ready.notify_all();
          ready.wait(lock, [&] { return release; });
          return Status{ErrorCode::OperationFailed,
                        "deliberate writer rollback"};
        },
        relation, {true, true, true, true});
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    ready.wait(lock, [&] { return writing; });
  }
  CancellationSource cancelled;
  cancelled.cancel();
  auto reader = std::async(std::launch::async, [&] {
    return reference.acquire_tensor(facts, 0, left, cancelled.token());
  });
  auto prefix_reader = std::async(std::launch::async, [&] {
    return reference.acquire_tensor(facts, 0, left);
  });
  const auto responsive =
      reader.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  const auto prefix_responsive =
      prefix_reader.wait_for(std::chrono::seconds(1)) ==
      std::future_status::ready;
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
    ready.notify_all();
  }
  auto read_result = reader.get();
  auto prefix_result = prefix_reader.get();
  auto write_result = writer.get();
  require(responsive && read_result.status().code == ErrorCode::Cancelled,
          "pre-cancelled reader fails while writer transaction is active");
  require(prefix_responsive && prefix_result.ok(),
          "certified prefix remains readable during an active writer callback");
  require(
      write_result.code == ErrorCode::OperationFailed && !builder.seal().ok(),
      "failed kernel remains sticky");
  require(reference.acquire_tensor(facts, 0, left).ok(),
          "earlier certified prefix survives failed write");
  require(!reference.acquire_tensor(facts, 0, right).ok(),
          "failed transaction publishes no coverage");

  for (bool view : {false, true}) {
    builder = take(ResultBuilder::start(
        root, shape, view ? "sticky.view.cancel" : "sticky.kernel.cancel"));
    require(builder.bind_descriptor_relation(basis).ok(), "cancel basis");
    auto window = take(reference.acquire_tensor(facts, 0, left));
    CancellationSource cancelled;
    cancelled.cancel();
    const auto failure =
        view ? builder.publish_tensor_view(0, left, {&window}, relation,
                                           {true, true, true, true},
                                           cancelled.token())
             : builder.publish_tensor_kernel(
                   0, left, [](const auto&) { return Status::success(); },
                   relation, {true, true, true, true}, cancelled.token());
    require(
        failure.code == ErrorCode::Cancelled &&
            builder.publish_tensor(
                       0, left,
                       ByteView(reinterpret_cast<const std::uint8_t*>(samples),
                                8),
                       relation, {true, true, true, true})
                    .code == ErrorCode::Cancelled &&
            !builder.seal().ok(),
        "pre-cancelled image publication stays sticky without blocking "
        "cancellation");
  }
  builder = take(ResultBuilder::start(root, shape, "sticky.slot"));
  require(builder.bind_descriptor_relation(basis).ok(), "sticky basis");
  const auto bad_slot = builder.publish_tensor(
      1, left, ByteView(reinterpret_cast<const std::uint8_t*>(samples), 8),
      relation, {true, true, true, true});
  require(!bad_slot.ok() &&
              builder.publish_tensor(
                         0, left,
                         ByteView(
                             reinterpret_cast<const std::uint8_t*>(samples), 8),
                         relation, {true, true, true, true})
                      .code == bad_slot.code &&
              !builder.seal().ok(),
          "invalid slot is sticky");
  builder = take(ResultBuilder::start(root, shape, "sticky.allocation"));
  require(builder.bind_descriptor_relation(basis).ok(), "allocation basis");
  const auto failed = builder.publish_tensor_kernel(
      0, left, [](const auto&) -> Status { throw std::bad_alloc(); }, relation,
      {true, true, true, true});
  require(failed.code == ErrorCode::ResourceExhausted &&
              builder.publish_tensor(
                         0, left,
                         ByteView(
                             reinterpret_cast<const std::uint8_t*>(samples), 8),
                         relation, {true, true, true, true})
                      .code == failed.code &&
              !builder.seal().ok(),
          "kernel allocation failure is sticky");
}
void windows_and_views() {
  Driver source;
  auto original = image(source.root);
  auto facts = take(original.descriptor());
  const Region selected({{1, 1}, {0, 1}, {0, 3}, {0, 5}, {2, 1}});
  auto window = take(original.acquire_tensor(facts, 0, selected));
  auto first = take(window.row_run({1, 0, 0, 0, 2}));
  auto rectangle = take(window.rectangle_run({1, 0, 0, 0, 2}));
  require(first.samples == 5 && rectangle.rows == 3,
          "zero-copy bounded rectangle");
  require(!window.row_run({1, 0, 0, 0, 1}).ok(), "window channel authority");
  require(!window.row_run({0, 0, 0, 0, 2}).ok(), "window frame authority");
  Driver destination;
  auto output_schema = original.schema();
  auto& output = output_schema.tensors[0];
  output.descriptor.shape = {3, 5};
  output.layout.channel_axis.reset();
  output.facets.clear();
  const auto count = take(output.sample_count());
  auto builder =
      take(ResultBuilder::start(destination.root, output_schema, "view.test"));
  require(builder
              .bind_descriptor_relation(take(
                  ResultRelation::cartesian(destination.root, 1, {0, 1, 0, 0})))
              .ok(),
          "view descriptor");
  auto relation =
      take(ResultRelation::cartesian(destination.root, count, {0, 1, 0, 0}));
  const Region target({{1, 1}, {0, 1}, {0, 3}, {0, 5}});
  const auto ambient_live = source.root.statistics().live.values;
  {
    ResourceAllocationScope foreign(source.root);
    require(builder
                .publish_tensor_view(0, target, {&window}, relation,
                                     {true, true, true, true})
                .ok(),
            "cross-budget channel view");
  }
  require(source.root.statistics().live.values == ambient_live,
          "destination view metadata does not charge ambient source root");
  auto result = take(builder.seal());
  auto alias =
      take(result.acquire_tensor(take(result.descriptor()), 0, target));
  auto mapped = take(alias.row_run({1, 0, 0, 0}));
  require(mapped.data == first.data &&
              alias.storage_owner_token() == window.storage_owner_token(),
          "view preserves actual payload pointer and root");
  original = {};
  window = {};
  source.context.reset();
  require(read(result, {1, 0, 2, 4}) > 0,
          "view retains source after context retirement");
  alias = {};
  result = {};

  // Several publications may mix immutable view blocks and materialized pages.
  original = image(destination.root);
  facts = take(original.descriptor());
  auto left = take(original.acquire_tensor(
      facts, 0, Region({{0, 1}, {0, 1}, {0, 3}, {0, 2}, {1, 1}})));
  auto right = take(original.acquire_tensor(
      facts, 0, Region({{0, 1}, {0, 1}, {0, 3}, {3, 2}, {1, 1}})));
  builder =
      take(ResultBuilder::start(destination.root, output_schema, "mixed.test"));
  require(builder
              .bind_descriptor_relation(take(
                  ResultRelation::cartesian(destination.root, 1, {0, 1, 0, 0})))
              .ok(),
          "mixed descriptor");
  require(builder
              .publish_tensor_view(0, Region({{0, 1}, {0, 1}, {0, 3}, {0, 2}}),
                                   {&left}, relation, {true, true, true, true})
              .ok(),
          "left view");
  require(builder
              .publish_tensor_view(0, Region({{0, 1}, {0, 1}, {0, 3}, {3, 2}}),
                                   {&right}, relation, {true, true, true, true})
              .ok(),
          "right view");
  require(builder
              .publish_tensor_kernel(
                  0, Region({{0, 1}, {0, 1}, {0, 3}, {2, 1}}),
                  [](const auto& writers) {
                    for (std::uint64_t y = 0; y < 3; ++y) {
                      auto run = writers[0].row_run({0, 0, y, 2});
                      if (!run.ok()) {
                        return run.status();
                      }
                      const float value = 42;
                      std::memcpy(run.value().data, &value, 4);
                    }
                    return Status::success();
                  },
                  relation, {true, true, true, true})
              .ok(),
          "direct transactional image write");
  result = take(builder.seal());
  alias = take(result.acquire_tensor(take(result.descriptor()), 0,
                                     Region({{0, 1}, {0, 1}, {0, 3}, {0, 5}})));
  require(take(alias.row_run({0, 0, 0, 0})).samples == 2 &&
              take(alias.row_run({0, 0, 0, 2})).samples == 1 &&
              take(alias.row_run({0, 0, 0, 3})).samples == 2,
          "mixed window stops at publication boundaries");
  require(read(result, {0, 0, 1, 2}) == 42, "mixed materialized sample");
  auto cancelled = CancellationSource{};
  cancelled.cancel();
  require(!result
               .acquire_tensor(take(result.descriptor()), 0,
                               Region({{0, 1}, {0, 1}, {0, 3}, {0, 5}}),
                               cancelled.token())
               .ok(),
          "window acquisition cancellation");
}
void spatial_backing_boundaries() {
  for (auto order : {ImagePlaneOrder::Continuous, ImagePlaneOrder::Tiled}) {
    ResourceBudget root;
    SchemaTemplate spec;
    spec.id = "test.spatial.physical";
    ResultTensorSpec tensor;
    tensor.key = "samples";
    tensor.descriptor = {ElementType::UInt8, {5, 19, 3}};
    tensor.layout.spatial = true;
    tensor.layout.order = order;
    tensor.layout.row_pitch_bytes =
        order == ImagePlaneOrder::Continuous ? 32 : 0;
    spec.tensors.push_back(tensor);
    auto builder =
        take(ResultBuilder::start(root, spec, "physical.roi", {}, {}, 2, 8));
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(root, 1, {})))
                .ok(),
            "physical descriptor");
    const Region roi({{1, 3}, {3, 14}, {0, 3}});
    require(
        builder
            .publish_tensor_kernel(
                0, roi,
                [&](const auto& windows) {
                  const auto& window = windows.at(0);
                  for (std::uint64_t c = 0; c < 3; ++c)
                    for (std::uint64_t y = 1; y < 4; ++y)
                      for (std::uint64_t x = 3; x < 17;) {
                        auto rectangle = take(window.rectangle_run({y, x, c}));
                        const auto columns =
                            order == ImagePlaneOrder::Continuous
                                ? 17 - x
                                : std::min<std::uint64_t>(17 - x, 8 - x % 8);
                        const auto rows =
                            order == ImagePlaneOrder::Continuous
                                ? 4 - y
                                : std::min<std::uint64_t>(4 - y, 2 - y % 2);
                        require(
                            rectangle.row.samples == columns &&
                                rectangle.rows == rows &&
                                rectangle.row_stride_bytes ==
                                    (order == ImagePlaneOrder::Continuous ? 32
                                                                          : 8),
                            "physical runs stop at ROI and tile edges without "
                            "padding");
                        for (std::uint64_t dx = 0; dx < columns; ++dx)
                          rectangle.row.data[dx] = static_cast<std::uint8_t>(
                              y * 40 + (x + dx) * 2 + c);
                        x += columns;
                      }
                  return Status::success();
                },
                take(ResultRelation::cartesian(root, 285, {})),
                {true, true, true, true})
            .ok(),
        "physical ROI publication");
    auto result = take(builder.seal());
    auto descriptor = take(result.descriptor());
    auto window = take(result.acquire_tensor(descriptor, 0, roi));
    for (std::uint64_t c = 0; c < 3; ++c)
      for (std::uint64_t y = 1; y < 4; ++y)
        for (std::uint64_t x = 3; x < 17; ++x) {
          std::uint8_t value = 0;
          require(
              result.read_tensor(descriptor, 0, {y, x, c}, &value, 1).ok() &&
                  value == static_cast<std::uint8_t>(y * 40 + x * 2 + c),
              "physical sample oracle");
        }
    require(
        !result.acquire_tensor(descriptor, 0, Region({{0, 1}, {0, 1}, {0, 3}}))
             .ok(),
        "unpublished padding stays unavailable");
  }
  ResourceBudget root;
  SchemaTemplate spec;
  spec.id = "test.spatial.rollback";
  spec.publication = PublishPolicy::StablePrefix;
  ResultTensorSpec tensor;
  tensor.key = "samples";
  constexpr std::uint64_t width = 16384 * 1027;
  tensor.descriptor = {ElementType::UInt8, {2, width}};
  tensor.layout.spatial = true;
  tensor.layout.channel_axis.reset();
  tensor.layout.order = ImagePlaneOrder::Continuous;
  spec.tensors.push_back(tensor);
  auto builder = take(ResultBuilder::start(root, spec, "rollback.pages"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(root, 1, {})))
              .ok(),
          "rollback descriptor");
  auto relation = take(ResultRelation::cartesian(root, 2 * width, {}));
  for (std::uint64_t y = 0; y < 2; ++y)
    require(builder
                .publish_tensor_kernel(
                    0, Region({{y, 1}, {0, 1}}),
                    [&](const auto& windows) {
                      take(windows.at(0).row_run({y, 0})).data[0] =
                          static_cast<std::uint8_t>(123 + y);
                      return Status::success();
                    },
                    relation, {true, true, true, true})
                .ok(),
            "retained page prefix");
  auto captured = take(builder.reference().capture());
  auto descriptor = take(captured.descriptor(false));
  const auto retained = root.statistics().live[ResourceKind::Payload];
  bool wrote = false;
  auto failed = builder.publish_tensor_kernel(
      0, Region({{0, 2}, {1, width - 1}}),
      [&](const auto& windows) {
        take(windows.at(0).row_run({1, width - 1})).data[0] = 99;
        wrote = true;
        return Status{ErrorCode::Cancelled,
                      "rollback large unpublished page run"};
      },
      relation, {true, true, true, true});
  require(wrote && failed.code == ErrorCode::Cancelled &&
              root.statistics().live[ResourceKind::Payload] == retained,
          "failed large page transaction releases fresh backing only");
  for (std::uint64_t y = 0; y < 2; ++y) {
    std::uint8_t value = 0;
    require(captured.read_tensor(descriptor, 0, {y, 0}, &value, 1).ok() &&
                value == 123 + y,
            "captured prefix survives page rollback");
  }
  std::uint8_t unpublished = 0;
  require(!captured.read_tensor(descriptor, 0, {1, width - 1}, &unpublished, 1)
               .ok(),
          "failed transaction does not authorize written endpoint");
}
}  // namespace
int main() {
  try {
    numerical_tensor_windows();
    spatial_tensor_batches();
    indexed_batch_resources();
    tensor_writer_reentrancy();
    empty_tensor_writers();
    lazy_spatial_backing();
    direct_tensor_writers();
    backing_independent_spatial_runs();
    window_failures();
    windows_and_views();
    spatial_backing_boundaries();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
