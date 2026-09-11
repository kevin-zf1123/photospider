#include <cstring>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

void progressive_workflow();
void dynamic_workflow();
void demand_workflow();
void sharing_workflow();
void cache_workflow();
void reductions_workflow();
void scan_workflow();
void measure_workflow();

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T checked(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
void data_workflow() {
  using namespace ps;  // NOLINT(build/namespaces)
  constexpr std::uint64_t width = 1000000000;
  const ValueDescriptor descriptor{ElementType::Float64, {width}};
  WorkflowDocument document;
  document.inputs = {
      {1, "source", descriptor, Region::whole(descriptor.shape), {0, {8}}, {}}};
  document.nodes = {{1, "core.identity", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"result", 1, "value"}};
  auto operations = make_default_operation_registry();
  GraphContext graph(document);
  Compiler compiler(operations);
  const auto compiled = checked(compiler.compile(graph));
  ExecutionContext execution(operations, {1, false, 8, 4096});
  std::set<std::uint64_t> reads;
  auto source = std::make_shared<RegionalSource>();
  source->descriptor = descriptor;
  source->read = [&](const Region& region, std::uint8_t* bytes,
                     std::uint64_t size, const BufferAllocator&,
                     const CancellationToken& cancellation) {
    const auto d = region.dimensions()[0];
    if (size != d.extent * sizeof(double))
      return Result<Region>(
          Status::failure(ErrorCode::TypeMismatch, "source size"));
    for (std::uint64_t i = 0; i < d.extent; ++i) {
      if (cancellation.cancelled())
        return Result<Region>(
            Status::failure(ErrorCode::Cancelled, "source stopped"));
      const auto coordinate = d.offset + i;
      reads.insert(coordinate);
      const double value = static_cast<double>(coordinate);
      std::memcpy(bytes + i * sizeof(double), &value, sizeof(double));
    }
    return Result<Region>(region);
  };
  const auto request = checked(Footprint::from_regions(
      descriptor.shape, {Region({{1, 1}}), Region({{width - 2, 1}})}));
  std::vector<Value> values;
  // This scenario constructs fragments from separate rectangular requests.
  for (const auto& region : request.boxes()) {
    const auto plan = checked(compiled.plan.tile_plan("result", region));
    auto result =
        checked(execution.execute(plan, {{{"source", {}, source, {}}}}));
    values.push_back(result.values.at("result"));
  }
  auto fragments =
      checked(ValueFragments::create(descriptor, {}, request, values));
  double left = 0, right = 0, hole = 7;
  require(fragments.read({1}, &left, sizeof(left)).ok(),
          "left fragment missing");
  require(fragments.read({width - 2}, &right, sizeof(right)).ok(),
          "right fragment missing");
  require(left == 1 && right == 999999998,
          "independent identity oracle failed");
  require(reads == std::set<std::uint64_t>({1, width - 2}),
          "unexpected source reads");
  require(!fragments.read({width / 2}, &hole, sizeof(hole)).ok() && hole == 7,
          "hole was materialized");
  std::vector<AtomCertificate> rows;
  for (const auto index : reads) {
    auto singleton = checked(
        Footprint::from_regions(descriptor.shape, {Region({{index, 1}})}));
    rows.push_back({{index}, {{0, 1, singleton, {}}}});
  }
  const auto certificate = checked(DependencyCertificate::create(
      "identity/data-example", request, {descriptor.shape}, rows));
  auto dirty =
      checked(Footprint::from_regions(descriptor.shape, {Region({{1, 1}})}));
  require(checked(certificate.transpose({0, 1, dirty, {}})) == dirty,
          "identity transpose oracle failed");
  auto original = Value::create({ElementType::Int64, {2}}, Region::whole({2}),
                                {0, {8}}, std::vector<std::uint8_t>(16))
                      .take_value();
  InputSnapshotStore store({32, 1});
  auto snapshot = checked(store.import_value(original));
  require(checked(snapshot.content_identity(original.region())).size() == 64,
          "generic snapshot identity missing");
  std::cout << "data: values=[1,999999998], source_reads=2, hole=rejected, "
               "transpose={1}, generic_snapshot=ok\n";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--measure") {
      measure_workflow();
      return 0;
    }
    if (argc != 1)
      throw std::runtime_error("usage: photospider_g4_workflow [--measure]");
    data_workflow();
    progressive_workflow();
    dynamic_workflow();
    demand_workflow();
    sharing_workflow();
    cache_workflow();
    reductions_workflow();
    scan_workflow();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
