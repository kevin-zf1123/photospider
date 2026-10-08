#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "data/affine_view.hpp"
#include "support/result_numeric_observation_fixture.hpp"

namespace {
using namespace ps::test_numeric;  // NOLINT(build/namespaces)
ResultRef row_fragments(Driver& driver, bool related) {
  auto schema = SchemaTemplate{};
  schema.id = "test.fragments";
  ResultTensorSpec member;
  member.key = "samples";
  member.descriptor = {ElementType::Float64, {2, 2}};
  schema.tensors.push_back(member);
  auto owner = take(driver.root.allocator().allocate(33));
  for (uint64_t i = 0; i < 4; ++i) {
    const auto bits = double_bits(static_cast<double>(i + 1));
    std::memcpy(owner.data() + 1 + i * 8, &bits, 8);
  }
  auto common = std::move(owner).freeze();
  auto builder =
      take(ResultBuilder::start(driver.root, schema, "fragmented.source"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(driver.root, 1, {0, 8, 0, 0})))
              .ok(),
          "fragment source descriptor");
  auto relation = take(ResultRelation::cartesian(driver.root, 4, {0, 1, 0, 0}));
  for (uint64_t row = 0; row < 2; ++row) {
    std::shared_ptr<const CpuStorage> storage = common;
    if (!related) {
      auto buffer = take(driver.root.allocator().allocate(33));
      std::memcpy(buffer.data(), common->bytes().data(), 33);
      storage = std::move(buffer).freeze();
    }
    require(builder
                .publish_tensor(0, Region({{row, 1}, {0, 2}}),
                                {1 + row * 16, {INT64_MIN, 8}, {row, 0}},
                                storage, relation, {true, true, true, true})
                .ok(),
            "same-owner singleton row publication");
  }
  return take(builder.seal());
}
void layout_views() {
  Driver d;
  auto input = d.source({ElementType::Float64, {2, 3}},
                        {double_bits(1), double_bits(2), double_bits(3),
                         double_bits(4), double_bits(5), double_bits(6)},
                        {41, {-24, -8}});
  auto source_window = take(
      input.acquire_tensor(take(input.descriptor()), 0, Region::whole({2, 3})));
  const auto source_pointer = take(source_window.row_run({0, 0})).data;
  const auto source_owner = source_window.storage_owner_token();
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  auto reshaped = take(d.run("array.reshape_strict", {input}, {},
                             {{"shape", std::string("3,2")},
                              {"layout", std::string("view")}}))
                      .results.at("out");
  auto reshape_window = take(reshaped.acquire_tensor(
      take(reshaped.descriptor()), 0, Region::whole({3, 2})));
  require(reshape_window.storage_owner_token() == source_owner &&
              take(reshape_window.row_run({0, 0})).data == source_pointer &&
              take(reshape_window.row_run({0, 0})).sample_stride_bytes == -8 &&
              d.root.statistics().live[ResourceKind::Payload] == payload,
          "reshape preserves negative contiguous chunks as zero-payload view");
  for (uint64_t i = 0; i < 6; ++i)
    require(read_bits(reshaped, {i / 2, i % 2}) == double_bits(6 - i),
            "reshape logical order follows complete source ordinal");
  auto transposed = take(d.run(
      "array.transpose_strict", {input}, {},
      {{"permutation", std::string("1,0")}, {"layout", std::string("view")}}));
  auto transpose = transposed.results.at("out");
  require(read_bits(transpose, {2, 1}) == double_bits(1) &&
              read_bits(transpose, {0, 1}) == double_bits(3),
          "transpose permutes signed strides without copying");
  auto changed =
      take(Footprint::from_regions({2, 3}, {Region({{1, 1}, {2, 1}})}));
  require(take(transposed.dependencies.potential_dirty("input0", changed, 1))
                  .at("out") == take(Footprint::all({3, 2})),
          "layout preserves Whole data support despite physical mapping");
  auto starts = d.source({ElementType::Int64, {2}}, {1, 2}, {1, {8}});
  auto steps = d.source({ElementType::Int64, {2}},
                        {UINT64_C(0x8000000000000000), UINT64_MAX}, {1, {8}});
  auto sliced = take(d.run("array.slice_strict", {input, starts, steps}, {},
                           {{"counts", std::string("1,2")},
                            {"layout", std::string("view")}}))
                    .results.at("out");
  require(read_bits(sliced, {0, 0}) == double_bits(1) &&
              read_bits(sliced, {0, 1}) == double_bits(2),
          "slice ignores singleton steps and preserves reverse physical view");
  auto fragment = row_fragments(d, true);
  const auto join_payload = d.root.statistics().live[ResourceKind::Payload];
  auto joined = take(d.run("array.reshape_strict", {fragment}, {},
                           {{"shape", std::string("4")},
                            {"layout", std::string("view")}}))
                    .results.at("out");
  auto original = take(fragment.acquire_tensor(take(fragment.descriptor()), 0,
                                               Region::whole({2, 2})));
  auto joined_window = take(
      joined.acquire_tensor(take(joined.descriptor()), 0, Region::whole({4})));
  require(
      d.root.statistics().live[ResourceKind::Payload] == join_payload &&
          original.storage_owner_token() ==
              joined_window.storage_owner_token() &&
          take(original.row_run({0, 0})).data ==
              take(joined_window.row_run({0})).data,
      "same-owner singleton fragments infer the complete affine row stride");
  for (uint64_t i = 0; i < 4; ++i)
    require(read_bits(joined, {i}) == double_bits(i + 1),
            "fragment view retains all source bytes");
  auto fragmented = row_fragments(d, false);
  auto point = take(Footprint::from_regions({4}, {Region({{0, 1}})}));
  auto unavailable =
      d.run("array.reshape_strict", {fragmented}, point,
            {{"shape", std::string("4")}, {"layout", std::string("view")}});
  require(!unavailable.ok() &&
              unavailable.status().code == ErrorCode::InvalidArgument &&
              unavailable.status().reason == FailureReason::InvalidDomain &&
              unavailable.status().detail.scope == FailureScope::Run,
          "small Q cannot make a multi-owner source a globally affine view");
  for (const auto* layout : {"auto", "dense"}) {
    auto copied = take(d.run("array.reshape_strict", {fragmented}, {},
                             {{"shape", std::string("4")},
                              {"layout", std::string(layout)}}))
                      .results.at("out");
    for (uint64_t i = 0; i < 4; ++i)
      require(read_bits(copied, {i}) == double_bits(i + 1),
              "Auto and Dense preserve fragmented raw values");
  }
  auto zero = d.source({ElementType::Float64, {UINT64_C(1) << 40}},
                       {double_bits(7)}, {1, {0}});
  const auto before = d.root.statistics().live[ResourceKind::Payload];
  auto huge = take(d.run("array.reshape_strict", {zero}, {},
                         {{"shape", std::string("1048576,1048576")},
                          {"layout", std::string("view")}}))
                  .results.at("out");
  require(
      read_bits(huge, {1048575, 1048575}) == double_bits(7) &&
          d.root.statistics().live[ResourceKind::Payload] == before,
      "large zero-stride reshape proves chunks without materializing samples");
  source_window = {};
  original = {};
  input = {};
  fragment = {};
  d.context.reset();
  require(read_bits(joined, {3}) == double_bits(4) &&
              read_bits(reshaped, {2, 1}) == double_bits(1),
          "array views retain source owner after input and context retirement");
}
void layout_profiles() {
  Driver d;
  auto input = d.source({ElementType::Float64, {2, 3}},
                        {double_bits(1), double_bits(2), double_bits(3),
                         double_bits(4), double_bits(5), double_bits(6)},
                        {1, {24, 8}});
  auto starts = d.source({ElementType::Int64, {2}}, {0, 0}, {1, {8}});
  auto steps = d.source({ElementType::Int64, {2}}, {1, 1}, {1, {8}});
  for (const auto* suffix :
       {"_strict", "_accelerated_apple_silicon", "_accelerated_x86_64"}) {
    for (const auto* kind : {"reshape", "transpose", "slice"}) {
      std::vector<ResultRef> inputs{input};
      std::map<std::string, ParameterValue> parameters{
          {"layout", std::string("dense")}};
      if (std::string(kind) == "reshape") {
        parameters["shape"] = std::string("3,2");
      } else if (std::string(kind) == "transpose") {
        parameters["permutation"] = std::string("1,0");
      } else {
        inputs.push_back(starts);
        inputs.push_back(steps);
        parameters["counts"] = std::string("2,3");
      }
      std::vector<OperationMetadata> metadata;
      for (const auto& value : inputs) {
        OperationMetadata member;
        member.result_schema = std::make_shared<SchemaTemplate>(value.schema());
        metadata.push_back(std::move(member));
      }
      const auto key = std::string("array.") + kind + suffix;
      auto resolved = d.registry->resolve_traits(key, metadata, parameters);
      if (!resolved.ok()) {
        require(std::string(suffix) != "_strict" &&
                    resolved.status().code == ErrorCode::BackendUnavailable,
                "unavailable array profile reports BackendUnavailable");
        continue;
      }
      auto output = take(d.run(key, inputs, {}, parameters)).results.at("out");
      require(read_bits(output, std::string(kind) == "slice"
                                    ? std::vector<uint64_t>{1, 2}
                                    : std::vector<uint64_t>{2, 1}) ==
                  double_bits(6),
              "each available layout profile preserves raw values");
    }
  }
  auto bad = d.source({ElementType::Int64, {1}}, {1}, {1, {8}});
  std::vector<OperationMetadata> metadata;
  for (const auto& value : {input, starts, bad}) {
    OperationMetadata member;
    member.result_schema = std::make_shared<SchemaTemplate>(value.schema());
    metadata.push_back(std::move(member));
  }
  auto rejected = d.registry->resolve_traits(
      "array.slice_strict", metadata,
      {{"counts", std::string("1,1")}, {"layout", std::string("view")}});
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
          "excluded singleton step still obeys complete static metadata "
          "validation");
}
void constant_and_broadcast() {
  Driver d;
  const auto word = UINT64_C(0x7ff0000000000123);
  std::vector<uint64_t> oversized(4096, 0);
  oversized[0] = word;
  auto scalar = d.source({ElementType::Float64, {1}}, oversized, {1, {8}});
  const auto scalar_weak = scalar.weak();
  const auto scalar_id = scalar.object_id();
  auto scalar_window = take(
      scalar.acquire_tensor(take(scalar.descriptor()), 0, Region::whole({1})));
  const auto before = d.root.statistics().live[ResourceKind::Payload];
  auto huge = take(d.run("numeric.constant_strict", {scalar}, {},
                         {{"shape", std::string("1048576,1048576")},
                          {"layout", std::string("view")}}))
                  .results.at("out");
  auto window = take(huge.acquire_tensor(take(huge.descriptor()), 0,
                                         Region::whole({1048576, 1048576})));
  require(
      d.root.statistics().live[ResourceKind::Payload] == before + 8 &&
          window.storage_owner_token() != scalar_window.storage_owner_token() &&
          read_bits(huge, {1048575, 1048575}) == word &&
          huge.schema().tensors[0].atomic_trailing_axes == 2,
      "constant view copies only one scalar and preserves full-array tuple "
      "identity");
  auto source = d.source({ElementType::Int64, {2, 1, 3}}, {1, 2, 3, 4, 5, 6},
                         {41, {-24, INT64_MIN, -8}});
  auto original = take(source.acquire_tensor(take(source.descriptor()), 0,
                                             Region::whole({2, 1, 3})));
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  auto broadcast = take(d.run("numeric.broadcast_strict", {source}, {},
                              {{"shape", std::string("3,2,4")},
                               {"axis_map", std::string("1,2,0")},
                               {"layout", std::string("view")}}))
                       .results.at("out");
  auto borrowed = take(broadcast.acquire_tensor(take(broadcast.descriptor()), 0,
                                                Region::whole({3, 2, 4})));
  require(
      d.root.statistics().live[ResourceKind::Payload] == payload &&
          borrowed.storage_owner_token() == original.storage_owner_token() &&
          take(borrowed.row_run({0, 0, 0})).data ==
              take(original.row_run({0, 0, 0})).data,
      "broadcast view preserves source owner, negative strides and singleton "
      "expansion");
  for (uint64_t i = 0; i < 3; ++i)
    for (uint64_t j = 0; j < 2; ++j)
      for (uint64_t k = 0; k < 4; ++k)
        require(read_bits(broadcast, {i, j, k}) == 6 - j * 3 - i,
                "broadcast logical axis permutation remains exact");
  for (const auto* suffix :
       {"_strict", "_accelerated_apple_silicon", "_accelerated_x86_64"}) {
    for (bool constant : {true, false}) {
      const auto input = constant ? scalar : source;
      const auto key =
          std::string(constant ? "numeric.constant" : "numeric.broadcast") +
          suffix;
      std::map<std::string, ParameterValue> parameters{
          {"shape", std::string(constant ? "2,8" : "3,2,4")},
          {"layout", std::string("dense")}};
      if (!constant)
        parameters["axis_map"] = std::string("1,2,0");
      OperationMetadata metadata;
      metadata.result_schema = std::make_shared<SchemaTemplate>(input.schema());
      auto traits = d.registry->resolve_traits(key, {metadata}, parameters);
      if (!traits.ok()) {
        require(std::string(suffix) != "_strict" &&
                    traits.status().code == ErrorCode::BackendUnavailable,
                "unavailable array profile returns BackendUnavailable");
        continue;
      }
      auto dense = take(d.run(key, {input}, {}, parameters)).results.at("out");
      require(constant ? read_bits(dense, {1, 7}) == word
                       : read_bits(dense, {2, 1, 3}) == 1,
              "available array dense profiles copy raw sNaN/integer bits");
    }
  }
  auto multiple = row_fragments(d, false);
  auto failed = d.run("numeric.broadcast_strict", {multiple}, {},
                      {{"shape", std::string("2,2,3")},
                       {"axis_map", std::string("0,1")},
                       {"layout", std::string("view")}});
  require(!failed.ok() &&
              failed.status().reason == FailureReason::InvalidDomain &&
              failed.status().detail.scope == FailureScope::Run,
          "broadcast view requires one complete affine owner");
  original = {};
  scalar_window = {};
  source = {};
  scalar = {};
  d.context.reset();
  require(!scalar_weak.lock().valid() &&
              huge.association() == ResourceVector<uint64_t>{scalar_id} &&
              read_bits(broadcast, {2, 1, 3}) == 1 &&
              read_bits(huge, {1048575, 1048575}) == word,
          "scalar-copy releases oversized source while preserving association "
          "and borrowed views");
}
void array_association_ownership() {
  for (bool constant : {true, false}) {
    Driver d;
    std::weak_ptr<const CpuStorage> storage;
    std::vector<uint64_t> bits(constant ? 512 : 3, 0);
    if (constant)
      bits[125] = double_bits(7);
    else
      bits = {double_bits(1), double_bits(2), double_bits(3)};
    auto source =
        d.source({ElementType::Float64, {constant ? 1U : 3U}}, bits,
                 constant ? StridedLayout{1001, {8}} : StridedLayout{17, {-8}},
                 {}, {}, &storage);
    const auto source_weak = source.weak();
    const auto source_id = source.object_id();
    std::map<std::string, ParameterValue> parameters{
        {"shape", std::string(constant ? "8,8" : "4,3")},
        {"layout", std::string("view")}};
    if (!constant)
      parameters["axis_map"] = std::string("1");
    auto execution = take(
        d.run(constant ? "numeric.constant_strict" : "numeric.broadcast_strict",
              {source}, {}, parameters));
    auto output = execution.results.at("out");
    auto evidence = execution.dependencies;
    execution.results.clear();
    auto window = take(output.acquire_tensor(
        take(output.descriptor()), 0,
        Region::whole(output.schema().tensors[0].sample_shape())));
    source = {};
    d.context.reset();
    require(output.association() == ResourceVector<uint64_t>{source_id} &&
                read_bits(output, constant ? std::vector<uint64_t>{7, 7}
                                           : std::vector<uint64_t>{3, 2}) ==
                    double_bits(constant ? 7 : 1),
            "escaped array keeps exact source association and readable bytes");
    bool data = false, validation = false, descriptor = false;
    for (const auto& observation : take(evidence.source_observations())) {
      data |= (observation.roles & 1) != 0;
      validation |= (observation.roles & 4) != 0;
      descriptor |= (observation.roles & 8) != 0;
    }
    require(data && validation && descriptor,
            "escaped dependency bundle preserves data/validation/descriptor "
            "witnesses without payload");
    require(constant
                ? storage.expired() && !source_weak.lock().valid() &&
                      d.root.statistics().live[ResourceKind::Payload] == 8
                : !storage.expired() && source_weak.lock().valid() &&
                      d.root.statistics().live[ResourceKind::Payload] == 25,
            "constant releases oversized source while broadcast keeps its "
            "physical owner");
    output = {};
    require(!constant ? !storage.expired() : true,
            "last borrowed output window still pins broadcast backing");
    window = {};
    require(storage.expired() && !source_weak.lock().valid() &&
                d.root.statistics().live[ResourceKind::Payload] == 0,
            "last result/window releases payload despite escaped dependency "
            "evidence");
  }
}
void indexing_workflows() {
  Driver d;
  auto base = d.source({ElementType::Int64, {3}}, {10, 20, 30}, {1, {8}});
  auto indices = d.source({ElementType::Int64, {3}}, {2, 0, 2}, {17, {-8}});
  auto updates = d.source({ElementType::Int64, {3}}, {3, 2, 1}, {17, {-8}});
  const uint64_t expected[][3] = {{30, 10, 30},
                                  {2, 20, 3},
                                  {12, 20, 34},
                                  {2, 20, 1},
                                  {10, 20, 30}};
  const char* names[] = {"gather", "scatter_replace", "scatter_sum",
                         "scatter_minimum", "scatter_maximum"};
  for (const auto* suffix :
       {"_strict", "_accelerated_apple_silicon", "_accelerated_x86_64"}) {
    OperationMetadata input_metadata, index_metadata;
    input_metadata.result_schema =
        std::make_shared<SchemaTemplate>(base.schema());
    index_metadata.result_schema =
        std::make_shared<SchemaTemplate>(indices.schema());
    auto available = d.registry->resolve_traits(
        std::string("array.gather") + suffix, {input_metadata, index_metadata},
        {{"axis", static_cast<int64_t>(0)}});
    if (!available.ok()) {
      require(std::string(suffix) != "_strict" &&
                  available.status().code == ErrorCode::BackendUnavailable,
              "indexing incompatible profile reports BackendUnavailable");
      continue;
    }
    for (unsigned kind = 0; kind < 5; ++kind) {
      const auto key = std::string("array.") + names[kind] + suffix;
      auto inputs = std::vector<ResultRef>{base, indices};
      if (kind)
        inputs.push_back(updates);
      auto result =
          take(d.run(key, inputs, {}, {{"axis", static_cast<int64_t>(0)}}));
      for (uint64_t at = 0; at < 3; ++at)
        require(read_bits(result.results.at("out"), {at}) == expected[kind][at],
                "gather/scatter preserve index order and stable contributors");
      for (unsigned port = 0; port < inputs.size(); ++port) {
        auto edit = take(Footprint::from_regions({3}, {Region({{1, 1}})}));
        require(take(result.dependencies.potential_dirty(
                         "input" + std::to_string(port), edit, 1))
                        .at("out") == take(Footprint::all({3})),
                "every active indexing input has Whole source support");
      }
    }
  }
  auto matrix = d.source({ElementType::UInt8, {2, 3}}, {10, 11, 12, 20, 21, 22},
                         {1, {3, 1}});
  auto gathered = take(d.run("array.gather_strict", {matrix, indices}, {},
                             {{"axis", static_cast<int64_t>(1)}}))
                      .results.at("out");
  const uint64_t expected_matrix[] = {12, 10, 12, 22, 20, 22};
  for (uint64_t at = 0; at < 6; ++at)
    require(read_bits(gathered, {at / 3, at % 3}) == expected_matrix[at],
            "gather copies complete nonleading-axis slices");
  auto zero_indices = d.source({ElementType::Int64, {2}}, {0}, {1, {0}});
  auto one = d.source({ElementType::Float64, {1}},
                      {double_bits(std::ldexp(1.0, 100))}, {1, {0}});
  auto cancellation =
      d.source({ElementType::Float64, {2}},
               {double_bits(-std::ldexp(1.0, 100)), double_bits(3)}, {1, {8}});
  auto exact =
      take(d.run("array.scatter_sum_strict", {one, zero_indices, cancellation},
                 {}, {{"axis", static_cast<int64_t>(0)}}))
          .results.at("out");
  require(read_bits(exact, {0}) == double_bits(3),
          "scatter sum retains exact residual through wide cancellation");
  auto nan_base = d.source(
      {ElementType::Float64, {2}},
      {UINT64_C(0xfff0000000000003), UINT64_C(0x7ff0000000000011)}, {1, {8}});
  auto nan_updates = d.source(
      {ElementType::Float64, {2}},
      {UINT64_C(0x7ff0000000000021), UINT64_C(0xfff0000000000007)}, {1, {8}});
  auto copied = take(d.run("array.scatter_replace_strict",
                           {nan_base, zero_indices, nan_updates}, {},
                           {{"axis", static_cast<int64_t>(0)}}))
                    .results.at("out");
  require(read_bits(copied, {0}) == UINT64_C(0xfff0000000000007) &&
              read_bits(copied, {1}) == UINT64_C(0x7ff0000000000011),
          "replace and no-hit scatter preserve raw signaling NaN bits");
  for (const auto* name : {"sum", "minimum", "maximum"}) {
    copied = take(d.run(std::string("array.scatter_") + name + "_strict",
                        {nan_base, zero_indices, nan_updates}, {},
                        {{"axis", static_cast<int64_t>(0)}}))
                 .results.at("out");
    require(read_bits(copied, {0}) == UINT64_C(0xfff8000000000003) &&
                read_bits(copied, {1}) == UINT64_C(0x7ff0000000000011),
            "aggregate hits quiet base-first NaN while no-hit remains raw");
  }
  auto largest = d.source({ElementType::Int64, {1}}, {INT64_MAX}, {1, {0}});
  auto offsets = d.source({ElementType::Int64, {2}}, {1, UINT64_MAX}, {1, {8}});
  copied =
      take(d.run("array.scatter_sum_strict", {largest, zero_indices, offsets},
                 {}, {{"axis", static_cast<int64_t>(0)}}))
          .results.at("out");
  require(read_bits(copied, {0}) == INT64_MAX,
          "integer scatter checks final exact sum rather than intermediate "
          "overflow");
}
void concatenate_views() {
  Driver d;
  auto buffer = take(d.root.allocator().allocate(65));
  for (uint64_t index = 0; index < 8; ++index) {
    const auto bits = double_bits(index + 1);
    std::memcpy(buffer.data() + 1 + index * 8, &bits, 8);
  }
  auto common = std::move(buffer).freeze();
  std::weak_ptr<const CpuStorage> weak = common;
  const auto source = [&](std::vector<uint64_t> shape, uint64_t offset,
                          std::vector<int64_t> strides, bool fragmented) {
    SchemaTemplate schema;
    schema.id = "test.concatenate";
    ResultTensorSpec member;
    member.key = "samples";
    member.descriptor = {ElementType::Float64, shape};
    schema.tensors.push_back(member);
    auto builder =
        take(ResultBuilder::start(d.root, schema, "concatenate.source"));
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(d.root, 1, {0, 8, 0, 0})))
                .ok(),
            "concatenate source descriptor");
    auto relation = take(
        ResultRelation::cartesian(d.root, shape[0] * shape[1], {0, 1, 0, 0}));
    if (fragmented) {
      for (uint64_t row = 0; row < shape[0]; ++row)
        require(builder
                    .publish_tensor(
                        0, Region({{row, 1}, {0, shape[1]}}),
                        {offset + row * 16, {INT64_MIN, strides[1]}, {row, 0}},
                        common, relation, {true, true, true, true})
                    .ok(),
                "concatenate singleton row fragment");
    } else {
      require(builder
                  .publish_tensor(0, Region::whole(shape), {offset, strides},
                                  common, relation, {true, true, true, true})
                  .ok(),
              "concatenate affine source");
    }
    return take(builder.seal());
  };
  auto first = source({2, 2}, 1, {16, 8}, true);
  auto second = source({2, 2}, 33, {16, 8}, false);
  auto original = take(
      first.acquire_tensor(take(first.descriptor()), 0, Region::whole({2, 2})));
  const auto pointer = take(original.row_run({0, 0})).data;
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  ResultRef joined;
  for (const auto* suffix :
       {"_strict", "_accelerated_apple_silicon", "_accelerated_x86_64"}) {
    OperationMetadata metadata;
    metadata.result_schema = std::make_shared<SchemaTemplate>(first.schema());
    const auto key = std::string("array.concatenate") + suffix;
    auto available = d.registry->resolve_traits(
        key, {metadata, metadata},
        {{"axis", static_cast<int64_t>(0)}, {"layout", std::string("view")}});
    if (!available.ok()) {
      require(std::string(suffix) != "_strict" &&
                  available.status().code == ErrorCode::BackendUnavailable,
              "concatenate profile availability");
      continue;
    }
    auto result = take(d.run(
        key, {first, second}, {},
        {{"axis", static_cast<int64_t>(0)}, {"layout", std::string("view")}}));
    joined = result.results.at("out");
    auto window = take(joined.acquire_tensor(take(joined.descriptor()), 0,
                                             Region::whole({4, 2})));
    require(window.storage_owner_token() == common.get() &&
                take(window.row_run({0, 0})).data == pointer &&
                d.root.statistics().live[ResourceKind::Payload] == payload,
            "concatenate View joins complete singleton fragments without "
            "payload copying");
    for (uint64_t at = 0; at < 8; ++at)
      require(read_bits(joined, {at / 2, at % 2}) == double_bits(at + 1),
              "concatenate prefix mapping preserves all values");
    auto edit =
        take(Footprint::from_regions({2, 2}, {Region({{1, 1}, {1, 1}})}));
    require(take(result.dependencies.potential_dirty("input1", edit, 1))
                    .at("out") == take(Footprint::all({4, 2})),
            "concatenate View still retains Whole source support");
  }
  auto singleton_first = source({1, 2}, 1, {INT64_MIN, 8}, false);
  auto singleton_second = source({1, 2}, 17, {INT64_MIN, 8}, false);
  auto inferred = take(d.run("array.concatenate_strict",
                             {singleton_first, singleton_second}, {},
                             {{"axis", static_cast<int64_t>(0)},
                              {"layout", std::string("view")}}))
                      .results.at("out");
  require(read_bits(inferred, {1, 1}) == double_bits(4),
          "all singleton concat axes infer stride between port anchors");
  auto negative_first = source({2, 2}, 57, {-16, -8}, false);
  auto negative_second = source({2, 2}, 25, {-16, -8}, false);
  auto reverse = take(d.run("array.concatenate_strict",
                            {negative_first, negative_second}, {},
                            {{"axis", static_cast<int64_t>(0)},
                             {"layout", std::string("view")}}))
                     .results.at("out");
  require(read_bits(reverse, {0, 0}) == double_bits(8) &&
              read_bits(reverse, {3, 1}) == double_bits(1),
          "concatenate View preserves negative global strides");
  auto reordered = d.run(
      "array.concatenate_strict", {second, first},
      take(Footprint::from_regions({4, 2}, {Region({{0, 1}, {0, 1}})})),
      {{"axis", static_cast<int64_t>(0)}, {"layout", std::string("view")}});
  require(!reordered.ok() &&
              reordered.status().code == ErrorCode::InvalidArgument &&
              reordered.status().reason == FailureReason::InvalidDomain &&
              reordered.status().detail.scope == FailureScope::Run,
          "partial Q cannot relax global contiguous concat View proof");
  auto dense = take(d.run("array.concatenate_strict", {second, first}, {},
                          {{"axis", static_cast<int64_t>(0)},
                           {"layout", std::string("dense")}}))
                   .results.at("out");
  require(read_bits(dense, {0, 0}) == double_bits(5) &&
              read_bits(dense, {3, 1}) == double_bits(4),
          "Dense explicitly copies ordered unrelated-to-prefix inputs");
  auto unrelated = d.source({ElementType::Float64, {2, 2}}, {0}, {1, {0, 0}});
  auto failed = d.run(
      "array.concatenate_strict", {first, unrelated}, {},
      {{"axis", static_cast<int64_t>(0)}, {"layout", std::string("view")}});
  require(!failed.ok() && failed.status().message.find("ViewUnavailable") !=
                              std::string::npos,
          "concatenate View rejects independent physical owners");
  auto empty = take(d.run(
      "array.concatenate_strict", {first, unrelated},
      take(Footprint::none({4, 2})),
      {{"axis", static_cast<int64_t>(0)}, {"layout", std::string("view")}}));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty concat does not inspect physical viewability");
  original = {};
  first = {};
  second = {};
  singleton_first = {};
  singleton_second = {};
  inferred = {};
  negative_first = {};
  negative_second = {};
  reverse = {};
  dense = {};
  common.reset();
  d.context.reset();
  require(!weak.expired() && read_bits(joined, {3, 1}) == double_bits(8),
          "concat output owns source schema and storage after input/context "
          "retirement");
  joined = {};
  require(weak.expired(), "last concat view releases its source backing");
}
void concatenate_many_ports() {
  for (uint64_t ports : {65, 256}) {
    Driver d;
    auto scalar =
        d.source({ElementType::Float64, {1}}, {double_bits(7)}, {1, {0}});
    std::vector<ResultRef> inputs(ports, scalar);
    for (const auto* layout : {"dense", "view"}) {
      const auto payload = d.root.statistics().live[ResourceKind::Payload];
      auto result = take(d.run("array.concatenate_strict", inputs, {},
                               {{"axis", static_cast<int64_t>(0)},
                                {"layout", std::string(layout)}}));
      require(read_bits(result.results.at("out"), {0}) == double_bits(7) &&
                  read_bits(result.results.at("out"), {ports - 1}) ==
                      double_bits(7),
              "concat preserves every legal repeated port across bounded Need "
              "envelopes");
      if (std::string(layout) == "view") {
        auto source = take(scalar.acquire_tensor(take(scalar.descriptor()), 0,
                                                 Region::whole({1})));
        auto output = take(result.results.at("out").acquire_tensor(
            take(result.results.at("out").descriptor()), 0,
            Region::whole({ports})));
        require(
            source.storage_owner_token() == output.storage_owner_token() &&
                take(source.row_run({0})).data ==
                    take(output.row_run({ports - 1})).data &&
                d.root.statistics().live[ResourceKind::Payload] == payload,
            "many-port constant affine concatenation remains a zero-copy view");
      }
    }
  }
}
void indexing_boundaries() {
  Driver d;
  auto base = d.source({ElementType::Int64, {3}}, {10, 20, 30}, {1, {8}});
  auto invalid = d.source({ElementType::Int64, {2}}, {0, 3}, {1, {8}});
  auto failed = d.run("array.gather_strict", {base, invalid},
                      take(Footprint::from_regions({2}, {Region({{0, 1}})})),
                      {{"axis", static_cast<int64_t>(0)}});
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              failed.status().reason == FailureReason::InvalidDomain &&
              failed.status().detail.origin == FailureOrigin::Domain &&
              failed.status().detail.scope == FailureScope::Run &&
              !failed.status().detail.atom &&
              failed.status().message.find("position=1 index=3 extent=3") !=
                  std::string::npos,
          "an index outside the selected output fails the complete run");
  auto empty = take(d.run("array.gather_strict", {base, invalid},
                          take(Footprint::none({2})),
                          {{"axis", static_cast<int64_t>(0)}}));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty gather skips invalid index payload");
  auto targets = d.source({ElementType::Int64, {1}}, {1}, {1, {0}});
  auto overflows =
      d.source({ElementType::Int64, {2}}, {0, INT64_MAX}, {1, {8}});
  auto update = d.source({ElementType::Int64, {1}}, {1}, {1, {0}});
  const auto live = d.root.statistics().live[ResourceKind::Payload];
  failed = d.run("array.scatter_sum_strict", {overflows, targets, update},
                 take(Footprint::from_regions({2}, {Region({{0, 1}})})),
                 {{"axis", static_cast<int64_t>(0)}});
  require(!failed.ok() && failed.status().code == ErrorCode::OperationFailed &&
              failed.status().reason == FailureReason::ArithmeticOverflow &&
              failed.status().detail.scope == FailureScope::Run &&
              failed.status().detail.origin == FailureOrigin::Domain &&
              failed.status().message.find("output=[1,") != std::string::npos &&
              d.root.statistics().live[ResourceKind::Payload] == live,
          "late exact scatter overflow outside Q rolls back output with Run "
          "provenance");
  auto facet = take(encode_semantic(coverage_semantics()));
  auto typed = d.source({ElementType::Float32, {2, 2}}, {0, 0, 0, 0},
                        {1, {8, 4}}, {facet});
  auto bad_updates = d.source({ElementType::Float32, {2, 2}},
                              {0, 0, 0, 0x40000000}, {1, {8, 4}}, {facet});
  auto repeated = d.source({ElementType::Int64, {2}}, {0}, {1, {0}});
  failed = d.run("array.scatter_replace_strict", {typed, repeated, bad_updates},
                 {}, {{"axis", static_cast<int64_t>(1)}});
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              failed.status().detail.origin == FailureOrigin::Domain &&
              failed.status().detail.input_id == 13,
          "overwritten typed updates still undergo complete Whole validation");
  CancellationSource stop;
  stop.cancel();
  failed = d.run("array.gather_strict", {base, invalid}, {},
                 {{"axis", static_cast<int64_t>(0)}}, stop.token());
  require(!failed.ok() && failed.status().code == ErrorCode::Cancelled,
          "indexing pre-cancellation preserves cancellation before bad index");
  Driver limited(3000);
  auto large = limited.source({ElementType::Int64, {64}}, {1}, {1, {0}});
  auto many = limited.source({ElementType::Int64, {64}}, {0}, {1, {0}});
  auto payload = limited.root.statistics().live[ResourceKind::Payload];
  failed = limited.run("array.scatter_sum_strict", {large, many, large}, {},
                       {{"axis", static_cast<int64_t>(0)}});
  require(!failed.ok() &&
              failed.status().code == ErrorCode::ResourceExhausted &&
              failed.status().reason == FailureReason::WorkLimit &&
              limited.root.statistics().live[ResourceKind::Payload] == payload,
          "radix/aggregate work failure releases indexing scratch and output");
}
void joined_resource_failure() {
  ResourceBudget root;
  OcioConfigSnapshot snapshot;
  snapshot.config = {'t'};
  snapshot.spaces = {{"linear", "scene"}};
  snapshot.build_identity = "test-pinned";
  snapshot.settings = "reference";
  auto config = take(OcioConfigResource::import(snapshot, root));
  auto resources = take(ResourceBindings::create({}, {config}, root));
  auto source =
      take(Value::create({ElementType::Float64, {2, 2}}, Region::whole({2, 2}),
                         {0, {16, 8}}, std::vector<uint8_t>(32)));
  std::vector<Value> parts{take(source.view(Region({{0, 1}, {0, 2}}))),
                           take(source.view(Region({{1, 1}, {0, 2}})))};
  SemanticDescriptor semantics;
  semantics.channels = {{"value", "value", "dimensionless"}};
  const auto facet = take(encode_semantic(semantics));
  auto scratch = take(root.reserve(ResourceCapacity::host(4096, 4096)));
  require(root.consume({UINT64_MAX - root.statistics().issued.work}).ok(),
          "exhaust retained resource root");
  const auto live = root.statistics().live.values;
  auto joined = input_internal::join_affine_view(
      source.descriptor(), source.region(), parts, FootprintLimits{}, {facet},
      resources);
  require(!joined.ok() &&
              joined.status().code == ErrorCode::ResourceExhausted &&
              joined.status().reason == FailureReason::WorkLimit &&
              root.statistics().live.values == live,
          "affine join preserves resource selection failure instead of view "
          "fallback");
}
void paged_layout_copy() {
  for (const auto* layout : {"auto", "dense"}) {
    Driver d;
    SchemaTemplate schema;
    schema.id = "test.paged.numeric";
    ResultTensorSpec slot;
    slot.key = "samples";
    slot.descriptor = {ElementType::Float64, {1, 1}};
    slot.batch_axes = {128};
    slot.layout.spatial = true;
    slot.layout.channel_axis.reset();
    schema.tensors.push_back(slot);
    auto builder = take(ResultBuilder::start(d.root, schema, "paged.layouts"));
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(d.root, 1, {0, 8, 0, 0})))
                .ok(),
            "paged layout descriptor");
    double values[128];
    for (unsigned i = 0; i < 128; ++i)
      values[i] = static_cast<double>(i + 1);
    require(builder
                .publish_tensor(
                    0, Region::whole(slot.sample_shape()),
                    ByteView(reinterpret_cast<const uint8_t*>(values),
                             sizeof(values)),
                    take(ResultRelation::cartesian(d.root, 128, {0, 1, 0, 0})),
                    {true, true, true, true})
                .ok(),
            "paged layout input");
    auto source = take(builder.seal());
    const auto before = d.root.statistics().issued.work;
    auto output = take(d.run("array.reshape_strict", {source}, {},
                             {{"shape", std::string("16,8")},
                              {"layout", std::string(layout)}}))
                      .results.at("out");
    const auto work = d.root.statistics().issued.work - before;
    require(work < 20000,
            "paged Whole copy reuses one window instead of quadratic directory "
            "scans");
    for (uint64_t i = 0; i < 128; ++i)
      require(read_bits(output, {i / 8, i % 8}) == double_bits(i + 1),
              "paged Auto/Dense reshape preserves every batch value");
  }
}
struct BoundedLayoutPhase {
  ResultContinuation inner;
  unsigned samples = 0;
  explicit BoundedLayoutPhase(ResultContinuation continuation)
      : inner(std::move(continuation)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto bounded = phase;
    bounded.consume_work = [&](uint64_t amount) {
      auto status = phase.consume_work(amount);
      if (status.ok() && amount == 13 && ++samples == 2) {
        // The first sample has already acquired the full paged window. Leave
        // two units immediately before the second sample's owning row lookup.
        status = phase.resources.consume(
            {UINT64_MAX - phase.resources.statistics().issued.work - 2});
      }
      return status;
    };
    return inner.poll(bounded);
  }
};
void paged_layout_failure() {
  Driver d;
  const auto original_registry = d.registry;
  OperationDefinition definition;
  definition.key = "array.reshape_strict";
  definition.traits = take(original_registry->find_traits(definition.key));
  definition.traits.requires_metadata_specialization = false;
  auto& output_schema = *definition.traits.outputs[0].result_schema;
  output_schema.tensors[0].descriptor = {ElementType::Float64, {16, 8}};
  definition.traits.outputs[0].continuation_bytes += sizeof(BoundedLayoutPhase);
  definition.start_result = [original_registry](const auto& query,
                                                const auto& allocator) {
    auto forwarded = query;
    forwarded.prepared.reset();
    auto inner = original_registry->start_result("array.reshape_strict",
                                                 forwarded, allocator);
    if (!inner.ok())
      return inner;
    return ResultContinuation::make<BoundedLayoutPhase>(allocator,
                                                        inner.take_value());
  };
  auto registry = std::make_shared<OperationRegistry>();
  require(registry->register_operation(std::move(definition)).ok(),
          "bounded layout registration");
  require(registry->freeze().ok(), "bounded layout registry freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  d.registry = registry;
  d.context = std::make_unique<ExecutionContext>(registry, config);
  d.root = take(d.context->resource_budget());
  SchemaTemplate schema;
  schema.id = "test.paged.failure";
  ResultTensorSpec slot;
  slot.key = "samples";
  slot.descriptor = {ElementType::Float64, {1, 1}};
  slot.batch_axes = {128};
  slot.layout.spatial = true;
  slot.layout.channel_axis.reset();
  schema.tensors.push_back(slot);
  auto builder = take(ResultBuilder::start(d.root, schema, "paged.failure"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(d.root, 1, {0, 8, 0, 0})))
              .ok(),
          "paged failure descriptor");
  double values[128]{};
  require(builder
              .publish_tensor(
                  0, Region::whole(slot.sample_shape()),
                  ByteView(reinterpret_cast<const uint8_t*>(values),
                           sizeof(values)),
                  take(ResultRelation::cartesian(d.root, 128, {0, 1, 0, 0})),
                  {true, true, true, true})
              .ok(),
          "paged failure source");
  auto source = take(builder.seal());
  const auto live = d.root.statistics().live[ResourceKind::Payload];
  auto failed =
      d.run("array.reshape_strict", {source}, {},
            {{"shape", std::string("16,8")}, {"layout", std::string("dense")}});
  require(!failed.ok() &&
              failed.status().code == ErrorCode::ResourceExhausted &&
              failed.status().reason == FailureReason::WorkLimit &&
              failed.status().detail.scope == FailureScope::Group &&
              !failed.status().detail.atom &&
              d.root.statistics().live[ResourceKind::Payload] == live,
          "owning paged row failure preserves complete cause and rolls back "
          "dense output");
}
void indexing_upstream() {
  Driver d;
  auto registry = make_default_operation_registry(false);
  auto starts = std::make_shared<unsigned>(0);
  OperationDefinition producer;
  producer.key = "test.index.updates";
  auto& output = producer.traits.outputs[0];
  output.output_schema.kind = OperationPortKind::Result;
  output.output_schema.result_schema_id = "photospider.tensor";
  output.output_schema.result_schema_version = 1;
  SchemaTemplate schema;
  schema.id = "photospider.tensor";
  ResultTensorSpec member;
  member.key = "samples";
  member.descriptor = {ElementType::Float64, {2}};
  schema.tensors.push_back(member);
  output.result_schema = schema;
  output.continuation_bytes = sizeof(FailingProducer);
  output.maximum_dependency_stages = 1;
  producer.start_result = [starts](const auto&, const auto& allocator) {
    ++*starts;
    return ResultContinuation::make<FailingProducer>(allocator);
  };
  require(registry->register_operation(std::move(producer)).ok(),
          "indexing producer registration");
  require(registry->freeze().ok(), "indexing producer registry freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  d.registry = registry;
  d.context = std::make_unique<ExecutionContext>(registry, config);
  d.root = take(d.context->resource_budget());
  auto base = d.source({ElementType::Float64, {2}}, {double_bits(1)}, {1, {0}});
  auto indices = d.source({ElementType::Int64, {2}}, {0}, {1, {0}});
  auto prepared =
      d.prepare("array.scatter_replace_strict", {base, indices, base},
                {{"axis", static_cast<int64_t>(0)}});
  auto document = prepared.graph->snapshot().document();
  document.nodes.insert(document.nodes.begin(),
                        {3, "test.index.updates", {}, {}});
  document.nodes.back().inputs[2] = WorkflowNodeOutput{3, "value"};
  auto graph = std::make_shared<GraphContext>(document);
  auto compiled = take(Compiler(registry).compile(*graph));
  auto frozen = take(d.context->freeze(compiled.plan, prepared.bindings));
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  auto empty = take(d.context->execute_fragments(
      frozen, {{"out", take(Footprint::none({2}))}}));
  require(*starts == 0 &&
              d.root.statistics().live[ResourceKind::Payload] == payload,
          "Empty scatter does not start computed updates");
  auto point = take(Footprint::from_regions({2}, {Region({{1, 1}})}));
  auto failed = d.context->execute_fragments(frozen, {{"out", point}});
  require(!failed.ok() && failed.status().code == ErrorCode::OperationFailed &&
              failed.status().message == "must stay lazy" && *starts == 1 &&
              failed.status().detail.node_id == 3 &&
              d.root.statistics().live[ResourceKind::Payload] == payload,
          "Whole scatter evaluates updates even when Q only selects a no-hit "
          "base sample");
  auto batched = d.source({ElementType::Int64, {2}}, {0}, {1, {0, 0}}, {}, {1});
  OperationMetadata base_metadata, index_metadata;
  base_metadata.result_schema = std::make_shared<SchemaTemplate>(base.schema());
  index_metadata.result_schema =
      std::make_shared<SchemaTemplate>(batched.schema());
  auto rejected = registry->resolve_traits("array.gather_strict",
                                           {base_metadata, index_metadata},
                                           {{"axis", static_cast<int64_t>(0)}});
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
          "index vector logical rank includes every batch axis");
}
void indexing_metadata_failure() {
  for (bool second_vector : {false, true}) {
    const uint64_t count = second_vector ? 512 : 64;
    Driver d;
    const auto original_registry = d.registry;
    OperationDefinition definition;
    definition.key = "array.scatter_sum_strict";
    definition.traits = take(original_registry->find_traits(definition.key));
    definition.traits.requires_metadata_specialization = false;
    auto& output = definition.traits.outputs[0];
    output.result_schema->tensors[0].descriptor = {ElementType::Int64, {count}};
    output.continuation_bytes += sizeof(BoundedMetadataPhase);
    definition.start_result = [original_registry, second_vector](
                                  const auto& query, const auto& allocator) {
      auto forwarded = query;
      forwarded.prepared.reset();
      auto inner = original_registry->start_result("array.scatter_sum_strict",
                                                   forwarded, allocator);
      if (!inner.ok())
        return inner;
      return ResultContinuation::make<BoundedMetadataPhase>(
          allocator, inner.take_value(), second_vector ? 256 : 64,
          second_vector ? 8192 : 1024, second_vector ? 2 : 1);
    };
    auto registry = std::make_shared<OperationRegistry>();
    require(registry->register_operation(std::move(definition)).ok(),
            "bounded indexing registration");
    require(registry->freeze().ok(), "bounded indexing registry freeze");
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Metadata] = 1000000;
    d.registry = registry;
    d.context = std::make_unique<ExecutionContext>(registry, config);
    d.root = take(d.context->resource_budget());
    auto source = d.source({ElementType::Int64, {count}}, {1}, {1, {0}});
    auto indices = d.source({ElementType::Int64, {count}}, {0}, {1, {0}});
    auto prepared =
        d.prepare("array.scatter_sum_strict", {source, indices, source},
                  {{"axis", static_cast<int64_t>(0)}});
    auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
    auto demand = take(Footprint::all({count}));
    auto warm = take(d.context->execute_fragments(
        frozen, {{"out", take(Footprint::none({count}))}}));
    warm = {};
    const auto payload = d.root.statistics().live[ResourceKind::Payload];
    const auto metadata = d.root.statistics().live[ResourceKind::Metadata];
    auto failed = d.context->execute_fragments(frozen, {{"out", demand}});
    require(!failed.ok() &&
                failed.status().code == ErrorCode::ResourceExhausted &&
                failed.status().reason == FailureReason::CapacityLimit &&
                d.root.statistics().live[ResourceKind::Payload] == payload &&
                d.root.statistics().live[ResourceKind::Metadata] == metadata,
            "index-plan metadata rejection retires readers, radix state and "
            "output");
  }
}
void singleton_slice_controls() {
  Driver d;
  auto registry = make_default_operation_registry(false);
  auto starts = std::make_shared<unsigned>(0);
  OperationDefinition producer;
  producer.key = "test.steps";
  auto& out = producer.traits.outputs[0];
  out.output_schema.kind = OperationPortKind::Result;
  out.output_schema.result_schema_id = "photospider.tensor";
  out.output_schema.result_schema_version = 1;
  SchemaTemplate schema;
  schema.id = "photospider.tensor";
  ResultTensorSpec spec;
  spec.key = "samples";
  spec.descriptor = {ElementType::Int64, {2}};
  schema.tensors.push_back(spec);
  out.result_schema = schema;
  out.continuation_bytes = sizeof(FailingProducer);
  out.maximum_dependency_stages = 1;
  producer.start_result = [starts](const auto&, const auto& allocator) {
    ++*starts;
    return ResultContinuation::make<FailingProducer>(allocator);
  };
  require(registry->register_operation(std::move(producer)).ok(),
          "step producer registration");
  require(registry->freeze().ok(), "step registry freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  d.registry = registry;
  d.context = std::make_unique<ExecutionContext>(registry, config);
  d.root = take(d.context->resource_budget());
  auto source = d.source({ElementType::Float64, {2, 3}},
                         {double_bits(1), double_bits(2), double_bits(3),
                          double_bits(4), double_bits(5), double_bits(6)},
                         {1, {24, 8}});
  auto starts_input = d.source({ElementType::Int64, {2}}, {1, 2}, {1, {8}});
  auto prepared = d.prepare(
      "array.slice_strict", {source, starts_input, starts_input},
      {{"counts", std::string("1,1")}, {"layout", std::string("view")}});
  auto document = prepared.graph->snapshot().document();
  document.nodes.insert(document.nodes.begin(), {3, "test.steps", {}, {}});
  document.nodes.back().inputs[2] = WorkflowNodeOutput{3, "value"};
  auto graph = std::make_shared<GraphContext>(document);
  auto compiled = take(Compiler(registry).compile(*graph));
  auto copied = take(d.context->execute_fragments(
      take(d.context->freeze(compiled.plan, prepared.bindings)),
      {{"out", take(Footprint::all({1, 1}))}}));
  require(*starts == 0 &&
              read_bits(copied.results.at("out"), {0, 0}) == double_bits(6),
          "all singleton slice leaves the step producer unstarted");
  document.nodes.back().parameters["counts"] = std::string("1,2");
  graph = std::make_shared<GraphContext>(document);
  compiled = take(Compiler(registry).compile(*graph));
  auto failed = d.context->execute_fragments(
      take(d.context->freeze(compiled.plan, prepared.bindings)),
      {{"out", take(Footprint::all({1, 2}))}});
  require(!failed.ok() && failed.status().code == ErrorCode::OperationFailed &&
              *starts == 1,
          "mixed slice counts retain the whole step producer obligation");
}
}  // namespace
int main() {
  try {
    layout_views();
    layout_profiles();
    constant_and_broadcast();
    array_association_ownership();
    indexing_workflows();
    concatenate_views();
    concatenate_many_ports();
    indexing_boundaries();
    indexing_metadata_failure();
    indexing_upstream();
    paged_layout_copy();
    paged_layout_failure();
    joined_resource_failure();
    singleton_slice_controls();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
