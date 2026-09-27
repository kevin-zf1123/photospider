#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
void require(bool condition, const std::string& reason) {
  if (!condition)
    throw std::runtime_error(reason);
}
template <class T>
T take(Result<T> result) {
  require(result.ok(), result.status().message);
  return result.take_value();
}
Value samples(std::vector<std::uint64_t> shape, const std::vector<float>& data,
              bool image = false) {
  auto buffer = take(MutableValue::allocate(
      {ElementType::Float32, shape}, Region::whole(shape), BufferAllocator{}));
  require(buffer.size() == data.size() * sizeof(float), "fixture shape");
  std::memcpy(buffer.data(), data.data(), buffer.size());
  std::vector<ValueFacet> facets;
  if (image) {
    auto rgb = rgba_semantics();
    rgb.channels.pop_back();
    rgb.association = "none";
    facets = {take(encode_semantic(rgb))};
  }
  return take(std::move(buffer).publish(facets));
}
float sample(const Value& value, const std::vector<std::uint64_t>& coordinate) {
  float number = 0;
  const auto address = take(value.byte_address(coordinate));
  std::memcpy(&number, value.bytes().data() + address, sizeof(number));
  return number;
}
struct Reads {
  std::mutex mutex;
  std::map<std::string, std::set<std::string>> regions;
};
std::string region_text(const Region& region) {
  std::ostringstream text;
  for (const auto& axis : region.dimensions())
    text << '[' << axis.offset << ',' << axis.offset + axis.extent << ')';
  return text.str();
}
// Only public APIs: source callbacks record actual transport, independently of
// the dependency certificates returned by execution.
ExecutionResult run(WorkflowDocument doc, const std::vector<Value>& inputs,
                    bool joint, const std::string& label,
                    PlanningOptions planning = {}) {
  auto reads = std::make_shared<Reads>();
  ExecutionBindings bindings;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const auto name = "input" + std::to_string(i);
    const auto value = inputs[i];
    doc.inputs.push_back({i + 1, name, value.descriptor(), value.region(),
                          value.layout(), value.facets()});
    auto source = std::make_shared<RegionalSource>();
    source->descriptor = value.descriptor();
    source->facets = value.facets();
    source->read = [reads, name, value](
                       const Region& region, std::uint8_t* output,
                       std::uint64_t size, const BufferAllocator&,
                       const CancellationToken&) -> Result<Region> {
      {
        std::lock_guard<std::mutex> lock(reads->mutex);
        reads->regions[name].insert(region_text(region));
      }
      auto count = take(region.element_count());
      if (size != count * sizeof(float))
        return Result<Region>(
            Status{ErrorCode::InvalidArgument, "source size"});
      const auto& axes = region.dimensions();
      std::vector<std::uint64_t> coordinate(axes.size());
      for (std::uint64_t i = 0; i < count; ++i) {
        auto remaining = i;
        for (std::size_t axis = axes.size(); axis-- > 0;) {
          coordinate[axis] = axes[axis].offset + remaining % axes[axis].extent;
          remaining /= axes[axis].extent;
        }
        auto address = value.byte_address(coordinate);
        if (!address.ok())
          return Result<Region>(address.status());
        std::memcpy(output + i * sizeof(float),
                    value.bytes().data() + address.value(), sizeof(float));
      }
      return Result<Region>(region);
    };
    bindings.inputs.push_back({name, {}, source});
  }
  auto registry = make_default_operation_registry();
  GraphContext graph(doc);
  planning.tile_height = 1;
  planning.tile_width = 2;
  auto compiled = take(Compiler(registry).compile(graph, planning));
  ExecutionContext execution(registry, {2, false, 64, 16777216, 0});
  ExecutionOptions options;
  options.enable_joint = joint;
  // The 129x129 case intentionally exceeds the conservative default fuel.
  // Bound this demonstration explicitly, including certificate normalization.
  options.maximum_dependency_work = 100000000;
  options.dependencies.maximum_work = 100000000;
  options.dependencies.sets.maximum_work = 100000000;
  auto result = take(execution.execute(compiled.plan, bindings, {}, options));
  std::cout << label << " joint=" << joint
            << " groups=" << result.diagnostics.joint_groups
            << " source_reads=" << result.diagnostics.source_read_count << '\n';
  for (const auto& entry : result.values) {
    std::cout << "  " << entry.first << " shape=";
    for (auto extent : entry.second.descriptor().shape)
      std::cout << extent << ' ';
    std::cout << '\n';
  }
  std::map<ValueRef, std::uint64_t> attempts;
  for (const auto& timing : result.diagnostics.operation_timings)
    attempts[timing.output] += timing.invocation_count;
  for (const auto& entry : attempts)
    std::cout << "  result=" << entry.first.node_id << ':'
              << entry.first.output_index << " attempts=" << entry.second
              << '\n';
  for (const auto& entry : reads->regions) {
    std::cout << "  " << entry.first << " read_set=";
    for (const auto& region : entry.second)
      std::cout << region << ' ';
    std::cout << '\n';
  }
  return result;
}
std::vector<float> pixels() {
  std::vector<float> data(3 * 5 * 3);
  for (std::size_t i = 0; i < data.size(); ++i)
    data[i] = static_cast<float>((i * 7) % 23) / 22;
  return data;
}
void split(bool joint) {
  auto data = pixels();
  WorkflowDocument doc;
  doc.nodes = {{1,
                "image.split_horizontal",
                {WorkflowInputReference{1}},
                {{"split_x", std::int64_t{2}}}}};
  doc.outputs = {{"full", 1, "full"},
                 {"left", 1, "left"},
                 {"right", 1, "right"}};
  PlanningOptions planning;
  planning.output_regions = {{"full", Region({{0, 1}, {3, 1}, {0, 3}})},
                             {"left", Region({{1, 1}, {0, 1}, {0, 3}})},
                             {"right", Region({{2, 1}, {1, 1}, {0, 3}})}};
  auto result =
      run(doc, {samples({3, 5, 3}, data, true)}, joint, "split", planning);
  const std::array<std::string, 3> names{"full", "left", "right"};
  const std::array<std::uint64_t, 3> local_x{3, 0, 1}, source_x{3, 0, 3};
  for (unsigned i = 0; i < 3; ++i)
    for (unsigned c = 0; c < 3; ++c)
      require(sample(result.values.at(names[i]), {i, local_x[i], c}) ==
                  data[(i * 5 + source_x[i]) * 3 + c],
              "split oracle");
}
}  // namespace
int main(int argc, char** argv) {
  try {
    bool joint = true;
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--help") {
        std::cout << "--joint on|off\n";
        return 0;
      }
      require(option == "--joint" && i + 1 < argc, "use --joint on|off");
      const std::string value = argv[++i];
      require(value == "on" || value == "off", "joint value");
      joint = value == "on";
    }
    split(joint);
    std::cout << "multi-output split oracle=passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "multi-output failed: " << error.what() << '\n';
    return 1;
  }
}
