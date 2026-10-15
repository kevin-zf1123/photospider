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
  tensor.descriptor = {ElementType::Float32, {1, 16}};
  tensor.layout.spatial = true;
  tensor.layout.channel_axis.reset();
  tensor.layout.height_axis = 0;
  tensor.layout.width_axis = 1;
  if (mode == 21) {
    tensor.batch_axes = {2, 2};
    tensor.descriptor.shape = {1, 2, 3};
    tensor.layout.channel_axis = 2;
    tensor.atomic_trailing_axes = 1;
  }
  if (mode == 22)
    tensor.descriptor.shape = {1, UINT64_C(1) << 40};
  if (mode == 23 || mode == 24)
    tensor.layout.spatial = false;
  schema.tensors.push_back(tensor);
  if (mode == 25 || mode == 26) {
    tensor.key = "second";
    schema.tensors.push_back(tensor);
  }
  const auto valid = schema.validate();
  if (!valid.ok())
    std::cerr << "schema mode=" << mode << " " << valid.message << '\n';
  require(valid.ok(), "valid discovery Result schema rejected");
  ResultProgramMetadata result;
  OperationMetadata input;
  input.result_schema =
      std::make_shared<const SchemaTemplate>(std::move(schema));
  result.inputs.push_back(input);
  if (mode == 10)
    result.inputs.push_back(std::move(input));
  return result;
}
std::shared_ptr<const CpuStorage> table(const ResourceBudget& root,
                                        unsigned mode) {
  auto made = root.allocator().allocate(48).take_value();
  std::memset(made.data(), 0, made.size());
  word(made.data(), mode == 11 ? 0 : 2);
  for (unsigned i = 0; i < 2; ++i) {
    auto* row = made.data() + 16 + i * 16;
    word(row, mode == 10 ? i : 0);
    const bool whole = mode == 24 || mode == 26 || (mode == 27 && !i);
    word(row + 4, whole ? 1 : mode == 28 ? 2 : 0);
    word(row + 8,
         whole || mode == 28 ? 0
         : mode == 21        ? (i ? 7 : 0)
         : mode == 22        ? (i ? UINT64_C(1) << 39 : 0)
         : mode == 29        ? 9
                             : (i ? 9 : 1),
         8);
  }
  auto* first = made.data() + 16;
  if (mode == 1) {
    word(made.data(), 3);
    word(made.data() + 4, 1);
  }
  if (mode == 2)
    word(made.data(), 3);
  if (mode == 3)
    word(first + 4, 3);
  if (mode == 4)
    word(first + 4, 1);  // Whole with nonzero index.
  if (mode == 5)
    word(first + 4, 2);  // Descriptor with nonzero index.
  if (mode == 6)
    word(first + 4, UINT32_MAX);
  if (mode == 7)
    word(first + 8, 16, 8);
  if (mode == 8)
    word(made.data() + 8, 1);
  if (mode == 9)
    word(first + 8, UINT64_MAX, 8);
  if (mode == 12)
    word(made.data(), 4);
  if (mode == 13)
    word(first, 9);
  if (mode == 14)
    word(made.data() + 4, 2);
  if (mode == 30) {
    word(first + 4, 1);
    word(first + 8, 0, 8);
    word(first + 24, 16, 8);  // Whole must not hide another bad tile.
  }
  return std::move(made).freeze();
}
void decode(unsigned mode, ErrorCode expected, std::uint64_t work = 1048576,
            std::uint64_t boxes = 65536) {
  ResourceBudget root;
  ResourceAllocationScope scope(root);
  auto facts = metadata(mode);
  const std::map<std::string, ParameterValue> parameters;
  ResultProgramQuery query(facts, parameters);
  query.tile_extent = 1;
  auto storage = table(root, mode);
  FootprintLimits limits;
  limits.maximum_boxes = boxes;
  CancellationSource cancelled;
  limits.cancellation = cancelled.token();
  unsigned charges = 0;
  limits.consume_work = [&](std::uint64_t units) {
    if (units > work)
      return Status{ErrorCode::ResourceExhausted, {}};
    work -= units;
    ++charges;
    if ((mode == 31 && units > 1) || (mode == 32 && charges == 2))
      cancelled.cancel();
    return root.consume({units});
  };
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
  if (!decoded.ok()) {
    require(entries == 1, "failed decode changed metadata counter");
    return;
  }
  storage.reset();
  require(root.statistics().live[ResourceKind::Payload] == 0,
          "discovery metadata retained table payload");
  if (mode == 11) {
    require(decoded.value().empty(), "empty discovery gained a Need");
    return;
  }
  require(decoded.value().size() == (mode == 10 ? 2 : 1),
          "discovery input groups");
  const auto& need = decoded.value().front();
  require(need.input == 0 && need.tiles.owned_by(root), "input/Root ownership");
  if (mode == 24 || mode == 26 || mode == 27) {
    require(need.whole && !need.descriptor_only && need.tiles.empty(),
            "Whole normalization");
  } else if (mode == 28) {
    require(!need.whole && need.descriptor_only && need.tiles.empty(),
            "descriptor normalization");
  } else {
    const auto first = mode == 21 || mode == 22 ? 0U : mode == 29 ? 9U : 1U;
    const TileIndex last = mode == 21 ? 7 : mode == 22 ? UINT64_C(1) << 39 : 9;
    require(
        !need.whole && !need.descriptor_only && *need.tiles.begin() == first,
        "discovery tile kind/index");
    if (mode == 10) {
      require(need.tiles.size() == 1 && decoded.value()[1].input == 1 &&
                  *decoded.value()[1].tiles.begin() == 9,
              "distinct input requests merged");
    } else {
      require(need.tiles.size() == (mode == 29 ? 1 : 2) &&
                  *(need.tiles.end() - 1) == last,
              "sparse, batched or 64-bit tile index changed");
    }
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
  for (unsigned mode : {2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 23, 25, 30})
    decode(mode, ErrorCode::InvalidArgument);
  for (unsigned mode : {10, 11, 21, 22, 24, 26, 27, 28, 29})
    decode(mode, ErrorCode::Ok);
  for (unsigned mode : {15, 31, 32})
    decode(mode, ErrorCode::Cancelled);
  decode(10, ErrorCode::ResourceExhausted, 1048576, 4);
  decode(0, ErrorCode::ResourceExhausted, 1);
  admission(false);
  admission(true);
  std::cout
      << "Result discovery: tiles, Whole, descriptor, batches, 64-bit indices, "
         "budgets, cancellation and owner retirement passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
