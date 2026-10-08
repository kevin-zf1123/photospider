#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "execution/result_native.hpp"
#include "photospider/data/color_array.hpp"
#include "plugin/dependency_discovery.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
void require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
void word(std::uint8_t* bytes, std::uint64_t value, unsigned width = 4) {
  for (unsigned i = 0; i < width; ++i)
    bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
}
ResultProgramMetadata metadata(unsigned mode) {
  SchemaTemplate schema;
  schema.id = "test.discovery";
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ElementType::Float32, {16}};
  if (mode == 20 || mode == 21) {
    tensor.batch_axes = {2, 2};
    tensor.layout.spatial = true;
    tensor.layout.channel_axis = 2;
    tensor.descriptor.shape = {1, 1, 3};
    tensor.facets = {encode_color_array(ColorArrayDescriptor{}).take_value()};
  }
  if (mode == 22)
    tensor.descriptor.shape = std::vector<std::uint64_t>(8, UINT64_C(1) << 40);
  schema.tensors.push_back(tensor);
  if (mode == 23) {
    tensor.key = "second";
    schema.tensors.push_back(tensor);
  }
  require(schema.validate().ok(), "valid discovery Result schema rejected");
  ResultProgramMetadata result;
  OperationMetadata input;
  input.result_schema =
      std::make_shared<const SchemaTemplate>(std::move(schema));
  result.inputs.push_back(std::move(input));
  return result;
}
std::shared_ptr<const CpuStorage> table(const ResourceBudget& root,
                                        const ResultProgramMetadata& metadata,
                                        unsigned mode) {
  auto made = root.allocator().allocate(16 + 2 * 144).take_value();
  std::memset(made.data(), 0, made.size());
  word(made.data(), mode == 11 ? 0 : 2);
  const auto shape =
      metadata.inputs[0].result_schema->tensors[0].sample_shape();
  for (unsigned i = 0; i < 2; ++i) {
    auto* row = made.data() + 16 + i * 144;
    word(row + 4, mode == 10 && i ? 4 : 1);
    word(row + 8, shape.size());
    word(row + 12, mode == 23 ? 1 : 0);
    for (unsigned axis = 0; axis < shape.size(); ++axis) {
      word(row + 16 + axis * 8,
           mode == 22   ? (i ? UINT64_C(1) << 39 : 0)
           : mode == 20 ? (axis == 4 ? i : 0)
           : mode == 21 ? (axis == 0 ? i : 0)
                        : (i ? 9 : 1),
           8);
      word(row + 80 + axis * 8,
           mode == 20 && axis == 4   ? (i ? 2 : 1)
           : mode == 21 && axis == 4 ? 3
                                     : 1,
           8);
    }
  }
  auto* first = made.data() + 16;
  if (mode == 1) {
    word(made.data(), 3);
    word(made.data() + 4, 1);
  }
  if (mode == 2)
    word(made.data(), 3);
  if (mode == 3)
    word(first + 24, 1, 8);
  if (mode == 4)
    word(first + 8, 0);
  if (mode == 5)
    word(first + 4, 8);
  if (mode == 6)
    word(first + 80, 0, 8);
  if (mode == 7)
    word(first + 16, 16, 8);
  if (mode == 8)
    word(made.data() + 8, 1);
  if (mode == 9)
    word(first + 12, 1);
  if (mode == 12)
    word(made.data(), 4);
  if (mode == 13)
    word(first, 9);
  if (mode == 14)
    word(made.data() + 4, 2);
  return std::move(made).freeze();
}
void decode(unsigned mode, ErrorCode expected, std::uint64_t work = 1048576,
            std::uint64_t boxes = 65536) {
  ResourceBudget root;
  ResourceAllocationScope scope(root);
  auto facts = metadata(mode);
  const std::map<std::string, ParameterValue> parameters;
  ResultProgramQuery query(facts, parameters);
  auto storage = table(root, facts, mode);
  FootprintLimits limits;
  limits.maximum_boxes = boxes;
  limits.consume_work = [&](std::uint64_t units) {
    if (units > work)
      return Status{ErrorCode::ResourceExhausted, {}};
    work -= units;
    return root.consume({units});
  };
  CancellationSource cancelled;
  if (mode == 15) {
    cancelled.cancel();
    limits.cancellation = cancelled.token();
  }
  std::uint64_t entries = 1;
  auto decoded = plugin_internal::decode_result_discovery(
      *storage, 2, 3, query, root, limits, &entries);
  if (decoded.status().code != expected)
    std::cerr << "mode=" << mode << " " << decoded.status().message << '\n';
  require(decoded.status().code == expected, "Result discovery decoder status");
  if (!decoded.ok())
    return;
  storage.reset();
  require(root.statistics().live[ResourceKind::Payload] == 0,
          "discovery metadata retained table payload");
  if (mode == 11) {
    require(decoded.value().empty(), "empty discovery gained a Need");
    return;
  }
  require(decoded.value().size() == (mode == 10 ? 2 : 1),
          "discovery role groups");
  const auto& need = decoded.value().front();
  require(need.slot == (mode == 23 ? 1 : 0), "discovery tensor slot");
  if (mode == 22) {
    require(
        need.samples.contains(std::vector<std::uint64_t>(8, UINT64_C(1) << 39)),
        "rank-8 64-bit discovery address truncated");
  } else if (mode == 21) {
    require(need.samples.element_count().value() == 6 &&
                need.samples.shape().size() == 5,
            "Result batch/channel discovery closure");
  } else if (mode == 10) {
    require(need.roles == 1 && need.samples.contains({1}) &&
                !need.samples.contains({9}) && decoded.value()[1].roles == 4 &&
                decoded.value()[1].samples.contains({9}),
            "discovery merged distinct role coverage");
  } else {
    require(need.samples.element_count().value() == 2 &&
                need.samples.contains({1}) && need.samples.contains({9}),
            "discovery changed sparse request");
  }
}
void admission(bool disabled) {
  ResourceBudget root;
  ResourceAllocationScope allocation_scope(root);
  const auto allocator = root.allocator();
  const execution_internal::ResultNativeScope::Upload upload =
      [](const Value&,
         const execution_internal::ResultNativeScope::CacheWork&) {
        return Result<std::pair<Value, std::uint64_t>>(
            Status{ErrorCode::BackendUnavailable, {}});
      };
  execution_internal::ResultNativeScope scope(allocator, upload, root, {},
                                              disabled ? 0 : 65536);
  auto facts = metadata(0);
  const std::map<std::string, ParameterValue> parameters;
  ResultProgramQuery query(facts, parameters);
  query.backend = Backend::Gpu;
  bool entered = false;
  std::uint64_t remaining = 64;
  auto result = execution_internal::ResultNativeScope::discover(
      query, 2, 3,
      [&](const auto&) {
        entered = true;
        return Status::success();
      },
      [&](std::uint64_t units) {
        if (units > remaining)
          return Status{ErrorCode::ResourceExhausted, {}};
        remaining -= units;
        return root.consume({units});
      });
  require(result.status().code == ErrorCode::ResourceExhausted && !entered &&
              root.statistics().peak[ResourceKind::Payload] == 0,
          "discovery admission allocated before work or disable check");
}
}  // namespace
int main() try {
  decode(0, ErrorCode::Ok);
  decode(1, ErrorCode::ResourceExhausted);
  for (unsigned mode : {2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 20})
    decode(mode, ErrorCode::InvalidArgument);
  for (unsigned mode : {10, 11, 21, 22, 23})
    decode(mode, ErrorCode::Ok);
  decode(15, ErrorCode::Cancelled);
  decode(10, ErrorCode::ResourceExhausted, 1048576, 4);
  decode(0, ErrorCode::ResourceExhausted, 1);
  admission(false);
  admission(true);
  std::cout << "Result discovery: slots, roles, batch/channel closure, rank-8, "
               "budgets, cancellation and owner retirement passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
