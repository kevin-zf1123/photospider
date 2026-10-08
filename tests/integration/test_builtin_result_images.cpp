#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
template <class T>
T take(Result<T> result) {
  if (!result.ok()) {
    throw std::runtime_error(result.status().message);
  }
  return result.take_value();
}
void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}
SchemaTemplate schema(bool mask = false) {
  SchemaTemplate schema;
  schema.id = "photospider.image";
  ResultTensorSpec slot;
  slot.batch_axes = {1, 1};
  slot.layout.spatial = true;
  slot.key = "pixels";
  slot.batch_axes[0] = slot.batch_axes[1] = 2;
  slot.descriptor = {ElementType::Float32,
                     mask ? std::vector<std::uint64_t>{3, 5}
                          : std::vector<std::uint64_t>{3, 5, 4}};
  if (mask) {
    slot.layout.channel_axis.reset();
  }
  slot.facets = {
      take(encode_semantic(mask ? coverage_semantics() : rgba_semantics()))};
  schema.tensors.push_back(std::move(slot));
  return schema;
}
ResultRef image(const ResourceBudget& root, bool mask = false, float bias = 0) {
  const auto s = schema(mask);
  auto builder = take(ResultBuilder::start(root, s, "test.source"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(root, 1, {0, 1, 0, 0})))
              .ok(),
          "source descriptor");
  const auto shape = s.tensors[0].sample_shape();
  const auto count = take(s.tensors[0].sample_count());
  std::vector<float> pixels(count);
  for (std::uint64_t i = 0; i < count; ++i) {
    pixels[i] = mask ? .25F : i % 4 == 3 ? .5F : bias + (i / 4) * .125F;
  }
  require(builder
              .publish_tensor(
                  0, Region::whole(shape),
                  ByteView(reinterpret_cast<const std::uint8_t*>(pixels.data()),
                           pixels.size() * 4),
                  take(ResultRelation::cartesian(root, count, {0, 1, 0, 0})),
                  {true, true, true, true})
              .ok(),
          "source publication");
  return take(builder.seal());
}
ResultRef scalar(const ResourceBudget& root, float number) {
  SchemaTemplate schema;
  schema.id = "test.scalar";
  ResultTensorSpec tensor;
  tensor.key = "control";
  tensor.descriptor = {ElementType::Float32, {1}};
  schema.tensors.push_back(tensor);
  auto builder = take(ResultBuilder::start(root, schema, "test.control"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
              .ok(),
          "control descriptor");
  auto bytes = take(root.allocator().allocate(5));
  std::memcpy(bytes.data() + 1, &number, 4);
  require(builder
              .publish_tensor(
                  0, Region::whole({1}), {1, {INT64_MIN}},
                  std::move(bytes).freeze(),
                  take(ResultRelation::cartesian(root, 1, {0, 1, 0, 0})),
                  {true, true, true, true})
              .ok(),
          "control publication");
  return take(builder.seal());
}
struct Driver {
  std::shared_ptr<OperationRegistry> registry =
      make_default_operation_registry();
  ExecutionContextConfig config;
  std::unique_ptr<ExecutionContext> context;
  ResourceBudget root;
  bool native = false;
  explicit Driver(bool gpu = false) : native(gpu) {
    config.gpu_enabled = gpu;
    config.managed_resources = ResourceLimits{};
    context = std::make_unique<ExecutionContext>(registry, config);
    root = take(context->resource_budget());
  }
  struct Run {
    std::shared_ptr<GraphContext> graph;
    ExecutionPlan plan;
    ExecutionBindings bindings;
  };
  Run prepare(const std::string& operation,
              std::vector<ExecutionBinding> inputs,
              std::map<std::string, ParameterValue> parameters = {},
              const std::string& output = "value") {
    WorkflowDocument document;
    WorkflowNode node;
    node.id = 1;
    node.operation = operation;
    node.parameters = std::move(parameters);
    for (unsigned i = 0; i < inputs.size(); ++i) {
      WorkflowInputDeclaration declaration;
      declaration.id = i + 1;
      declaration.name = inputs[i].name;
      if (inputs[i].result.valid()) {
        declaration.result_schema =
            std::make_shared<SchemaTemplate>(inputs[i].result.schema());
      }
      document.inputs.push_back(std::move(declaration));
      node.inputs.push_back(WorkflowInputReference{i + 1});
    }
    document.nodes.push_back(std::move(node));
    document.outputs = {{"out", 1, output}};
    auto graph = std::make_shared<GraphContext>(document);
    PlanningOptions planning;
    planning.execution_mode =
        native ? ExecutionMode::NativeGpu : ExecutionMode::CpuExact;
    auto compiled = take(Compiler(registry).compile(*graph, planning));
    return {std::move(graph), std::move(compiled.plan), {std::move(inputs)}};
  }
  ResultRef run(const std::string& operation,
                std::vector<ExecutionBinding> inputs,
                std::map<std::string, ParameterValue> parameters = {},
                const std::string& output = "value") {
    auto prepared =
        prepare(operation, std::move(inputs), std::move(parameters), output);
    return take(context->execute(prepared.plan, prepared.bindings))
        .results.at("out");
  }
};
float read(const ResultRef& result, std::vector<std::uint64_t> coordinate) {
  float value = 0;
  require(
      result.read_tensor(take(result.descriptor()), 0, coordinate, &value, 4)
          .ok(),
      "read image output");
  return value;
}
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
struct ScalarPreludeState {
  ScalarPreludeState(bool input, float number)
      : consumer(input), value(number) {}
  bool consumer;
  float value;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (consumer) {
      auto status = phase.tensors->at({0, 2}).read({0}, &value, sizeof(value),
                                                   phase.query.cancellation);
      if (!status.ok())
        return Result<ResultProgramPoll>(status);
    }
    auto started =
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key);
    if (!started.ok())
      return Result<ResultProgramPoll>(started.status());
    auto builder = started.take_value();
    auto descriptor = ResultRelation::cartesian(
        phase.resources, 1, {0, 8, 0, 0, ResultSupportTarget::Descriptor, 0});
    if (!descriptor.ok())
      return Result<ResultProgramPoll>(descriptor.status());
    auto status = builder.bind_descriptor_relation(descriptor.take_value());
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    auto relation = ResultRelation::cartesian(
        phase.resources, 1,
        {0, 1, 0, consumer ? 1U : 0U, ResultSupportTarget::Tensor,
         consumer ? 2U : 0U});
    if (!relation.ok())
      return Result<ResultProgramPoll>(relation.status());
    for (uint32_t slot = 0;
         slot < phase.query.output.result_schema->tensors.size(); ++slot) {
      if (consumer) {
        // A real output view pins the completed producer in the weak sharing
        // directory. Merely retaining its dependency facts does not pin bytes.
        auto window = phase.tensors->at({0, 2}).acquire(
            Region::whole({1}), phase.query.cancellation);
        if (!window.ok())
          return Result<ResultProgramPoll>(window.status());
        ResultTensorViewTransform transform;
        transform.source_axes = {{0, 0, 1, 1}};
        status = builder.publish_tensor_view(
            slot, Region::whole({1}), window.value(), transform,
            relation.value(), {true, true, true, true});
      } else {
        status = builder.publish_tensor(
            slot, Region::whole({1}),
            ByteView(reinterpret_cast<const uint8_t*>(&value), sizeof(value)),
            relation.value(), {true, true, true, true});
      }
      if (!status.ok())
        return Result<ResultProgramPoll>(status);
    }
    auto result = builder.seal();
    return result.ok() ? Result<ResultProgramPoll>(
                             ResultPublication{result.take_value(), true})
                       : Result<ResultProgramPoll>(result.status());
  }
};
void reshape_relations() {
  ResourceBudget root;
  const std::vector<std::vector<uint64_t>> shapes{
      {24},   {2, 12},   {3, 8},    {4, 6},
      {6, 4}, {2, 3, 4}, {4, 2, 3}, {1, 2, 1, 12}};
  uint64_t random = 17;
  const auto next = [&] {
    random = random * 6364136223846793005ULL + 1;
    return random;
  };
  for (const auto& output_shape : shapes)
    for (const auto& input_shape : shapes) {
      auto relation = take(ResultRelation::reshape(
          root, output_shape, Region::whole(output_shape), input_shape,
          Region::whole(input_shape),
          {0, 5, 0, 0, ResultSupportTarget::Tensor, 0}));
      for (unsigned trial = 0; trial < 12; ++trial) {
        std::vector<RegionDimension> dims;
        for (auto n : output_shape) {
          const auto first = next() % n;
          dims.push_back({first, 1 + next() % (n - first)});
        }
        auto request =
            take(Footprint::from_regions(output_shape, {Region(dims)}));
        std::vector<Region> expected_boxes;
        require(
            request
                .visit(
                    [&](const auto& at) {
                      uint64_t ordinal = 0;
                      for (size_t i = 0; i < at.size(); ++i)
                        ordinal = ordinal * output_shape[i] + at[i];
                      std::vector<RegionDimension> source(input_shape.size());
                      for (size_t i = input_shape.size(); i-- > 0;) {
                        source[i] = {ordinal % input_shape[i], 1};
                        ordinal /= input_shape[i];
                      }
                      expected_boxes.emplace_back(std::move(source));
                      return Status::success();
                    },
                    1000)
                .ok(),
            "reshape scalar projection oracle");
        auto expected =
            take(Footprint::from_regions(input_shape, expected_boxes));
        Footprint actual;
        require(relation.project(request,
                                 [&](auto support, const auto* samples) {
                                   require(
                                       samples &&
                                           support.target ==
                                               ResultSupportTarget::Tensor &&
                                           support.roles == 5,
                                       "reshape typed support");
                                   actual = *samples;
                                   return Status::success();
                                 })
                        .ok() &&
                    actual == expected,
                "reshape compact projection equals independent ordinal oracle");
        auto full = take(Footprint::all(output_shape));
        auto inverse = take(relation.preimage(
            full, {0, 4, 0, 0, ResultSupportTarget::Tensor, 0}, expected));
        require(inverse == request && relation.certify(request).ok(),
                "reshape compact inverse and structural witness");
      }
    }
  const std::vector<uint64_t> source_shape{4, 5}, output_shape{2, 3};
  const Region source({{1, 2}, {1, 3}}), witness({{1, 1}, {0, 3}});
  auto cropped = take(
      ResultRelation::reshape(root, output_shape, witness, source_shape, source,
                              {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto request =
      take(Footprint::from_regions(output_shape, {Region({{1, 1}, {0, 1}})}));
  auto expected =
      take(Footprint::from_regions(source_shape, {Region({{2, 1}, {1, 1}})}));
  Footprint actual;
  require(cropped.project(request,
                          [&](auto, const auto* samples) {
                            actual = *samples;
                            return Status::success();
                          })
                  .ok() &&
              actual == expected,
          "reshape ROI keeps full output ordinal basis and source offsets");
  require(take(cropped.preimage(take(Footprint::all(output_shape)),
                                {0, 1, 0, 0, ResultSupportTarget::Tensor, 0},
                                expected)) == request,
          "reshape cropped inverse retains source offsets and output witness");
  auto outside =
      take(Footprint::from_regions(output_shape, {Region({{0, 1}, {0, 1}})}));
  require(cropped.certify(outside).code == ErrorCode::NotFound,
          "reshape witness cannot certify unrequested output samples");
  const std::vector<uint64_t> huge_source{UINT64_MAX, 6},
      huge_output{UINT64_MAX, 2, 3};
  auto huge = take(
      ResultRelation::reshape(root, huge_output, Region::whole(huge_output),
                              huge_source, Region::whole(huge_source),
                              {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto columns = take(Footprint::from_regions(
      huge_output, {Region({{0, UINT64_MAX}, {0, 2}, {1, 1}})}));
  auto source_columns = take(Footprint::from_regions(
      huge_source,
      {Region({{0, UINT64_MAX}, {1, 1}}), Region({{0, UINT64_MAX}, {4, 1}})}));
  FootprintLimits limits;
  limits.maximum_work = 5000;
  require(huge.project(
                  columns,
                  [&](auto, const auto* samples) {
                    actual = *samples;
                    return Status::success();
                  },
                  limits)
                  .ok() &&
              actual == source_columns,
          "huge shared reshape axis projects without row enumeration");
  require(
      take(huge.preimage(columns, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0},
                         source_columns, limits)) == columns &&
          huge.certify(columns, limits).ok(),
      "huge reshape inverse and certification do not flatten the domain");
  constexpr uint64_t large = 1000000;
  for (bool reverse : {false, true}) {
    const std::vector<uint64_t> input = reverse
                                            ? std::vector<uint64_t>{2, large}
                                            : std::vector<uint64_t>{large, 2};
    const std::vector<uint64_t> output = reverse
                                             ? std::vector<uint64_t>{large, 2}
                                             : std::vector<uint64_t>{2, large};
    const Region queried =
        reverse ? Region({{0, large}, {0, 1}}) : Region({{0, 1}, {0, 1}});
    const Region changed_region =
        reverse ? Region({{0, 1}, {0, 1}}) : Region({{0, large}, {0, 1}});
    auto limited = take(ResultRelation::reshape(
        root, output, queried, input, Region::whole(input),
        {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
    auto q = take(Footprint::from_regions(output, {queried}));
    auto changes = take(Footprint::from_regions(input, {changed_region}));
    auto bound = limits;
    bound.maximum_boxes = 16;
    require(
        take(limited.preimage(q, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0},
                              changes, bound)) ==
            take(Footprint::from_regions(output, {Region({{0, 1}, {0, 1}})})),
        "reshape dirty chooses the bounded direction for large stripe and "
        "singleton intersections");
    require(take(limited.preimage(take(Footprint::none(output)),
                                  {0, 1, 0, 0, ResultSupportTarget::Tensor, 0},
                                  changes, bound))
                .empty(),
            "empty reshape demand does not expand changed stripes");
    auto empty_witness = take(ResultRelation::reshape(
        root, output,
        Region(std::vector<RegionDimension>(output.size(), {0, 0})), input,
        Region::whole(input), {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
    require(take(empty_witness.preimage(
                     q, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}, changes,
                     bound))
                .empty(),
            "empty reshape witness has no dirty sample work");
  }
  auto partial = take(ResultRelation::reshape(
      root, {large, 2}, Region::whole({large, 2}), {2, large},
      Region::whole({2, large}), {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto half_column = take(
      Footprint::from_regions({large, 2}, {Region({{0, large / 2}, {0, 1}})}));
  auto first_row =
      take(Footprint::from_regions({2, large}, {Region({{0, 1}, {0, large}})}));
  auto bounded_projection = limits;
  bounded_projection.maximum_boxes = 16;
  require(take(partial.preimage(half_column,
                                {0, 1, 0, 0, ResultSupportTarget::Tensor, 0},
                                first_row, bounded_projection)) == half_column,
          "reshape dirty uses compact row inverse despite a smaller scattered "
          "output cardinality");
  auto whole_column =
      take(Footprint::from_regions({large, 2}, {Region({{0, large}, {0, 1}})}));
  require(
      take(partial.preimage(
          whole_column, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0},
          take(Footprint::all({2, large})), bounded_projection)) ==
          whole_column,
      "whole changed reshape domain does not expand a scattered output query");
  ResourceBudget direct_work, callback_work;
  auto direct_relation = take(ResultRelation::reshape(
      direct_work, {2, 3}, Region::whole({2, 3}), {3, 2}, Region::whole({3, 2}),
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto callback_relation = take(ResultRelation::reshape(
      callback_work, {2, 3}, Region::whole({2, 3}), {3, 2},
      Region::whole({3, 2}), {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto charged_query =
      take(Footprint::from_regions({2, 3}, {Region({{0, 2}, {1, 1}})}));
  uint64_t reported = 0;
  auto callback_limits = limits;
  callback_limits.consume_work = [&](uint64_t n) {
    reported += n;
    return callback_work.consume({n});
  };
  require(
      direct_relation
              .project(
                  charged_query,
                  [](auto, const auto*) { return Status::success(); }, limits)
              .ok() &&
          callback_relation
              .project(
                  charged_query,
                  [](auto, const auto*) { return Status::success(); },
                  callback_limits)
              .ok() &&
          reported > 0 &&
          direct_work.statistics().issued.work ==
              callback_work.statistics().issued.work,
      "reshape host callback precharges each work unit exactly once");
  const std::vector<uint64_t> wide(8, UINT64_MAX);
  auto extreme = take(ResultRelation::reshape(
      root, wide, Region::whole(wide), wide, Region::whole(wide),
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  std::vector<RegionDimension> final_point(8, {UINT64_MAX - 1, 1});
  auto point = take(Footprint::from_regions(wide, {Region(final_point)}));
  require(extreme.project(
                     point,
                     [&](auto, const auto* samples) {
                       actual = *samples;
                       return Status::success();
                     },
                     limits)
                  .ok() &&
              actual == point,
          "rank8 reshape retains complete 512-bit coordinate identity");
  const std::vector<uint64_t> reassociated_input{UINT64_MAX, UINT64_MAX,
                                                 UINT64_MAX, 2},
      reassociated_output{2, UINT64_MAX, UINT64_MAX, UINT64_MAX};
  auto reassociated = take(ResultRelation::reshape(
      root, reassociated_output, Region::whole(reassociated_output),
      reassociated_input, Region::whole(reassociated_input),
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto middle_output = take(Footprint::from_regions(
      reassociated_output, {Region({{1, 1}, {0, 1}, {0, 1}, {0, 1}})}));
  auto middle_input = take(
      Footprint::from_regions(reassociated_input, {Region({{UINT64_MAX / 2, 1},
                                                           {UINT64_MAX / 2, 1},
                                                           {UINT64_MAX / 2, 1},
                                                           {1, 1}})}));
  require(reassociated
                  .project(
                      middle_output,
                      [&](auto, const auto* samples) {
                        actual = *samples;
                        return Status::success();
                      },
                      limits)
                  .ok() &&
              actual == middle_input,
          "reshape reassociation uses ordinals exceeding 128 bits");
  require(take(reassociated.preimage(
              middle_output, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0},
              middle_input, limits)) == middle_output,
          "wide reassociation inverse retains complete coordinates");
  auto tight = limits;
  tight.maximum_boxes = 1;
  bool visited = false;
  require(huge.project(
                  columns,
                  [&](auto, const auto*) {
                    visited = true;
                    return Status::success();
                  },
                  tight)
                      .code == ErrorCode::ResourceExhausted &&
              !visited,
          "reshape holes exceeding rectangle budget never emit a bounding-box "
          "support");
  ResourceLimits unavailable;
  unavailable.capacity[ResourceKind::Entries] = 0;
  ResourceBudget denied(unavailable);
  auto exhausted = ResultRelation::reshape(
      denied, {3, 2}, Region::whole({3, 2}), {2, 3}, Region::whole({2, 3}),
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0});
  require(!exhausted.ok() &&
              exhausted.status().code == ErrorCode::ResourceExhausted &&
              denied.statistics().live[ResourceKind::Metadata] == 0,
          "reshape constructor resource failure releases temporary metadata");
  require(!ResultRelation::reshape(root, {3}, Region::whole({3}), {2},
                                   Region::whole({2}),
                                   {0, 1, 0, 0, ResultSupportTarget::Tensor, 0})
               .ok(),
          "reshape cardinality mismatch is invalid");
  ResourceBudget measurement;
  auto measured = take(ResultRelation::reshape(
      measurement, {2, 3}, Region::whole({2, 3}), {3, 2}, Region::whole({3, 2}),
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  ResourceLimits tight_metadata;
  tight_metadata.capacity[ResourceKind::Metadata] =
      measurement.statistics().live[ResourceKind::Metadata] + 1024;
  ResourceBudget constrained(tight_metadata);
  auto bounded = take(ResultRelation::reshape(
      constrained, {2, 3}, Region::whole({2, 3}), {3, 2}, Region::whole({3, 2}),
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto touched =
      take(Footprint::from_regions({3, 2}, {Region({{0, 3}, {1, 1}})}));
  const auto capacity_before = constrained.statistics().live.values;
  auto limited_inverse =
      bounded.preimage(take(Footprint::all({2, 3})),
                       {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}, touched);
  require(!limited_inverse.ok() &&
              limited_inverse.status().code == ErrorCode::ResourceExhausted &&
              constrained.statistics().live.values == capacity_before,
          "reshape inverse metadata admission is typed and releases all "
          "temporary boxes");
  CancellationSource cancelled;
  cancelled.cancel();
  limits.cancellation = cancelled.token();
  require(huge.project(
                  columns, [](auto, const auto*) { return Status::success(); },
                  limits)
                  .code == ErrorCode::Cancelled,
          "reshape projection observes cancellation");
}
void repeated_tensor_shapes() {
  SchemaTemplate schema;
  schema.id = "test.repeated";
  ResultTensorSpec member;
  member.key = "samples";
  member.descriptor = {ElementType::UInt8, {3}};
  member.batch_axes = {2};
  schema.tensors.push_back(member);
  OperationPortConstraint constraint;
  constraint.kind = OperationPortKind::Result;
  constraint.tensor_key = "samples";
  OperationTraits traits;
  traits.input_count = 0;
  traits.input_schema = {constraint};
  traits.repeated_minimum = 1;
  traits.repeated_maximum = 4;
  traits.outputs[0].output_schema = constraint;
  traits.outputs[0].output_schema.result_schema_id = "test.repeated";
  traits.outputs[0].output_schema.result_schema_version = 1;
  traits.outputs[0].continuation_bytes = 1;
  traits.outputs[0].maximum_dependency_stages = 1;
  traits.outputs[0].result_schema = schema;
  traits.outputs[0].dependency_version = 2;
  traits.outputs[0].region_rule = OperationRegionRule::Dependency;
  auto resolved = take(resolve_operation_traits(traits, 2, {}));
  OperationMetadata first, second;
  first.result_schema = std::make_shared<SchemaTemplate>(schema);
  second.result_schema = first.result_schema;
  require(infer_operation_outputs(resolved, {first, second}, {}).ok(),
          "repeated matching Result sample domains infer successfully");
  schema.tensors[0].batch_axes = {4};
  second.result_schema = std::make_shared<SchemaTemplate>(schema);
  auto different = infer_operation_outputs(resolved, {first, second}, {});
  require(
      !different.ok() && different.status().code == ErrorCode::TypeMismatch,
      "repeated Result inputs include batch axes in homogeneous logical shape");
}
void scalar_prelude_workflows() {
  SchemaTemplate source_schema;
  source_schema.id = "test.scalar.members";
  for (const char* key : {"unused", "other", "gain"}) {
    ResultTensorSpec tensor;
    tensor.key = key;
    tensor.descriptor = {ElementType::Float32, {1}};
    source_schema.tensors.push_back(tensor);
  }
  auto output_schema = source_schema;
  output_schema.tensors.resize(1);
  auto registry = std::make_shared<OperationRegistry>();
  auto starts = std::make_shared<int>(0);
  auto producer_starts = std::make_shared<int>(0);
  for (int profile = 0; profile < 5; ++profile) {
    OperationDefinition operation;
    operation.key = profile == 0   ? "test.scalar.consumer"
                    : profile == 1 ? "test.scalar.nan"
                    : profile == 2 ? "test.scalar.range"
                    : profile == 3 ? "test.scalar.valid"
                                   : "test.scalar.loose";
    auto& traits = operation.traits;
    const bool consumer = profile == 0 || profile == 4;
    traits.input_count = consumer ? 2 : 0;
    traits.input_schema.clear();
    if (consumer) {
      OperationPortConstraint constraint;
      constraint.kind = OperationPortKind::Result;
      constraint.tensor_key = "gain";
      constraint.element_type = static_cast<uint32_t>(ElementType::Float32);
      constraint.scalar_bounds = true;
      constraint.maximum = profile == 4 ? 2 : 1;
      traits.input_schema = {constraint, constraint};
    }
    auto& output = traits.outputs[0];
    output.output_schema.kind = OperationPortKind::Result;
    output.output_schema.result_schema_id = "test.scalar.members";
    output.output_schema.result_schema_version = 1;
    output.result_schema = consumer ? output_schema : source_schema;
    output.dependency_version = 2;
    output.region_rule = OperationRegionRule::Dependency;
    output.input_indices =
        consumer ? std::vector<uint32_t>{0} : std::vector<uint32_t>{};
    output.continuation_bytes = sizeof(ScalarPreludeState);
    output.maximum_dependency_stages = 1;
    operation.start_result = [consumer, starts, producer_starts, profile](
                                 const ResultProgramQuery&,
                                 const BufferAllocator& allocator) {
      if (consumer)
        ++*starts;
      else
        ++*producer_starts;
      return ResultContinuation::make<ScalarPreludeState>(
          allocator, consumer,
          profile == 1   ? std::numeric_limits<float>::quiet_NaN()
          : profile == 2 ? 1.5F
                         : .5F);
    };
    require(registry->register_operation(std::move(operation)).ok(),
            "scalar fixture registration");
  }
  require(registry->freeze().ok(), "scalar fixture freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  const auto binding = [&](float value) {
    auto builder =
        take(ResultBuilder::start(root, source_schema, "scalar.source"));
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
                .ok(),
            "scalar source descriptor");
    auto buffer = take(root.allocator().allocate(sizeof(value)));
    std::memcpy(buffer.data(), &value, sizeof(value));
    auto storage = std::move(buffer).freeze();
    for (uint32_t slot = 0; slot < 3; ++slot)
      require(builder
                  .publish_tensor(
                      slot, Region::whole({1}), {0, {INT64_MIN}}, storage,
                      take(ResultRelation::cartesian(root, 1, {0, 1, 0, 0})),
                      {true, true, true, true})
                  .ok(),
              "scalar signed singleton source");
    ExecutionBinding input;
    input.name = "control";
    input.result = take(builder.seal());
    return input;
  };
  auto external = binding(.5F);
  auto ignored = binding(std::numeric_limits<float>::quiet_NaN());
  ignored.name = "ignored";
  WorkflowDocument direct;
  for (uint64_t id : {1, 2}) {
    WorkflowInputDeclaration declaration;
    declaration.id = id;
    declaration.name = id == 1 ? "control" : "ignored";
    declaration.result_schema = std::make_shared<SchemaTemplate>(source_schema);
    direct.inputs.push_back(declaration);
  }
  direct.nodes = {{91,
                   "test.scalar.consumer",
                   {WorkflowInputReference{1}, WorkflowInputReference{2}},
                   {}}};
  direct.outputs = {{"out", 91, "value"}};
  GraphContext graph(direct);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  auto success = context.execute(plan, {{external, ignored}});
  require(success.ok() && *starts == 1,
          "selected named scalar is ready before consumer start and excluded "
          "NaN is not read");
  auto observations = take(success.value().dependencies.source_observations());
  bool validation = false, descriptor = false;
  for (const auto& observation : observations) {
    if (observation.input == "control") {
      validation |= observation.target == ResultSupportTarget::Tensor &&
                    observation.slot == 2 && (observation.roles & 4);
      descriptor |= observation.target == ResultSupportTarget::Descriptor &&
                    (observation.roles & 8);
    }
    require(observation.input != "ignored",
            "excluded input does not acquire implicit scalar witness");
  }
  require(
      validation && descriptor,
      "implicit scalar validation and descriptor retain full Result ancestry");
  {
    auto cached_config = config;
    cached_config.result_cache_bytes = 4096;
    ExecutionContext cached(registry, cached_config);
    auto cached_root = take(cached.resource_budget());
    ExecutionBindings inputs;
    for (const char* name : {"control", "ignored"}) {
      auto builder = take(ResultBuilder::start(cached_root, source_schema,
                                               "scalar.cache.source"));
      require(builder
                  .bind_descriptor_relation(
                      take(ResultRelation::cartesian(cached_root, 1, {})))
                  .ok(),
              "cached scalar source descriptor");
      const float number = .5F;
      for (uint32_t slot = 0; slot < 3; ++slot)
        require(builder
                    .publish_tensor(
                        slot, Region::whole({1}),
                        ByteView(reinterpret_cast<const uint8_t*>(&number), 4),
                        take(ResultRelation::cartesian(cached_root, 1, {})),
                        {true, true, true, true})
                    .ok(),
                "cached scalar source");
      inputs.inputs.push_back({name, take(builder.seal())});
    }
    {
      auto seed = cached.execute(plan, inputs);
      require(seed.ok() && cached.cache_statistics().entries == 1,
              "scalar completed Result retention");
    }
    ExecutionOptions limited;
    limited.maximum_dependency_cache_work = 0;
    uint64_t lower = 0, upper = 4096;
    while (lower + 1 < upper) {
      const auto middle = lower + (upper - lower) / 2;
      limited.maximum_dependency_work = middle;
      auto attempted = cached.execute(plan, inputs, {}, limited);
      if (attempted.ok()) {
        upper = middle;
      } else {
        require(attempted.status().code == ErrorCode::ResourceExhausted,
                "scalar Run admission boundary");
        lower = middle;
      }
    }
    limited.maximum_dependency_work = upper;
    for (uint64_t fuel : {8U, 64U, 128U, 256U, 512U, 1024U, 2048U}) {
      limited.maximum_dependency_cache_work = fuel;
      auto completed = cached.execute(plan, inputs, {}, limited);
      require(completed.ok(),
              "optional cache miss restores scalar prelude at minimum Run "
              "quota");
    }
  }
  auto warm = context.execute(plan, {{external, ignored}});
  require(warm.ok(), "cached Result scalar consumer remains valid");
  const auto before = *starts;
  for (float number : {std::numeric_limits<float>::quiet_NaN(), 1.5F}) {
    auto invalid = context.execute(plan, {{binding(number), ignored}});
    require(!invalid.ok() &&
                invalid.status().code == ErrorCode::InvalidArgument &&
                invalid.status().detail.input_id == 1 &&
                invalid.status().detail.origin == FailureOrigin::Domain &&
                *starts == before,
            "bad direct named scalar reports input origin and never starts "
            "consumer");
  }
  for (const char* key : {"test.scalar.nan", "test.scalar.range"}) {
    auto computed = direct;
    computed.inputs.erase(computed.inputs.begin());
    computed.nodes = {
        {70, key, {}, {}},
        {91,
         "test.scalar.consumer",
         {WorkflowNodeOutput{70, "value"}, WorkflowInputReference{2}},
         {}}};
    GraphContext computed_graph(computed);
    auto computed_plan = take(Compiler(registry).compile(computed_graph)).plan;
    auto invalid = context.execute(computed_plan, {{ignored}});
    require(!invalid.ok() &&
                invalid.status().code == ErrorCode::OperationFailed &&
                invalid.status().detail.node_id == 70 &&
                invalid.status().detail.origin == FailureOrigin::Domain &&
                *starts == before,
            "bad computed named scalar reports upstream origin and never "
            "starts consumer");
  }
  auto shared = direct;
  shared.inputs.erase(shared.inputs.begin());
  shared.nodes = {{70, "test.scalar.range", {}, {}},
                  {80,
                   "test.scalar.loose",
                   {WorkflowNodeOutput{70, "value"}, WorkflowInputReference{2}},
                   {}},
                  {91,
                   "test.scalar.consumer",
                   {WorkflowNodeOutput{70, "value"}, WorkflowInputReference{2}},
                   {}}};
  shared.outputs.push_back({"loose", 80, "value"});
  auto shared_graph = std::make_shared<GraphContext>(shared);
  auto shared_plan = take(Compiler(registry).compile(*shared_graph)).plan;
  auto frozen = take(context.freeze(shared_plan, {{ignored}}));
  auto warm_source =
      context.execute_fragments(frozen, {{"loose", take(Footprint::all({1}))}});
  require(warm_source.ok(),
          "unbounded producer can publish a value valid for a looser consumer");
  const auto produced = *producer_starts, consumed = *starts;
  auto restricted =
      context.execute_fragments(frozen, {{"out", take(Footprint::all({1}))}});
  require(!restricted.ok() &&
              restricted.status().code == ErrorCode::OperationFailed &&
              restricted.status().detail.node_id == 70 &&
              *producer_starts == produced && *starts == consumed,
          "cached completed producer is revalidated per consumer before start");
  auto wrong_metadata = direct;
  auto wrong_schema = std::make_shared<SchemaTemplate>(source_schema);
  wrong_schema->tensors[2].descriptor.shape = {2};
  wrong_metadata.inputs[1].result_schema = wrong_schema;
  GraphContext wrong_graph(wrong_metadata);
  auto refused = Compiler(registry).compile(wrong_graph);
  require(!refused.ok() && refused.status().code == ErrorCode::TypeMismatch,
          "excluded runtime input still receives full static scalar metadata "
          "validation");
}
void foundation_tensor_workflows() {
  Driver driver;
  WorkflowDocument document;
  document.nodes = {{1, "core.constant", {}, {{"value", 2.5}}},
                    {2, "core.identity", {WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"number", 1, "value"}, {"alias", 2, "value"}};
  auto graph = std::make_shared<GraphContext>(document);
  auto compiled = take(Compiler(driver.registry).compile(*graph));
  auto output = take(driver.context->execute(compiled.plan));
  auto number = output.results.at("number");
  auto alias = output.results.at("alias");
  auto source_window = take(
      number.acquire_tensor(take(number.descriptor()), 0, Region::whole({1})));
  auto view_window = take(
      alias.acquire_tensor(take(alias.descriptor()), 0, Region::whole({1})));
  const auto first = take(source_window.row_run({0}));
  const auto last = take(view_window.row_run({0}));
  double value = 0;
  std::memcpy(&value, last.data, sizeof(value));
  require(value == 2.5 && first.data == last.data &&
              source_window.storage_owner_token() ==
                  view_window.storage_owner_token(),
          "public constant to identity workflow publishes Result tensor views");
  for (bool sparse : {false, true}) {
    SchemaTemplate schema;
    schema.id = "photospider.tensor";
    ResultTensorSpec spec;
    spec.key = "samples";
    spec.descriptor = {ElementType::UInt8, {UINT64_MAX}};
    schema.tensors.push_back(spec);
    auto buffer = take(driver.root.allocator().allocate(1));
    buffer.data()[0] = 0x72;
    auto storage = std::move(buffer).freeze();
    const Region region =
        sparse ? Region({{UINT64_MAX - 2, 2}}) : Region::whole({UINT64_MAX});
    auto builder =
        take(ResultBuilder::start(driver.root, schema, "identity.source"));
    require(builder
                .bind_descriptor_relation(take(
                    ResultRelation::cartesian(driver.root, 1, {0, 8, 0, 0})))
                .ok(),
            "huge identity basis");
    require(builder
                .publish_tensor(0, region, {0, {0}}, storage,
                                take(ResultRelation::cartesian(
                                    driver.root, UINT64_MAX, {0, 1, 0, 0})),
                                {true, true, true, true})
                .ok(),
            "huge identity affine input");
    ExecutionBinding binding;
    binding.name = "samples";
    binding.result = take(builder.seal());
    auto run = driver.prepare("core.identity", {binding});
    ResultRef identity;
    if (sparse) {
      auto result = driver.context->execute_fragments(
          take(driver.context->freeze(run.plan, run.bindings)),
          {{"out", take(Footprint::from_regions({UINT64_MAX}, {region}))}});
      require(result.ok(), result.status().message.c_str());
      identity = result.value().results.at("out");
    } else {
      auto result = driver.context->execute(run.plan, run.bindings);
      require(result.ok(), result.status().message.c_str());
      identity = result.value().results.at("out");
    }
    auto window =
        take(identity.acquire_tensor(take(identity.descriptor()), 0, region));
    require(
        *take(window.row_run({UINT64_MAX - 2})).data == 0x72 &&
            window.storage_owner_token() == storage.get() &&
            take(identity.descriptor()).tensor_coverage(0) ==
                take(Footprint::from_regions({UINT64_MAX}, {region})),
        "Result identity preserves huge broadcast owner and exact sparse ROI");
  }
  SchemaTemplate huge;
  huge.id = "photospider.tensor";
  ResultTensorSpec member;
  member.key = "samples";
  member.descriptor = {ElementType::UInt8, {UINT64_MAX, 2}};
  huge.tensors.push_back(member);
  const auto shape = member.sample_shape();
  const Region last_point({{UINT64_MAX - 1, 1}, {1, 1}});
  auto builder =
      take(ResultBuilder::start(driver.root, huge, "identity.huge.rank2"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(driver.root, 1, {0, 8, 0, 0})))
              .ok(),
          "huge rank2 basis");
  auto storage_buffer = take(driver.root.allocator().allocate(1));
  storage_buffer.data()[0] = 0x5a;
  auto storage = std::move(storage_buffer).freeze();
  auto no_input = take(ResultRelation::mapped(
      driver.root, shape, last_point, {1}, {{-1, 0, 0, 1}}, {0, 1, 0, 0}));
  require(builder
              .publish_tensor(0, last_point, {0, {0, 0}}, storage, no_input,
                              {true, true, true, true})
              .ok(),
          "huge rank2 sparse affine input");
  ExecutionBinding binding;
  binding.name = "samples";
  binding.result = take(builder.seal());
  auto prepared = driver.prepare("core.identity", {binding});
  auto requested = take(Footprint::from_regions(shape, {last_point}));
  auto huge_output = driver.context->execute_fragments(
      take(driver.context->freeze(prepared.plan, prepared.bindings)),
      {{"out", requested}});
  require(huge_output.ok(), huge_output.status().message.c_str());
  auto result = huge_output.value().results.at("out");
  uint8_t byte = 0;
  require(result.read_tensor(take(result.descriptor()), 0, {UINT64_MAX - 1, 1},
                             &byte, 1)
                  .ok() &&
              byte == 0x5a,
          "huge rank2 Result identity reads a representable source span");
  auto support = take(huge_output.value().dependencies.source_observations());
  bool exact = false;
  for (const auto& observation : support)
    exact |= observation.input == "samples" &&
             observation.target == ResultSupportTarget::Tensor &&
             observation.samples == requested && (observation.roles & 1U);
  require(exact,
          "huge rank2 compact backward support retains exact coordinates");
  auto dirty = take(
      huge_output.value().dependencies.potential_dirty("samples", requested));
  require(dirty.at("out") == requested,
          "huge rank2 compact dirty preimage remains exact");
}
void affine_source_views() {
  for (int mode = 0; mode < 6; ++mode) {
    ResourceBudget source_root;
    ResourceLimits limits;
    limits.capacity[ResourceKind::Payload] = 0;
    ResourceBudget target_root(limits);
    SchemaTemplate source_schema;
    source_schema.id = "test.affine.source";
    ResultTensorSpec slot;
    slot.key = "samples";
    slot.descriptor = {ElementType::UInt8, {2, 3}};
    StridedLayout layout{0, {3, 1}};
    std::vector<std::uint8_t> bytes{0, 1, 2, 3, 4, 5};
    std::vector<std::uint64_t> target_shape{3, 2};
    ResultTensorViewTransform transform;
    transform.source_axes = {{1, 0, 1, 1}, {0, 0, 1, 1}};
    std::vector<std::uint8_t> expected{0, 3, 1, 4, 2, 5};
    if (mode == 1) {
      slot.descriptor.shape = {6};
      layout = {0, {1}};
      target_shape = {3};
      transform.source_axes = {{0, 5, -2, 1}};
      expected = {5, 3, 1};
    } else if (mode == 2) {
      layout = {5, {-3, -1}};
      transform.reshape = true;
      transform.source_axes.clear();
      expected = {5, 4, 3, 2, 1, 0};
    } else if (mode == 3) {
      slot.descriptor.shape = {UINT64_MAX, UINT64_MAX};
      layout = {0, {0, 0}};
      bytes = {9};
      target_shape = {UINT64_MAX, UINT64_MAX};
      transform.reshape = true;
      transform.source_axes.clear();
    } else if (mode == 4) {
      slot.descriptor.shape = {1};
      layout = {0, {INT64_MIN}};
      bytes = {7};
      target_shape = {1, 1};
      transform.source_axes = {{0, 0, -1, 1}};
      expected = {7};
    } else if (mode == 5) {
      slot.descriptor.shape = {1};
      layout = {0, {0}};
      bytes = {4};
      target_shape = {2, 3};
      transform.source_axes = {{-1, 0, 0, 1}};
      expected = {4, 4, 4, 4, 4, 4};
    }
    source_schema.tensors.push_back(slot);
    auto buffer = take(source_root.allocator().allocate(bytes.size()));
    std::memcpy(buffer.data(), bytes.data(), bytes.size());
    auto storage = std::move(buffer).freeze();
    const auto* owner_token = storage.get();
    auto source_builder =
        take(ResultBuilder::start(source_root, source_schema, "affine.source"));
    require(source_builder
                .bind_descriptor_relation(take(
                    ResultRelation::cartesian(source_root, 1, {0, 8, 0, 0})))
                .ok(),
            "affine source basis");
    const auto source_count = slot.sample_count();
    auto source_relation = take(ResultRelation::cartesian(
        source_root, source_count.ok() ? source_count.value() : UINT64_MAX,
        {0, 1, 0, 0}));
    require(
        source_builder
            .publish_tensor(0, Region::whole(slot.sample_shape()), layout,
                            storage, source_relation, {true, true, true, true})
            .ok(),
        "affine view source publishes");
    auto source = take(source_builder.seal());
    auto window = take(source.acquire_tensor(
        take(source.descriptor()), 0, Region::whole(slot.sample_shape())));
    auto target_schema = source_schema;
    target_schema.id = "test.affine.view";
    target_schema.tensors[0].descriptor.shape = target_shape;
    auto target =
        take(ResultBuilder::start(target_root, target_schema, "affine.view"));
    require(target
                .bind_descriptor_relation(take(
                    ResultRelation::cartesian(target_root, 1, {0, 8, 0, 0})))
                .ok(),
            "affine target basis");
    auto relation =
        transform.reshape
            ? take(ResultRelation::reshape(
                  target_root, target_shape, Region::whole(target_shape),
                  slot.sample_shape(), Region::whole(slot.sample_shape()),
                  {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}))
            : take(ResultRelation::mapped(
                  target_root, target_shape, Region::whole(target_shape),
                  slot.sample_shape(), transform.source_axes,
                  {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
    require(
        target
            .publish_tensor_view(0, Region::whole(target_shape), window,
                                 transform, relation, {true, true, true, true})
            .ok(),
        "affine transform publishes with zero target payload capacity");
    auto result = take(target.seal());
    source_builder = {};
    source = {};
    window = {};
    storage.reset();
    auto retained = take(result.acquire_tensor(take(result.descriptor()), 0,
                                               Region::whole(target_shape)));
    require(retained.storage_owner_token() == owner_token &&
                target_root.statistics().peak[ResourceKind::Payload] == 0 &&
                target_root.statistics().issued.io_bytes == 0 &&
                source_root.statistics().live[ResourceKind::Payload] ==
                    bytes.size(),
            "view retains source root without payload copy or double charge");
    if (mode == 3) {
      auto run = take(retained.row_run({UINT64_MAX - 1, UINT64_MAX - 1}));
      require(
          run.samples == 1 && run.sample_stride_bytes == 0 && *run.data == 9,
          "huge zero-stride reshape avoids overflowing dense element count");
    } else {
      for (std::size_t i = 0; i < expected.size(); ++i) {
        std::vector<std::uint64_t> at(target_shape.size());
        auto position = i;
        for (std::size_t axis = target_shape.size(); axis;) {
          --axis;
          at[axis] = position % target_shape[axis];
          position /= target_shape[axis];
        }
        require(*take(retained.row_run(at)).data == expected[i],
                "affine point/reshape reads exact logical values");
      }
    }
    retained = {};
    result = {};
    target = {};
    require(source_root.statistics().live[ResourceKind::Payload] == 0,
            "last view retirement releases the independently charged source");
  }
}
void affine_view_failures() {
  ResourceBudget root;
  SchemaTemplate schema;
  schema.id = "test.affine.prefix";
  schema.publication = PublishPolicy::StablePrefix;
  ResultTensorSpec spec;
  spec.key = "samples";
  spec.descriptor = {ElementType::UInt8, {2}};
  schema.tensors.push_back(spec);
  const Region left({{0, 1}}), right({{1, 1}});
  auto relation = take(ResultRelation::cartesian(root, 2, {0, 1, 0, 0}));
  auto make = [&] {
    auto builder = take(ResultBuilder::start(root, schema, "affine.prefix"));
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
                .ok(),
            "affine prefix basis");
    return builder;
  };
  auto storage =
      take(Value::create({ElementType::UInt8, {1}}, left, {0, {0}}, {3}))
          .storage();
  auto first = make();
  require(first
              .publish_tensor(0, left, {0, {0}}, storage, relation,
                              {true, true, true, true})
              .ok(),
          "first affine prefix");
  auto first_ref = first.reference();
  auto facts = take(first_ref.descriptor(false));
  auto input = take(first_ref.acquire_tensor(facts, 0, left));
  ResultTensorViewTransform identity;
  identity.source_axes = {{0, 0, 1, 1}};
  auto second = make();
  require(second
              .publish_tensor_view(0, left, input, identity, relation,
                                   {true, true, true, true})
              .ok(),
          "first affine ownership edge");
  auto second_ref = second.reference();
  auto second_window = take(
      second_ref.acquire_tensor(take(second_ref.descriptor(false)), 0, left));
  ResultTensorViewTransform translated;
  translated.source_axes = {{0, 0, 1, 1, 1}};
  require(first.publish_tensor_view(0, right, second_window, translated,
                                    relation, {true, true, true, true})
                  .code == ErrorCode::TypeMismatch,
          "indirect affine ownership cycle rejected before publication");
  require(*take(first_ref.acquire_tensor(facts, 0, left))
                  .row_run({0})
                  .value()
                  .data == 3,
          "failed affine cycle leaves previous captured prefix readable");
  auto unauthorized = make();
  ResultTensorViewTransform outside;
  outside.source_axes = {{0, 1, 1, 1}};
  const auto failed = unauthorized.publish_tensor_view(
      0, left, input, outside, relation, {true, true, true, true});
  require(failed.reason == FailureReason::UnauthorizedRead &&
              failed.detail.origin == FailureOrigin::Protocol &&
              failed.detail.scope == FailureScope::Group &&
              unauthorized
                      .publish_tensor_view(0, left, input, identity, relation,
                                           {true, true, true, true})
                      .reason == failed.reason,
          "unauthorized transform is sticky with complete first cause");
  auto cancelled = make();
  CancellationSource stop;
  stop.cancel();
  require(cancelled.publish_tensor_view(0, left, input, identity, relation,
                                        {true, true, true, true}, stop.token())
                      .code == ErrorCode::Cancelled &&
              cancelled.seal().status().code == ErrorCode::Cancelled,
          "pre-cancelled affine view publication is sticky");
  auto fragmented = make();
  auto other =
      take(Value::create({ElementType::UInt8, {1}}, left, {0, {0}}, {4}))
          .storage();
  require(fragmented
                  .publish_tensor(0, left, {0, {0}}, storage, relation,
                                  {true, true, true, true})
                  .ok() &&
              fragmented
                  .publish_tensor(0, right, {0, {0}, {1}}, other, relation,
                                  {true, true, true, true})
                  .ok(),
          "unrelated source blocks");
  auto fragmented_ref = take(fragmented.seal());
  auto fragmented_window = take(fragmented_ref.acquire_tensor(
      take(fragmented_ref.descriptor()), 0, Region::whole({2})));
  auto fragmented_unauthorized = make();
  outside.source_axes = {{-1, 2, 0, 1}};
  require(
      fragmented_unauthorized
                  .publish_tensor_view(0, Region::whole({2}), fragmented_window,
                                       outside, relation,
                                       {true, true, true, true})
                  .reason == FailureReason::UnauthorizedRead &&
          fragmented_unauthorized.seal().status().reason ==
              FailureReason::UnauthorizedRead,
      "unavailable physical source cannot hide sticky authorization failure");
  auto unfinished = make();
  require(
      unfinished.publish_tensor_view(0, Region::whole({2}), fragmented_window,
                                     identity, relation,
                                     {false, true, true, true})
                  .code == ErrorCode::TypeMismatch &&
          unfinished.seal().status().code == ErrorCode::TypeMismatch,
      "unavailable physical source cannot hide publication finality failure");
  auto foreign = make();
  ResourceBudget foreign_root;
  auto foreign_relation =
      take(ResultRelation::cartesian(foreign_root, 2, {0, 1, 0, 0}));
  require(
      foreign.publish_tensor_view(0, Region::whole({2}), fragmented_window,
                                  identity, foreign_relation,
                                  {true, true, true, true})
                  .code == ErrorCode::TypeMismatch &&
          foreign.seal().status().code == ErrorCode::TypeMismatch,
      "unavailable physical source cannot hide foreign relation provenance");
  auto uniform = make();
  require(uniform
              .publish_tensor(0, Region::whole({2}), {0, {0}}, storage,
                              relation, {true, true, true, true})
              .ok(),
          "uniform reshape source");
  auto uniform_ref = take(uniform.seal());
  auto uniform_window = take(uniform_ref.acquire_tensor(
      take(uniform_ref.descriptor()), 0, Region::whole({2})));
  for (const auto* source : {&uniform_window, &fragmented_window}) {
    auto three_schema = schema;
    three_schema.tensors[0].descriptor.shape = {3};
    auto three =
        take(ResultBuilder::start(root, three_schema, "reshape.mismatch"));
    require(three
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
                .ok(),
            "reshape mismatch basis");
    ResultTensorViewTransform reshape;
    reshape.reshape = true;
    auto mismatch = three.publish_tensor_view(
        0, Region::whole({3}), *source, reshape,
        take(ResultRelation::cartesian(root, 3, {0, 1, 0, 0})),
        {true, true, true, true});
    require(mismatch.code == ErrorCode::InvalidArgument &&
                mismatch.detail.origin == FailureOrigin::Protocol &&
                mismatch.detail.scope == FailureScope::Group &&
                three.seal().status().code == ErrorCode::InvalidArgument,
            "reshape cardinality failure is sticky for affine and fragmented "
            "sources");
  }
  auto explicit_copy = make();
  auto unavailable = explicit_copy.publish_tensor_view(
      0, Region::whole({2}), fragmented_window, identity, relation,
      {true, true, true, true});
  const std::uint8_t bytes[] = {3, 4};
  require(unavailable.message.find("ViewUnavailable") != std::string::npos &&
              explicit_copy
                  .publish_tensor(0, Region::whole({2}), ByteView(bytes, 2),
                                  relation, {true, true, true, true})
                  .ok(),
          "unavailable physical view permits explicit copy retry");
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
void mix_cancellation_residual() {
  Driver driver;
  auto make = [&](float sample, bool mask) {
    auto shape = schema(mask);
    shape.tensors[0].batch_axes[0] = shape.tensors[0].batch_axes[1] = 1;
    shape.tensors[0].descriptor.shape =
        mask ? std::vector<std::uint64_t>{1, 1}
             : std::vector<std::uint64_t>{1, 1, 4};
    auto builder = take(ResultBuilder::start(driver.root, shape, "mix.source"));
    require(builder
                .bind_descriptor_relation(take(
                    ResultRelation::cartesian(driver.root, 1, {0, 1, 0, 0})))
                .ok(),
            "mix source basis");
    const std::vector<float> samples =
        mask ? std::vector<float>{sample} : std::vector<float>{sample, 0, 0, 1};
    require(
        builder
            .publish_tensor(
                0, Region::whole(shape.tensors[0].sample_shape()),
                ByteView(reinterpret_cast<const std::uint8_t*>(samples.data()),
                         samples.size() * sizeof(float)),
                take(ResultRelation::cartesian(driver.root, samples.size(),
                                               {0, 1, 0, 0})),
                {true, true, true, true})
            .ok(),
        "mix source samples");
    ExecutionBinding binding;
    binding.name = mask ? "mask" : sample > 0 ? "a" : "b";
    binding.result = take(builder.seal());
    return binding;
  };
  const auto output =
      driver.run("image.mix", {make(std::ldexp(1.F, -30), false),
                               make(-std::ldexp(1.F, 70), false),
                               make(std::ldexp(1.F, -100), true)});
  const float sample = read(output, {0, 0, 0, 0, 0});
  std::uint32_t bits = 0;
  std::memcpy(&bits, &sample, sizeof(bits));
  require(bits == 0x80080000U,
          "mix preserves compensated cancellation residual");
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
void view_graph_and_growth() {
  ResourceBudget root;
  auto spec = schema(true);
  spec.publication = PublishPolicy::StablePrefix;
  spec.tensors[0].batch_axes[0] = spec.tensors[0].batch_axes[1] = 1;
  spec.tensors[0].descriptor.shape = {1, 2};
  auto first = take(ResultBuilder::start(root, spec, "view.graph.first"));
  auto second = take(ResultBuilder::start(root, spec, "view.graph.second"));
  auto basis = take(ResultRelation::cartesian(root, 1, {0, 1, 0, 0}));
  require(first.bind_descriptor_relation(basis).ok() &&
              second.bind_descriptor_relation(basis).ok(),
          "graph bases");
  auto relation = take(ResultRelation::cartesian(root, 2, {0, 1, 0, 0}));
  const Region left({{0, 1}, {0, 1}, {0, 1}, {0, 1}});
  const Region right({{0, 1}, {0, 1}, {0, 1}, {1, 1}});
  const float sample = .5F;
  const ByteView bytes(reinterpret_cast<const std::uint8_t*>(&sample), 4);
  require(
      first.publish_tensor(0, right, bytes, relation, {true, true, true, true})
              .ok() &&
          second
              .publish_tensor(0, left, bytes, relation,
                              {true, true, true, true})
              .ok(),
      "graph prefixes");
  auto first_ref = first.reference(), second_ref = second.reference();
  auto first_window = take(
      first_ref.acquire_tensor(take(first_ref.descriptor(false)), 0, right));
  auto second_window = take(
      second_ref.acquire_tensor(take(second_ref.descriptor(false)), 0, left));
  require(first
              .publish_tensor_view(0, left, {&second_window}, relation,
                                   {true, true, true, true})
              .ok(),
          "acyclic view edge");
  require(!second.publish_tensor_view(0, right, {&first_window}, relation,
                                      {true, true, true, true})
                  .ok() &&
              !second.seal().ok(),
          "indirect Result view cycle rejected");
  require(
      !second_ref.acquire_tensor(take(second_ref.descriptor(false)), 0, right)
           .ok(),
      "cycle publishes no coverage");

  // A retained source window does not require the source's unpublished writer
  // lock during another builder's alias proof; callbacks can inspect outputs.
  auto source =
      take(ResultBuilder::start(root, spec, "view.concurrent.source"));
  auto target =
      take(ResultBuilder::start(root, spec, "view.concurrent.target"));
  require(source.bind_descriptor_relation(basis).ok() &&
              target.bind_descriptor_relation(basis).ok(),
          "concurrent view bases");
  require(
      source.publish_tensor(0, left, bytes, relation, {true, true, true, true})
          .ok(),
      "concurrent source prefix");
  auto source_ref = source.reference(), target_ref = target.reference();
  auto retained = take(
      source_ref.acquire_tensor(take(source_ref.descriptor(false)), 0, left));
  std::mutex mutex;
  std::condition_variable ready;
  bool entered = false, release = false;
  auto writer = std::async(std::launch::async, [&] {
    return source.publish_tensor_kernel(
        0, right,
        [&](const auto&) {
          std::unique_lock<std::mutex> lock(mutex);
          entered = true;
          ready.notify_all();
          ready.wait(lock, [&] { return release; });
          lock.unlock();
          return target_ref.descriptor(false).ok()
                     ? Status::success()
                     : Status{ErrorCode::OperationFailed, {}};
        },
        relation, {true, true, true, true});
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    ready.wait(lock, [&] { return entered; });
  }
  auto alias = std::async(std::launch::async, [&] {
    return target.publish_tensor_view(0, left, {&retained}, relation,
                                      {true, true, true, true});
  });
  const auto completes =
      alias.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
    ready.notify_all();
  }
  const auto alias_status = alias.get(), writer_status = writer.get();
  require(completes && alias_status.ok() && writer_status.ok(),
          "view publication avoids source callback lock cycle");

  spec.tensors[0].descriptor.shape = {1, 64};
  auto source64 = take(ResultBuilder::start(root, spec, "growth.source"));
  require(source64.bind_descriptor_relation(basis).ok(), "growth source basis");
  relation = take(ResultRelation::cartesian(root, 64, {0, 1, 0, 0}));
  std::vector<float> pixels(64, .25F);
  require(source64
              .publish_tensor(
                  0, Region::whole(spec.tensors[0].sample_shape()),
                  ByteView(reinterpret_cast<const std::uint8_t*>(pixels.data()),
                           256),
                  relation, {true, true, true, true})
              .ok(),
          "growth source");
  source_ref = take(source64.seal());
  ResultGrowthLimits growth;
  growth.maximum_bytes = 4096;
  auto bounded =
      take(ResultBuilder::start(root, spec, "growth.target", growth));
  require(bounded.bind_descriptor_relation(basis).ok(), "bounded basis");
  std::uint64_t successful = 0;
  Status outcome;
  for (std::uint64_t x = 0; x < 64; ++x) {
    const Region cell({{0, 1}, {0, 1}, {0, 1}, {x, 1}});
    auto input =
        take(source_ref.acquire_tensor(take(source_ref.descriptor()), 0, cell));
    outcome = bounded.publish_tensor_view(0, cell, {&input}, relation,
                                          {true, true, true, true});
    if (!outcome.ok()) {
      break;
    }
    ++successful;
  }
  require(successful > 0 && successful < 64 &&
              outcome.code == ErrorCode::ResourceExhausted,
          "all view metadata shares one producer growth budget");
  target_ref = bounded.reference();
  require(target_ref.acquire_tensor(take(target_ref.descriptor(false)), 0, left)
                  .ok() &&
              !target_ref
                   .acquire_tensor(
                       take(target_ref.descriptor(false)), 0,
                       Region({{0, 1}, {0, 1}, {0, 1}, {successful, 1}}))
                   .ok() &&
              !bounded.seal().ok(),
          "growth failure preserves earlier view and rejects new publication");
}
void large_view() {
  ResourceBudget root;
  auto image_schema = schema(true);
  image_schema.tensors[0].batch_axes[0] =
      image_schema.tensors[0].batch_axes[1] = 1;
  image_schema.tensors[0].descriptor.shape = {1025, 1025};
  auto builder = take(ResultBuilder::start(root, image_schema, "large.source"));
  auto basis = take(ResultRelation::cartesian(root, 1, {0, 1, 0, 0}));
  require(builder.bind_descriptor_relation(basis).ok(), "large source basis");
  const auto shape = image_schema.tensors[0].sample_shape();
  const auto count = take(image_schema.tensors[0].sample_count());
  auto independent = take(ResultRelation::cartesian(root, count, {0, 1, 0, 0}));
  require(builder
              .publish_tensor_kernel(
                  0, Region::whole(shape),
                  [](const auto& writers) {
                    for (std::uint64_t y = 0; y < 1025; ++y) {
                      for (std::uint64_t x = 0; x < 1025;) {
                        auto run = writers[0].row_run({0, 0, y, x});
                        if (!run.ok()) {
                          return run.status();
                        }
                        const float value = .25F;
                        for (std::uint64_t i = 0; i < run.value().samples;
                             ++i) {
                          std::memcpy(run.value().data + i * 4, &value, 4);
                        }
                        x += run.value().samples;
                      }
                    }
                    return Status::success();
                  },
                  independent, {true, true, true, true})
              .ok(),
          "large direct-write publication");
  auto original = take(builder.seal());
  auto window = take(original.acquire_tensor(take(original.descriptor()), 0,
                                             Region::whole(shape)));
  std::vector<ResultMappedAxis> axes;
  for (std::int32_t i = 0; i < 4; ++i) {
    axes.push_back({i, 0, 1, 1});
  }
  auto identity = take(
      ResultRelation::mapped(root, shape, Region::whole(shape), shape, axes,
                             {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto output = take(ResultBuilder::start(root, image_schema, "large.view"));
  require(output.bind_descriptor_relation(basis).ok(), "large view basis");
  const auto before = root.statistics().issued.work;
  require(output
              .publish_tensor_view(0, Region::whole(shape), {&window}, identity,
                                   {true, true, true, true})
              .ok(),
          "view publication above one million samples");
  require(root.statistics().issued.work - before < 10000,
          "structured view certification scales with rectangles");
  auto result = take(output.seal());
  auto alias = take(result.acquire_tensor(take(result.descriptor()), 0,
                                          Region::whole(shape)));
  require(take(alias.row_run({0, 0, 1024, 1024})).data ==
              take(window.row_run({0, 0, 1024, 1024})).data,
          "large view preserves last payload pointer");
}
void prefix_relations() {
  ResourceBudget root;
  const ResultSupport address{3, 5, 0, 0, ResultSupportTarget::Tensor, 2};
  auto relation = take(
      ResultRelation::prefix(root, 6, 3, 5, ResultSupportTarget::Tensor, 2));
  auto set = [](unsigned mask) {
    std::vector<Region> points;
    for (uint64_t i = 0; i < 6; ++i)
      if (mask & (1U << i))
        points.emplace_back(std::vector<RegionDimension>{{i, 1}});
    return take(Footprint::from_regions({6}, points));
  };
  for (uint64_t i = 0; i < 6; ++i) {
    unsigned calls = 0;
    require(
        relation.visit(
                    i, 1,
                    [&](auto span) {
                      ++calls;
                      require(
                          span.input == 3 && span.roles == 5 &&
                              span.slot == 2 &&
                              span.target == ResultSupportTarget::Tensor &&
                              !span.first && span.count == i + 1,
                          "prefix scalar support preserves address and roles");
                      return Status::success();
                    })
                .ok() &&
            calls == 1,
        "one prefix node visits one inclusive span");
  }
  for (unsigned wanted = 0; wanted < 64; ++wanted) {
    auto outputs = set(wanted);
    require(relation.certify(outputs).ok(), "prefix sparse certificate");
    uint64_t actual = 0, expected = 0;
    for (uint64_t i = 0; i < 6; ++i)
      if (wanted & (1U << i))
        expected = i + 1;
    require(relation.project(outputs,
                             [&](auto span, const Footprint* points) {
                               require(!points && !span.first,
                                       "prefix flat projection");
                               actual = span.count;
                               return Status::success();
                             })
                    .ok() &&
                actual == expected,
            "prefix projection matches enumerated union including Empty");
    for (unsigned edited = 0; edited < 64; ++edited) {
      unsigned expected_dirty = 0;
      for (unsigned i = 0; i < 6; ++i)
        for (unsigned j = 0; j <= i; ++j)
          if ((wanted & (1U << i)) && (edited & (1U << j)))
            expected_dirty |= 1U << i;
      auto dirty = take(relation.preimage(outputs, address, set(edited)));
      require(dirty == set(expected_dirty),
              "prefix inverse matches independent per-point dependency");
    }
  }
  auto all = take(Footprint::all({6}));
  auto unrelated = address;
  unrelated.roles = 2;
  require(take(relation.preimage(all, unrelated, all)).empty(),
          "prefix dirty address requires role overlap");
  unrelated = address;
  unrelated.slot = 1;
  require(take(relation.preimage(all, unrelated, all)).empty(),
          "prefix dirty address requires the same member");
  require(
      relation.certify(take(Footprint::all({2, 3}))).code ==
              ErrorCode::InvalidArgument &&
          relation.project(take(Footprint::all({2, 3})),
                           [](auto, const auto*) { return Status::success(); })
                  .code == ErrorCode::InvalidArgument,
      "prefix flattened count does not replace rank-one domain");
  auto wrong = ResultRelation::unite(
      root, {relation, take(ResultRelation::mapped(
                           root, {2, 3}, Region::whole({2, 3}), {2, 3},
                           {{0, 0, 1, 1}, {1, 0, 1, 1}}, address))});
  require(!wrong.ok() && wrong.status().code == ErrorCode::InvalidArgument,
          "prefix union rejects a different coordinate domain");
  {
    auto combined = take(ResultRelation::unite(
        root, {relation, take(ResultRelation::prefix(
                             root, 6, 4, 2, ResultSupportTarget::Tensor, 1))}));
    require(take(combined.preimage(all, address, set(8))) == set(56),
            "prefix union preserves each source address and exact suffix");
  }
  require(!ResultRelation::prefix(root, 0).ok() &&
              !ResultRelation::prefix(root, 6, 0, 8).ok() &&
              !ResultRelation::prefix(root, 6, 0, 1,
                                      ResultSupportTarget::Descriptor)
                   .ok(),
          "prefix rejects empty domain and reserved metadata observations");
  relation = {};
  const auto baseline = root.statistics().live;
  relation = take(ResultRelation::prefix(root, UINT64_MAX, 3, 5,
                                         ResultSupportTarget::Tensor, 2));
  auto requested = take(
      Footprint::from_regions({UINT64_MAX}, {Region({{UINT64_MAX - 4, 3}})}));
  auto changed = take(
      Footprint::from_regions({UINT64_MAX}, {Region({{UINT64_MAX - 3, 1}})}));
  const auto before_work = root.statistics().issued.work;
  uint64_t projected = 0;
  require(relation.project(take(Footprint::all({UINT64_MAX})),
                           [&](auto span, const auto*) {
                             projected = span.count;
                             return Status::success();
                           })
                  .ok() &&
              projected == UINT64_MAX,
          "complete uint64 prefix projection retains the full interval");
  require(relation.certify(requested).ok() &&
              take(relation.preimage(requested, address, changed)) ==
                  take(Footprint::from_regions(
                      {UINT64_MAX}, {Region({{UINT64_MAX - 3, 2}})})),
          "uint64 prefix inverse retains coordinates and requested holes");
  uint64_t count = 0;
  require(relation.visit(UINT64_MAX - 1, 1,
                         [&](auto span) {
                           count = span.count;
                           return Status::success();
                         })
                  .ok() &&
              count == UINT64_MAX,
          "last uint64 prefix span does not overflow");
  require(root.statistics().issued.work - before_work < 1000,
          "huge prefix work depends on rectangles, not samples");
  const auto huge_charge = root.statistics().live;
  relation = {};
  require(root.statistics().live.values == baseline.values,
          "prefix owner retires all admitted metadata");
  relation = take(
      ResultRelation::prefix(root, 6, 3, 5, ResultSupportTarget::Tensor, 2));
  require(root.statistics().live.values == huge_charge.values,
          "prefix metadata size is independent of domain cardinality");
  FootprintLimits limited;
  limited.maximum_work = 0;
  require(relation.project(
                      all, [](auto, const auto*) { return Status::success(); },
                      limited)
                  .code == ErrorCode::ResourceExhausted,
          "prefix projection obeys finite work limit");
  CancellationSource cancelled;
  cancelled.cancel();
  limited = {};
  limited.cancellation = cancelled.token();
  auto rejected = relation.preimage(all, address, all, limited);
  require(!rejected.ok() && rejected.status().code == ErrorCode::Cancelled,
          "prefix inverse observes cancellation");
  ResourceLimits capacity;
  capacity.capacity[ResourceKind::Entries] = 1;
  ResourceBudget exhausted(capacity);
  auto failed = ResultRelation::prefix(exhausted, 6);
  require(!failed.ok() && failed.status().code == ErrorCode::ResourceExhausted,
          "prefix partial allocation returns typed capacity failure");
  for (auto live : exhausted.statistics().live.values)
    require(!live, "failed prefix allocation retires all leases");
}
void mapped_relation_capacity_failure() {
  ResourceLimits limits;
  limits.capacity[ResourceKind::Entries] = 3;
  ResourceBudget exhausted(limits);
  auto failed = ResultRelation::mapped(
      exhausted, {2}, Region::whole({2}), {2}, {{0, 0, 1, 1}},
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0});
  require(!failed.ok() && failed.status().code == ErrorCode::ResourceExhausted,
          "mapped metadata admission returns typed failure");
  for (auto count : exhausted.statistics().live.values) {
    require(!count, "failed mapped construction retires every partial lease");
  }
  limits.capacity[ResourceKind::Entries] = 128;
  ResourceBudget root(limits);
  auto relation = take(
      ResultRelation::mapped(root, {2}, Region::whole({2}), {2}, {{0, 0, 1, 1}},
                             {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto selected = take(Footprint::all({2}));
  const auto before = root.statistics().live;
  ResourceCapacity occupied;
  occupied[ResourceKind::Entries] =
      limits.capacity[ResourceKind::Entries] - before[ResourceKind::Entries];
  auto blocker = take(root.reserve(occupied));
  auto projected = relation.project(
      selected, [](auto, const Footprint*) { return Status::success(); });
  require(projected.code == ErrorCode::ResourceExhausted,
          "mapped projection metadata failure does not throw");
  auto inverse = relation.preimage(
      selected, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}, selected);
  require(
      !inverse.ok() && inverse.status().code == ErrorCode::ResourceExhausted,
      "mapped inverse metadata failure does not throw");
  blocker = {};
  require(root.statistics().live.values == before.values,
          "failed compact relation queries retain no extra metadata");
}
void anchored_tensor_relations() {
  ResourceBudget root;
  for (std::int64_t step : {-2, -1, 0, 1, 2}) {
    for (std::uint64_t anchor = 0; anchor < 9; ++anchor) {
      for (std::uint64_t width = 1; width < 4; ++width) {
        ResultMappedAxis axis{0, anchor, step, width, 1};
        const auto first = axis.source_coordinate(1),
                   last = axis.source_coordinate(4);
        if (!first.ok() || !last.ok() || first.value() >= 9 ||
            last.value() >= 9) {
          continue;
        }
        auto relation = take(ResultRelation::mapped(
            root, {8}, Region({{1, 4}}), {9}, {axis},
            {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
        const auto selected =
            take(Footprint::from_regions({8}, {Region({{1, 4}})}));
        for (std::uint64_t dirty = 0; dirty < 9; ++dirty) {
          std::vector<Region> expected;
          for (std::uint64_t output = 1; output < 5; ++output) {
            const auto start = static_cast<std::int64_t>(anchor) +
                               step * (static_cast<std::int64_t>(output) - 1);
            if (static_cast<std::int64_t>(dirty) >= start &&
                static_cast<std::int64_t>(dirty) <
                    start + static_cast<std::int64_t>(width)) {
              expected.emplace_back(std::vector<RegionDimension>{{output, 1}});
            }
          }
          auto changed =
              take(Footprint::from_regions({9}, {Region({{dirty, 1}})}));
          auto got = take(relation.preimage(
              selected, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}, changed));
          require(got == take(Footprint::from_regions({8}, expected)),
                  "anchored signed inverse matches independent point support");
        }
        Footprint got;
        auto projected =
            relation.project(selected, [&](auto, const Footprint* input) {
              got = *input;
              return Status::success();
            });
        const auto magnitude = step < 0 ? -step : step;
        if (static_cast<std::uint64_t>(magnitude) > width) {
          require(projected.code == ErrorCode::NotFound,
                  "strided holes never become an exact bounding box");
        } else {
          std::vector<Region> expected;
          for (std::uint64_t output = 1; output < 5; ++output) {
            const auto start = static_cast<std::int64_t>(anchor) +
                               step * (static_cast<std::int64_t>(output) - 1);
            expected.emplace_back(std::vector<RegionDimension>{
                {static_cast<std::uint64_t>(start),
                 std::min<std::uint64_t>(width, 9 - start)}});
          }
          require(projected.ok() &&
                      got == take(Footprint::from_regions({9}, expected)),
                  "signed projection preserves exact clipped interval support");
        }
      }
    }
  }
  const std::uint64_t origin = std::uint64_t{1} << 63;
  ResultMappedAxis anchored{0, 0, 1, 1, origin};
  auto relation = take(ResultRelation::mapped(
      root, {UINT64_MAX}, Region({{origin, 2}}), {2}, {anchored},
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto selected =
      take(Footprint::from_regions({UINT64_MAX}, {Region({{origin, 2}})}));
  auto dirty = take(relation.preimage(
      selected, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0},
      take(Footprint::from_regions({2}, {Region({{1, 1}})}))));
  require(dirty == take(Footprint::from_regions({UINT64_MAX},
                                                {Region({{origin + 1, 1}})})),
          "u64 anchored translation does not narrow logical coordinates");
  ResultMappedAxis extreme{0, UINT64_MAX, INT64_MIN, 1, UINT64_MAX};
  require(extreme.source_coordinate(UINT64_MAX).value() == UINT64_MAX &&
              !extreme.source_coordinate(0).ok(),
          "INT64_MIN signed step checks product before overflow");
  const std::vector<std::uint64_t> huge{UINT64_MAX, 2};
  relation = take(ResultRelation::mapped(
      root, huge, Region::whole(huge), huge, {{0, 0, 1, 1}, {1, 0, 1, 1}},
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  require(relation.certify(take(Footprint::all(huge))).ok(),
          "compact tensor witness certifies unflattenable domain without "
          "enumeration");
  relation = take(
      ResultRelation::mapped(root, {1}, Region::whole({1}), huge,
                             {{-1, UINT64_MAX - 1, 1, 1}, {-1, 0, 1, 1}},
                             {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  Footprint input;
  require(
      relation.project(take(Footprint::all({1})),
                       [&](auto, const Footprint* samples) {
                         input = *samples;
                         return Status::success();
                       })
              .ok() &&
          input.contains({UINT64_MAX - 1, 0}),
      "unflattenable exact support still has an exact coordinate projection");
  auto visited = relation.visit(0, 64, [](auto) { return Status::success(); });
  require(visited.code == ErrorCode::ResourceExhausted,
          "flat support access returns typed overflow without wrapping exact "
          "evidence");
  auto table = take(ResultRelation::sample_rows(root, UINT64_MAX, 1, [](auto) {
    return Result<ResultRelationRow>(
        ResultRelationRow{UINT64_MAX - 3,
                          {0, 1, 0, 1, ResultSupportTarget::Tensor, 0}});
  }));
  auto samples = take(
      Footprint::from_regions(huge, {Region({{UINT64_MAX - 1, 1}, {0, 1}})}));
  require(table.certify(samples).code == ErrorCode::ResourceExhausted,
          "sample table certificate cannot authorize a wrapped coordinate");
}
void mapped_relations() {
  ResourceBudget root;
  const std::vector<std::uint64_t> output{2, 3, 257, 513};
  const std::vector<std::uint64_t> input{2, 3, 513, 1025, 4};
  const std::vector<ResultMappedAxis> axes{{0, 0, 1, 1},
                                           {1, 0, 1, 1},
                                           {2, 0, 2, 2},
                                           {3, 0, 2, 2},
                                           {-1, 0, 1, 4}};
  auto relation = take(
      ResultRelation::mapped(root, output, Region::whole(output), input, axes,
                             {0, 5, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto all = take(Footprint::all(output));
  Footprint projected;
  require(relation.project(all,
                           [&](auto support, const auto* samples) {
                             require(support.target ==
                                             ResultSupportTarget::Tensor &&
                                         support.roles == 5 && samples,
                                     "compact relation address and roles");
                             projected = *samples;
                             return Status::success();
                           })
                  .ok() &&
              projected == take(Footprint::all(input)),
          "large rectangle projection has no per-sample table");
  const Region last({{1, 1}, {2, 1}, {512, 1}, {1024, 1}, {3, 1}});
  auto changed = take(Footprint::from_regions(input, {last}));
  auto dirty = take(relation.preimage(
      all, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}, changed));
  require(dirty == take(Footprint::from_regions(
                       output, {Region({{1, 1}, {2, 1}, {256, 1}, {512, 1}})})),
          "large inverse clipped downsample cell");
  std::uint64_t count = 0;
  const auto last_index = take(all.element_count()) - 1;
  require(relation.visit(last_index, 16,
                         [&](auto span) {
                           count += span.count;
                           return Status::success();
                         })
                  .ok() &&
              count == 4,
          "mapped visit exact clipped support");
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
void image_schema_contracts() {
  Driver d;
  const auto control = scalar(d.root, .5F);
  OperationMetadata scalar_metadata;
  scalar_metadata.result_schema =
      std::make_shared<SchemaTemplate>(control.schema());
  for (const auto& batches :
       std::vector<std::vector<uint64_t>>{{}, {1}, {1, 1, 1}, {1, 1}}) {
    auto input_schema = schema();
    input_schema.tensors[0].batch_axes.assign(batches.begin(), batches.end());
    if (batches.size() == 2) {
      input_schema.tensors[0].layout.spatial = false;
      input_schema.tensors[0].facets.clear();
    }
    require(
        input_schema.validate(true).ok(),
        "generic Result schema permits alternate batch and spatial layouts");
    OperationMetadata input;
    input.result_schema = std::make_shared<SchemaTemplate>(input_schema);
    for (const auto* operation :
         {"image.opacity", "image.source_over", "image.split_horizontal"}) {
      std::vector<OperationMetadata> inputs{input};
      std::map<std::string, ParameterValue> parameters;
      if (std::string(operation) == "image.opacity")
        inputs.push_back(scalar_metadata);
      else if (std::string(operation) == "image.source_over")
        inputs.push_back(input);
      else
        parameters["split_x"] = int64_t{2};
      auto result = d.registry->resolve_traits(operation, inputs, parameters);
      require(!result.ok() && result.status().code == ErrorCode::TypeMismatch,
              "image metadata rejects unsupported batch or spatial layout "
              "before coordinate indexing");
    }
  }
}
void image_gaussian_contract() {
  Driver d;
  const auto shape = schema();
  auto builder = take(ResultBuilder::start(d.root, shape, "gaussian.source"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(d.root, 1, {0, 8, 0, 0})))
              .ok(),
          "Gaussian source descriptor");
  std::vector<float> pixels(240);
  for (unsigned i = 0; i < pixels.size(); ++i) {
    const auto slot = i % 60, batch = i / 60;
    pixels[i] =
        slot % 4 == 3
            ? .5F
            : static_cast<float>(
                  static_cast<int>((slot * 37 + batch * 11) % 101) - 50) /
                  7.F;
  }
  require(builder
              .publish_tensor(
                  0, Region::whole(shape.tensors[0].sample_shape()),
                  ByteView(reinterpret_cast<const uint8_t*>(pixels.data()),
                           pixels.size() * 4),
                  take(ResultRelation::cartesian(d.root, pixels.size(),
                                                 {0, 1, 0, 0})),
                  {true, true, true, true})
              .ok(),
          "Gaussian source samples");
  ExecutionBinding binding;
  binding.name = "image";
  binding.result = take(builder.seal());
  // Captured from the c552eb56 C Gaussian callback, radius 2 and sigma .9.
  const uint32_t expected[] = {
      0xc0748e68U, 0xbe1ac4d9U, 0x3de5e9f3U, 0x3f000000U, 0xbee97e75U,
      0x3f01f488U, 0xbea767b7U, 0x3f000000U, 0x3fc9673cU, 0x3e180e3eU,
      0x3d233547U, 0x3f000000U, 0x3fbcce6eU, 0xbe9b9600U, 0xbea8169dU,
      0x3f000000U, 0x400187f5U, 0xbfb8c0b3U, 0x3f6dae14U, 0x3f000000U,
      0xbfd52c35U, 0xbf76c091U, 0xbfb69522U, 0x3f000000U, 0xbee05335U,
      0xbea4c006U, 0x3e93ead2U, 0x3f000000U, 0x3d334a19U, 0x3e2a64bfU,
      0x3fad90f2U, 0x3f000000U, 0xbe93c8f1U, 0xbe306b87U, 0x3f8dd821U,
      0x3f000000U, 0xbee9d7faU, 0x3e7aaf6eU, 0x3fc33b7eU, 0x3f000000U,
      0xbddc56b7U, 0xc04cfb58U, 0xbf70e0aeU, 0x3f000000U, 0xbf918f3bU,
      0xbf0ba965U, 0x3f820f01U, 0x3f000000U, 0xbfbd36d8U, 0x3fbeeab7U,
      0x3fadb92aU, 0x3f000000U, 0xbff96effU, 0x3fb15959U, 0x3f81e347U,
      0x3f000000U, 0xbfb32d83U, 0x40291b06U, 0xbdff7ec7U, 0x3f000000U,
      0xc0291b06U, 0x3fb32d83U, 0x3fc6d359U, 0x3f000000U, 0xbfb15959U,
      0x3ff96effU, 0x3ed3e09cU, 0x3f000000U, 0xbfbeeab7U, 0x3fbd36d8U,
      0xbd71b27eU, 0x3f000000U, 0x3f0ba965U, 0x3f918f3bU, 0xbf0c6bcdU,
      0x3f000000U, 0x404cfb58U, 0x3ddc56b7U, 0xbe069444U, 0x3f000000U,
      0xbe7aaf6eU, 0x3ee9d7faU, 0xbdc60121U, 0x3f000000U, 0x3e306b87U,
      0x3e93c8f1U, 0x3ea40d34U, 0x3f000000U, 0xbe2a64bfU, 0xbd334a19U,
      0xbe2d2833U, 0x3f000000U, 0x3ea4c006U, 0x3ee05335U, 0xbf245582U,
      0x3f000000U, 0x3f76c091U, 0x3fd52c35U, 0xbfe405f4U, 0x3f000000U,
      0x3fb8c0b3U, 0xc00187f5U, 0x3f0008c8U, 0x3f000000U, 0x3e9b9600U,
      0xbfbcce6eU, 0x3fe0e116U, 0x3f000000U, 0xbe180e3eU, 0xbfc9673cU,
      0x3fa111ecU, 0x3f000000U, 0xbf01f488U, 0x3ee97e75U, 0x3f4b6610U,
      0x3f000000U, 0x3e1ac4d9U, 0x40748e68U, 0xbf97a17fU, 0x3f000000U,
      0xbff363f5U, 0x3ef0cf20U, 0x403fa40bU, 0x3f000000U, 0xbfad50c0U,
      0xbf8ee2f3U, 0x3fdc8ec6U, 0x3f000000U, 0xbf92566dU, 0x3f073d56U,
      0x3fa03751U, 0x3f000000U, 0xbf682556U, 0x400a93dcU, 0x3f42c170U,
      0x3f000000U, 0xc0305badU, 0x3fa4ac33U, 0x3fa7a234U, 0x3f000000U,
      0xbe5a6aaaU, 0x3f88983dU, 0x3f03845aU, 0x3f000000U, 0xbf8e3907U,
      0x3da0b541U, 0xbd263ca0U, 0x3f000000U, 0xbe924927U, 0x3ed4e68eU,
      0xbf050a38U, 0x3f000000U, 0x3f0a28e8U, 0x3f3f34eeU, 0xbf805ff3U,
      0x3f000000U, 0xbeb75cf7U, 0x3eafd2bbU, 0xbf95e3b5U, 0x3f000000U,
      0x400bc963U, 0xbf15365cU, 0xbedbd55fU, 0x3f000000U, 0x3eabb85eU,
      0xbe14caeaU, 0xbfd9aea7U, 0x3f000000U, 0x3f1263b2U, 0xbf066098U,
      0xc00b030fU, 0x3f000000U, 0x3f48585aU, 0xbf62ecbaU, 0xc02a6e5bU,
      0x3f000000U, 0x3faa3f63U, 0xc008c89dU, 0xc0074d9dU, 0x3f000000U,
      0xbea8fd8cU, 0xbeeb17cdU, 0xc03db2faU, 0x3f000000U, 0x3db23fb1U,
      0xbf65fd01U, 0x3ecd5cfaU, 0x3f000000U, 0xbece115dU, 0xbf0815d0U,
      0x401b8f0bU, 0x3f000000U, 0xbf601424U, 0xbf665474U, 0x401542a5U,
      0x3f000000U, 0xc000f2a2U, 0x3eb6c9e1U, 0x40386362U, 0x3f000000U,
      0x3fadd73eU, 0xbfffb9b5U, 0xbf4eeab4U, 0x3f000000U, 0x3e5fe055U,
      0xbe90a777U, 0x3ed68839U, 0x3f000000U, 0xbe8226c0U, 0x3f48d8c2U,
      0x3f66a256U, 0x3f000000U, 0xbf3e6407U, 0x3f09671eU, 0x3f11893eU,
      0x3f000000U, 0xbea73a91U, 0x3f742ddcU, 0x3ecd036fU, 0x3f000000U,
      0x40705badU, 0xbfc194e9U, 0x3f3fe2deU, 0x3f000000U, 0x3fe362d4U,
      0x3ee3a9bbU, 0xbe8f6183U, 0x3f000000U, 0x3fa803efU, 0x3f492932U,
      0xbf1efffeU, 0x3f000000U, 0x3f50698bU, 0x3ee2fad3U, 0xbf8bb824U,
      0x3f000000U, 0x400488bcU, 0xbf3238fbU, 0xbf0aed51U, 0x3f000000U,
  };
  const auto parameters =
      std::map<std::string, ParameterValue>{{"radius", int64_t{2}},
                                            {"sigma", .9}};
  auto prepared = d.prepare("image.gaussian_blur", {binding}, parameters);
  auto result = take(d.context->execute(prepared.plan, prepared.bindings))
                    .results.at("out");
  auto captured = take(result.descriptor());
  for (uint64_t i = 0; i < pixels.size(); ++i) {
    const std::vector<uint64_t> at{i / 120, i / 60 % 2, i / 20 % 3, i / 4 % 5,
                                   i % 4};
    uint32_t bits = 0;
    require(result.read_tensor(captured, 0, at, &bits, 4).ok() &&
                bits == expected[i],
            "Gaussian preserves old two-pass Float32 rounding across batches");
  }
  const auto output_shape = shape.tensors[0].sample_shape();
  const auto query = take(Footprint::from_regions(
      output_shape, {Region({{1, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 4}})}));
  auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
  auto partial = take(d.context->execute_fragments(frozen, {{"out", query}}));
  require(read(partial.results.at("out"), {1, 0, 0, 0, 0}) ==
              read(result, {1, 0, 0, 0, 0}),
          "Gaussian offset ROI matches full output");
  const auto needed = take(partial.dependencies.source_support()).at("image");
  require(needed == take(Footprint::from_regions(
                        output_shape,
                        {Region({{1, 1}, {0, 1}, {0, 3}, {0, 3}, {0, 4}})})),
          "Gaussian exact clipped halo support");
  const auto elsewhere = take(Footprint::from_regions(
      output_shape, {Region({{0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 4}})}));
  require(take(partial.dependencies.potential_dirty("image", elsewhere))
              .at("out")
              .empty(),
          "Gaussian dependency isolates frame and layer");
  for (const auto& values : std::vector<std::map<std::string, ParameterValue>>{
           {{"radius", int64_t{0}}, {"sigma", .9}},
           {{"radius", int64_t{65}}, {"sigma", .9}},
           {{"radius", int64_t{2}}, {"sigma", .09}},
           {{"radius", int64_t{2}},
            {"sigma", std::numeric_limits<double>::quiet_NaN()}}}) {
    OperationMetadata metadata;
    metadata.result_schema = std::make_shared<SchemaTemplate>(shape);
    require(
        !d.registry->resolve_traits("image.gaussian_blur", {metadata}, values)
             .ok(),
        "Gaussian parameter bounds");
  }
}
void image_split_storage_types() {
  Driver d;
  for (auto type :
       {ElementType::UInt8, ElementType::UInt16, ElementType::Float64}) {
    auto s = schema();
    s.tensors[0].batch_axes = {1, 1};
    s.tensors[0].descriptor = {type, {1, 2, 3}};
    s.tensors[0].facets.clear();
    const auto width = Value::element_size(type);
    std::vector<uint8_t> bytes(6 * width);
    for (uint64_t i = 0; i < 6; ++i) {
      if (type == ElementType::UInt8) {
        bytes[i] = static_cast<uint8_t>(i + 1);
      } else if (type == ElementType::UInt16) {
        const uint16_t value = static_cast<uint16_t>(257 * (i + 1));
        std::memcpy(bytes.data() + i * width, &value, width);
      } else {
        const double value = static_cast<double>(i + 1) + .25;
        std::memcpy(bytes.data() + i * width, &value, width);
      }
    }
    auto builder = take(ResultBuilder::start(d.root, s, "split.dtype"));
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(d.root, 1, {})))
                .ok(),
            "split dtype source descriptor");
    require(builder
                .publish_tensor(0, Region::whole(s.tensors[0].sample_shape()),
                                ByteView(bytes.data(), bytes.size()),
                                take(ResultRelation::cartesian(d.root, 6, {})),
                                {true, true, true, true})
                .ok(),
            "split dtype source samples");
    auto source = take(builder.seal());
    for (const std::string name : {"full", "left", "right"}) {
      auto output = d.run("image.split_horizontal", {{"image", source}},
                          {{"split_x", int64_t{1}}}, name);
      auto descriptor = take(output.descriptor());
      require(
          descriptor.tensor_coverage(0)
              .visit(
                  [&](const auto& at) {
                    uint8_t actual[8]{};
                    auto status =
                        output.read_tensor(descriptor, 0, at, actual, width);
                    const auto offset =
                        ((at[3] + (name == "right" ? 1 : 0)) * 3 + at[4]) *
                        width;
                    require(status.ok() &&
                                std::memcmp(actual, bytes.data() + offset,
                                            width) == 0,
                            "split preserves the complete dtype byte width");
                    return Status::success();
                  },
                  6)
              .ok(),
          "split dtype traversal");
    }
  }
}
void gaussian_sparse_domain() {
  Driver d;
  auto s = schema();
  s.tensors[0].batch_axes = {UINT64_MAX, 2};
  s.tensors[0].descriptor.shape = {1, 1, 4};
  auto builder = take(ResultBuilder::start(d.root, s, "gaussian.sparse"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(d.root, 1, {})))
              .ok(),
          "sparse Gaussian descriptor");
  const float pixels[] = {.25F, -.5F, 1.F, .5F};
  const Region cell({{0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 4}});
  require(builder
              .publish_tensor(
                  0, cell,
                  ByteView(reinterpret_cast<const uint8_t*>(pixels),
                           sizeof(pixels)),
                  take(ResultRelation::cartesian(d.root, UINT64_MAX, {})),
                  {true, true, true, true})
              .ok(),
          "sparse Gaussian source");
  auto prepared =
      d.prepare("image.gaussian_blur", {{"image", take(builder.seal())}},
                {{"radius", int64_t{1}}, {"sigma", .9}});
  auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
  auto query =
      take(Footprint::from_regions(s.tensors[0].sample_shape(), {cell}));
  auto result = take(d.context->execute_fragments(frozen, {{"out", query}}));
  for (uint64_t c = 0; c < 4; ++c)
    require(read(result.results.at("out"), {0, 0, 0, 0, c}) == pixels[c],
            "Gaussian bounded ROI accepts unrepresentable full cardinality");
  auto empty = take(d.context->execute_fragments(
      frozen, {{"out", take(Footprint::none(s.tensors[0].sample_shape()))}}));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Gaussian Empty sparse domain publishes no samples");
  require(take(empty.dependencies.source_observations()).empty(),
          "Gaussian Empty consumes no source payload or descriptor");
}

struct NativeRetryGuard {
  explicit NativeRetryGuard(unsigned value) : mode(value) {}
  unsigned mode, stage = 0;
  std::optional<ResultBuilder> builder;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Poll = Result<ResultProgramPoll>;
    if (phase.query.backend == Backend::Cpu)
      return ScalarPreludeState(false, .25F).poll(phase);
    if (mode == 6 && stage++ < 2) {
      ResultProgramNeed need;
      if (stage == 1)
        need.io.push_back(ResultCreateTemporary{});
      else
        need.tensors.push_back({0, 0, take(Footprint::all({1})), 1});
      return Poll(std::move(need));
    }
    if (mode == 7) {
      auto restored = phase.checkpoint_before(1, 0);
      if (!restored.ok())
        return Poll(restored.status());
    }
    if (mode == 8 && !builder) {
      builder.emplace(take(ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key)));
      require(builder
                  ->bind_descriptor_relation(
                      take(ResultRelation::cartesian(phase.resources, 1, {})))
                  .ok(),
              "retry guard prefix descriptor");
      require(
          builder
              ->publish(0, 0,
                        take(ResultRelation::cartesian(phase.resources, 1, {})),
                        {true, true, true, true})
              .ok(),
          "retry guard empty prefix");
      return Poll(ResultPublication{builder->reference(), false});
    }
    if (mode == 5) {
      float ignored;
      const auto denied = phase.read_tensor(0, 0, {0}, &ignored, 4);
      require(!denied.ok(), "retry guard unauthorized read");
    }
    if (mode == 10) {
      const auto denied = phase.consume_work(UINT64_MAX);
      require(!denied.ok(), "retry guard work limit");
    }
    constexpr ErrorCode errors[] = {
        ErrorCode::BackendUnavailable, ErrorCode::OperationFailed,
        ErrorCode::ResourceExhausted, ErrorCode::Cancelled, ErrorCode::Stale};
    return Poll(Status{mode < 5 ? errors[mode] : ErrorCode::BackendUnavailable,
                       "retry guard failure"});
  }
};
void native_retry_guards() {
  for (unsigned mode = 0; mode <= 10; ++mode) {
    auto registry = std::make_shared<OperationRegistry>();
    unsigned cpu_starts = 0;
    SchemaTemplate output_schema;
    output_schema.id = "test.native.retry";
    ResultTensorSpec tensor;
    tensor.key = "sample";
    tensor.descriptor = {ElementType::Float32, {1}};
    output_schema.tensors.push_back(tensor);
    if (mode == 8) {
      output_schema.tensors.clear();
      output_schema.publication = PublishPolicy::StablePrefix;
      output_schema.fields = {{"sample",
                               ElementType::Float32,
                               {ResultExtentKind::RuntimeCount},
                               {}}};
    }
    OperationDefinition op;
    op.key = "test.native.retry";
    op.traits.input_count = 1;
    OperationPortConstraint input_port;
    input_port.kind = OperationPortKind::Result;
    input_port.result_schema_id = "test.scalar";
    input_port.result_schema_version = 1;
    op.traits.input_schema = {input_port};
    op.traits.supports_gpu = op.traits.allows_cpu_fallback = true;
    op.traits.side_effect_free = mode != 9;
    op.traits.cacheable = false;
    auto& out = op.traits.outputs[0];
    out.output_schema.kind = OperationPortKind::Result;
    out.output_schema.result_schema_id = output_schema.id;
    out.output_schema.result_schema_version = 1;
    out.result_schema = output_schema;
    out.dependency_version = 2;
    out.region_rule = OperationRegionRule::Dependency;
    out.maximum_dependency_stages = 4;
    out.continuation_bytes = sizeof(NativeRetryGuard);
    op.start_result = [&](const ResultProgramQuery& query,
                          const BufferAllocator& allocator) {
      if (query.backend == Backend::Cpu)
        ++cpu_starts;
      return ResultContinuation::make<NativeRetryGuard>(allocator, mode);
    };
    auto registered = registry->register_operation(std::move(op));
    if (mode == 9) {
      require(!registered.ok() && registered.code == ErrorCode::InvalidArgument,
              "side-effecting structured callbacks cannot register");
      continue;
    }
    if (!registered.ok())
      throw std::runtime_error(
          "retry guard registration mode=" + std::to_string(mode) + ": " +
          registered.message);
    require(registry->freeze().ok(), "retry guard registry freeze");
    ExecutionContextConfig config;
    config.gpu_enabled = true;
    config.managed_resources = ResourceLimits{};
    ExecutionContext context(registry, config);
    WorkflowDocument document;
    WorkflowNode node;
    node.id = 1;
    node.operation = "test.native.retry";
    node.inputs = {WorkflowInputReference{1}};
    auto root = take(context.resource_budget());
    auto input = scalar(root, .5F);
    WorkflowInputDeclaration declaration;
    declaration.id = 1;
    declaration.name = "input";
    declaration.result_schema =
        std::make_shared<SchemaTemplate>(input.schema());
    document.inputs.push_back(std::move(declaration));
    document.nodes.push_back(std::move(node));
    document.outputs = {{"out", 1, "value"}};
    GraphContext graph(document);
    PlanningOptions planning;
    planning.execution_mode = ExecutionMode::NativeGpu;
    auto compiled = take(Compiler(registry).compile(graph, planning));
    const auto before = root.statistics().live[ResourceKind::Payload];
    {
      auto run = context.execute(compiled.plan, {{{"input", input}}});
      if (mode == 0) {
        require(run.ok() && cpu_starts == 1 &&
                    run.value().diagnostics.fallback_reasons.size() == 1,
                "recoverable pure poll failure restarts CPU exactly once");
      } else {
        const auto expected = mode == 1 ? ErrorCode::OperationFailed
                              : mode == 2 || mode == 10
                                  ? ErrorCode::ResourceExhausted
                              : mode == 3 ? ErrorCode::Cancelled
                              : mode == 4 ? ErrorCode::Stale
                              : mode == 5 ? ErrorCode::InvalidArgument
                                          : ErrorCode::BackendUnavailable;
        if (run.ok() || run.status().code != expected || cpu_starts != 0)
          throw std::runtime_error(
              "retry guard mode=" + std::to_string(mode) + " status=" +
              std::to_string(static_cast<unsigned>(run.status().code)) +
              " expected=" + std::to_string(static_cast<unsigned>(expected)) +
              " cpu_starts=" + std::to_string(cpu_starts) +
              " message=" + run.status().message);
      }
    }
    require(root.statistics().live[ResourceKind::Payload] == before,
            "retry guards retire all temporary payloads");
  }
}
bool native_image_algorithms() {
  Driver d(true);
  if (!d.context->gpu_enabled())
    return false;
  ExecutionBinding a{"a", image(d.root)};
  ExecutionBinding b{"b", image(d.root, false, 2)};
  ExecutionBinding mask{"mask", image(d.root, true)};
  ExecutionBinding gain{"gain", scalar(d.root, 2)};
  ExecutionBinding opacity{"opacity", scalar(d.root, .25F)};
  std::vector<ExecutionBinding> brush{a};
  for (float value : {4.5F, 2.5F, .5F, 1.F, 0.F, 0.F, .25F})
    brush.push_back(
        {"p" + std::to_string(brush.size()), scalar(d.root, value)});
  struct Case {
    std::string key;
    std::vector<ExecutionBinding> inputs;
    std::map<std::string, ParameterValue> parameters;
  };
  const std::vector<Case> cases{
      {"image.exposure_gain", {a, gain}, {}},
      {"image.opacity", {a, opacity}, {}},
      {"image.gaussian_blur", {a}, {{"radius", int64_t{2}}, {"sigma", .9}}},
      {"image.mask", {a, mask}, {}},
      {"image.source_over", {a, b}, {}},
      {"image.downsample_box", {a}, {{"factor", int64_t{2}}}},
      {"mask.downsample_box", {mask}, {{"factor", int64_t{2}}}},
      {"image.brush_circle", brush, {}}};
  uint64_t dispatches = 0;
  for (const auto& item : cases) {
    d.native = false;
    const auto reference = d.run(item.key, item.inputs, item.parameters);
    d.native = true;
    auto prepared = d.prepare(item.key, item.inputs, item.parameters);
    auto run = take(d.context->execute(prepared.plan, prepared.bindings));
    require(run.diagnostics.native_dispatch_count > 0 &&
                run.diagnostics.fallback_reasons.empty(),
            "image GPU case must execute native without fallback");
    dispatches += run.diagnostics.native_dispatch_count;
    const auto result = run.results.at("out");
    auto query =
        take(Footprint::all(result.schema().tensors[0].sample_shape()));
    require(query
                .visit(
                    [&](const auto& at) {
                      const auto expected = read(reference, at),
                                 actual = read(result, at);
                      require(
                          std::abs(actual - expected) <=
                              1e-6F + 1e-5F * std::abs(expected),
                          "native image result matches CPU numerical contract");
                      return Status::success();
                    },
                    UINT64_MAX)
                .ok(),
            "native image traversal");
  }
  auto make_image = [&](uint64_t width, float rgb) {
    auto s = schema();
    s.tensors[0].batch_axes = {1, 1};
    s.tensors[0].descriptor.shape = {1, width, 4};
    auto builder = take(ResultBuilder::start(d.root, s, "native.boundary"));
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(d.root, 1, {0, 8, 0, 0})))
                .ok(),
            "native boundary descriptor");
    std::vector<float> pixels(width * 4, rgb);
    for (uint64_t i = 3; i < pixels.size(); i += 4)
      pixels[i] = .5F;
    require(builder
                .publish_tensor(
                    0, Region::whole(s.tensors[0].sample_shape()),
                    ByteView(reinterpret_cast<const uint8_t*>(pixels.data()),
                             pixels.size() * 4),
                    take(ResultRelation::cartesian(d.root, pixels.size(),
                                                   {0, 1, 0, 0})),
                    {true, true, true, true})
                .ok(),
            "native boundary pixels");
    return take(builder.seal());
  };
  {
    auto source = make_image(64 * 205, .25F);
    auto prepared = d.prepare("image.opacity", {{"a", source}, opacity});
    ExecutionOptions options;
    options.maximum_dependency_work = UINT64_MAX;
    options.dependencies.maximum_work = UINT64_MAX;
    auto output =
        take(d.context->execute(prepared.plan, prepared.bindings, {}, options));
    require(output.diagnostics.native_dispatch_count >= 205 &&
                output.diagnostics.fallback_reasons.empty(),
            "205 native tiles release buffer tokens after each dispatch");
    require(
        read(output.results.at("out"), {0, 0, 0, 64 * 205 - 1, 0}) == .0625F,
        "last native tile published correctly");
    dispatches += output.diagnostics.native_dispatch_count;
  }
  {
    auto source = make_image(1, 1e-30F);
    std::vector<ExecutionBinding> inputs{{"a", source}, opacity};
    d.native = false;
    auto reference = d.run("image.opacity", inputs);
    d.native = true;
    auto prepared = d.prepare("image.opacity", inputs);
    const auto before = d.root.statistics().live[ResourceKind::Payload];
    {
      auto output = take(d.context->execute(prepared.plan, prepared.bindings));
      require(output.diagnostics.fallback_reasons.size() == 1 &&
                  output.diagnostics.selected_backends.at(
                      prepared.plan.steps()[0].result_ref()) == Backend::Cpu,
              "native domain rejection restarts exact CPU continuation");
      for (uint64_t c = 0; c < 4; ++c)
        require(read(reference, {0, 0, 0, 0, c}) ==
                    read(output.results.at("out"), {0, 0, 0, 0, c}),
                "fallback preserves exact CPU samples");
    }
    require(d.root.statistics().live[ResourceKind::Payload] == before,
            "failed GPU attempt and fallback output release payloads");
    ExecutionOptions limited;
    limited.dependencies.maximum_stages = 3;
    auto failed =
        d.context->execute(prepared.plan, prepared.bindings, {}, limited);
    require(
        !failed.ok() && failed.status().code == ErrorCode::ResourceExhausted,
        "fallback cannot reset caller stage budget");
  }
  ResultRef retained_chain;
  {
    Driver chain(true);
    std::vector<ExecutionBinding> inputs{{"image", image(chain.root)},
                                         {"gain", scalar(chain.root, 2)},
                                         {"opacity", scalar(chain.root, .25F)}};
    WorkflowDocument document;
    for (unsigned i = 0; i < inputs.size(); ++i) {
      WorkflowInputDeclaration declaration;
      declaration.id = i + 1;
      declaration.name = inputs[i].name;
      declaration.result_schema =
          std::make_shared<SchemaTemplate>(inputs[i].result.schema());
      document.inputs.push_back(std::move(declaration));
    }
    document.nodes = {
        {1,
         "image.exposure_gain",
         {WorkflowInputReference{1}, WorkflowInputReference{2}},
         {}},
        {2,
         "image.opacity",
         {WorkflowNodeOutput{1, "value"}, WorkflowInputReference{3}},
         {}}};
    document.outputs = {{"out", 2, "value"}};
    GraphContext graph(document);
    PlanningOptions options;
    options.execution_mode = ExecutionMode::NativeGpu;
    auto compiled = take(Compiler(chain.registry).compile(graph, options));
    auto result =
        take(chain.context->execute(compiled.plan, {std::move(inputs)}));
    require(
        result.diagnostics.native_dispatch_count == 8 &&
            result.diagnostics.transfer_count == 4 &&
            result.diagnostics.fallback_reasons.empty(),
        "native Result chain reuses GPU backing without intermediate uploads");
    retained_chain = result.results.at("out");
  }
  require(read(retained_chain, {0, 0, 0, 1, 0}) == .0625F &&
              read(retained_chain, {1, 1, 0, 1, 3}) == .125F,
          "native Result output retains backing after context retirement");
  native_retry_guards();
  std::cout << "eight Result image GPU algorithms passed; native_dispatches="
            << dispatches << '\n';
  return true;
}
void image_control_contracts() {
  Driver d;
  ExecutionBinding image_input, control;
  image_input.name = "image";
  image_input.result = image(d.root);
  control.name = "gain";
  control.result = scalar(d.root, .5F);
  auto prepared = d.prepare("image.opacity", {image_input, control});
  auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
  const auto shape = image_input.result.schema().tensors[0].sample_shape();
  auto query = take(Footprint::from_regions(
      shape, {Region({{1, 1}, {0, 1}, {1, 1}, {2, 1}, {0, 4}})}));
  auto output = take(d.context->execute_fragments(frozen, {{"out", query}}));
  require(read(output.results.at("out"), {1, 0, 1, 2, 0}) ==
              .5F * read(image_input.result, {1, 0, 1, 2, 0}),
          "Result scalar opacity accepts unaligned singleton stride");
  auto changed = take(Footprint::all({1}));
  for (uint32_t role : {1U, 4U})
    require(take(output.dependencies.potential_dirty("gain", changed, role))
                    .at("out") == query,
            "scalar data and validation edits dirty every observed pixel");
  auto empty_output = take(d.context->execute_fragments(
      frozen, {{"out", take(Footprint::none(shape))}}));
  bool scalar_validation = false;
  for (const auto& observation :
       take(empty_output.dependencies.source_observations())) {
    scalar_validation |=
        observation.input == "gain" && (observation.roles & 4U) != 0;
    require(observation.input != "image" ||
                observation.target == ResultSupportTarget::Descriptor,
            "Empty opacity reads no image samples");
  }
  require(scalar_validation && take(empty_output.results.at("out").descriptor())
                                   .tensor_coverage(0)
                                   .empty(),
          "Empty opacity still validates its Result scalar control");
  for (bool empty : {false, true}) {
    for (float value : {-1.F, 1.5F, std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()}) {
      control.result = scalar(d.root, value);
      auto invalid = d.prepare("image.opacity", {image_input, control});
      const auto before = d.root.statistics().live[ResourceKind::Payload];
      auto captured = d.context->freeze(invalid.plan, invalid.bindings);
      auto failed =
          captured.ok()
              ? d.context->execute_fragments(
                    captured.value(),
                    {{"out", empty ? take(Footprint::none(shape)) : query}})
              : Result<DemandResult>(captured.status());
      require(!failed.ok() &&
                  failed.status().code == ErrorCode::InvalidArgument &&
                  failed.status().detail.input_id == 2 &&
                  d.root.statistics().live[ResourceKind::Payload] == before,
              "direct Result opacity control rejects invalid numbers even "
              "for Empty and releases unpublished output");
    }
  }
  control.result = scalar(d.root, 16);
  require(
      read(d.run("image.exposure_gain", {image_input, control}),
           {0, 0, 0, 1, 0}) == 16 * read(image_input.result, {0, 0, 0, 1, 0}),
      "exposure accepts its inclusive maximum Result control");
  control.result = scalar(d.root, 16.5F);
  auto exposure = d.prepare("image.exposure_gain", {image_input, control});
  require(!d.context->execute(exposure.plan, exposure.bindings).ok(),
          "exposure retains maximum control bound");
  std::vector<ExecutionBinding> brush{image_input};
  for (float value : {0.F, 0.F, 0.F, 1.F, 0.F, 0.F, .5F}) {
    ExecutionBinding input;
    input.name = "p" + std::to_string(brush.size());
    input.result = scalar(d.root, value);
    brush.push_back(std::move(input));
  }
  auto radius = d.prepare("image.brush_circle", brush);
  auto rejected = d.context->execute(radius.plan, radius.bindings);
  require(!rejected.ok() &&
              rejected.status().code == ErrorCode::InvalidArgument &&
              rejected.status().detail.input_id == 4,
          "brush Result radius must be positive normal Float32");
  brush[3].result = scalar(d.root, .5F);
  brush[7].result = scalar(d.root, 1.5F);
  auto alpha = d.prepare("image.brush_circle", brush);
  rejected = d.context->execute(alpha.plan, alpha.bindings);
  require(!rejected.ok() && rejected.status().detail.input_id == 8,
          "brush Result alpha retains unit interval bound");
}
void run() {
  Driver d;
  ExecutionBinding a, b, mask, control;
  a.name = "a";
  a.result = image(d.root);
  b.name = "b";
  b.result = image(d.root, false, 2);
  mask.name = "mask";
  mask.result = image(d.root, true);
  control.name = "control";
  control.result = scalar(d.root, 2);
  const std::vector<std::uint64_t> at{1, 1, 2, 4, 0};
  const auto x = read(a.result, at);
  require(read(d.run("image.exposure_gain", {a, control}), at) == 2 * x,
          "exposure across frames and layers");
  control.result = scalar(d.root, .25F);
  require(read(d.run("image.opacity", {a, control}), at) == x * .25F,
          "opacity");
  require(read(d.run("image.mask", {a, mask}), at) == x * .25F, "mask");
  require(read(d.run("image.source_over", {a, b}), at) == x + (x + 2) * .5F,
          "source-over");
  require(read(d.run("image.mix", {a, b, mask}), at) == x + .5F, "mix");
  auto reduced =
      d.run("image.downsample_box", {a}, {{"factor", std::int64_t{2}}});
  require(reduced.schema().tensors[0].descriptor.shape ==
              std::vector<std::uint64_t>({2, 3, 4}),
          "ceil downsample shape");
  require(read(reduced, {1, 1, 1, 2, 0}) == x, "clipped downsample edge");
  require(
      read(d.run("mask.downsample_box", {mask}, {{"factor", std::int64_t{2}}}),
           {1, 1, 1, 2}) == .25F,
      "mask downsample");
  require(read(d.run("image.split_horizontal", {a},
                     {{"split_x", std::int64_t{2}}}, "right"),
               {1, 1, 2, 2, 0}) == x,
          "selected right crop");
  std::vector<ExecutionBinding> brush{a};
  for (float number : {4.5F, 2.5F, .5F, 1.F, 0.F, 0.F, .25F}) {
    ExecutionBinding input;
    input.name = "p" + std::to_string(brush.size());
    input.result = scalar(d.root, number);
    brush.push_back(std::move(input));
  }
  require(read(d.run("image.brush_circle", brush), at) == .25F + .75F * x,
          "brush coverage");
  auto prepared = d.prepare("image.mask", {a, mask});
  auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
  auto wanted = take(Footprint::from_regions(
      schema().tensors[0].sample_shape(),
      {Region({{1, 1}, {0, 1}, {1, 1}, {1, 1}, {0, 4}}),
       Region({{0, 1}, {1, 1}, {2, 1}, {4, 1}, {0, 4}})}));
  auto result = take(d.context->execute_fragments(frozen, {{"out", wanted}}));
  auto out = result.results.at("out");
  require(read(out, {1, 0, 1, 1, 0}) == read(a.result, {1, 0, 1, 1, 0}) * .25F,
          "nonzero sparse ROI");
  float missing = 0;
  require(
      !out.read_tensor(take(out.descriptor()), 0, {1, 0, 1, 2, 0}, &missing, 4)
           .ok(),
      "sparse holes remain unauthorized");
  auto changed =
      take(Footprint::from_regions(schema(true).tensors[0].sample_shape(),
                                   {Region({{1, 1}, {0, 1}, {1, 1}, {1, 1}})}));
  auto dirty = take(result.dependencies.potential_dirty("mask", changed));
  require(take(dirty.at("out").element_count()) == 4,
          "mask support maps to exactly one color");
  auto empty = take(d.context->execute_fragments(
      frozen, {{"out", take(Footprint::none(wanted.shape()))}}));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty remains empty");
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--native-only")
      return native_image_algorithms() ? 0 : 77;
    tensor_writer_reentrancy();
    empty_tensor_writers();
    direct_tensor_writers();
    lazy_spatial_backing();
    affine_source_views();
    foundation_tensor_workflows();
    scalar_prelude_workflows();
    repeated_tensor_shapes();
    reshape_relations();
    affine_view_failures();
    backing_independent_spatial_runs();
    spatial_tensor_batches();
    indexed_batch_resources();
    numerical_tensor_windows();
    mix_cancellation_residual();
    window_failures();
    view_graph_and_growth();
    large_view();
    prefix_relations();
    mapped_relation_capacity_failure();
    anchored_tensor_relations();
    mapped_relations();
    windows_and_views();
    image_schema_contracts();
    image_control_contracts();
    image_gaussian_contract();
    gaussian_sparse_domain();
    image_split_storage_types();
    run();
    std::cout << "built-in Result tensors passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
