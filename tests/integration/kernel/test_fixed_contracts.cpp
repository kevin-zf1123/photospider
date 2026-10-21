#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/status_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
using namespace ps;  // NOLINT(build/namespaces)
struct Library {
  using Mutate = void (*)(int);
  Mutate mutate = nullptr;
#if defined(_WIN32)
  HMODULE handle = LoadLibraryA(PS_CONTRACT_MODULE);
#else
  void* handle = dlopen(PS_CONTRACT_MODULE, RTLD_NOW | RTLD_LOCAL);
#endif
  Library() {
    if (!handle)
      return;
#if defined(_WIN32)
    auto symbol = GetProcAddress(handle, "ps_contract_mutate");
#else
    auto symbol = dlsym(handle, "ps_contract_mutate");
#endif
    static_assert(sizeof(mutate) == sizeof(symbol));
    std::memcpy(&mutate, &symbol, sizeof(mutate));
  }
  ~Library() {
    if (mutate)
      mutate(0);
    if (!handle)
      return;
#if defined(_WIN32)
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
  }
};
SchemaTemplate input_schema() {
  SchemaTemplate schema;
  schema.id = "contract.input";
  ResultTensorSpec member;
  member.key = "target";
  member.descriptor = {ElementType::Float32, {3}};
  schema.tensors.push_back(std::move(member));
  return schema;
}
int members(const std::shared_ptr<OperationRegistry>& registry) {
  const auto prepare = [&](const char* key, const SchemaTemplate& schema) {
    OperationMetadata input;
    input.result_schema = std::make_shared<const SchemaTemplate>(schema);
    return registry->prepare_operation(key, {input}, {});
  };
  auto schema = input_schema();
  PS_REQUIRE_OK(prepare("contract.member", schema));
  PS_REQUIRE_OK(prepare("contract.sole", schema));
  schema.tensors[0].key = "other";
  PS_CHECK(prepare("contract.member", schema).status().code ==
           ErrorCode::TypeMismatch);
  PS_REQUIRE_OK(prepare("contract.sole", schema));
  schema = input_schema();
  auto second = schema.tensors[0];
  second.key = "other";
  second.descriptor.element_type = ElementType::Float64;
  schema.tensors.insert(schema.tensors.begin(), second);
  PS_REQUIRE_OK(schema.validate());
  // The named predicate selects slot one; a sole predicate remains ambiguous
  // even when only one member has its requested dtype.
  auto named = prepare("contract.member", schema);
  PS_REQUIRE_OK(named);
  PS_CHECK(prepare("contract.sole", schema).status().code ==
           ErrorCode::TypeMismatch);
  schema.tensors[1].descriptor.element_type = ElementType::Int64;
  PS_CHECK(prepare("contract.member", schema).status().code ==
           ErrorCode::TypeMismatch);
  schema = input_schema();
  schema.tensors[0].descriptor.shape = {1, 3};
  PS_CHECK(prepare("contract.member", schema).status().code ==
           ErrorCode::TypeMismatch);
  return 0;
}
WorkflowDocument document(const char* operation) {
  WorkflowDocument result;
  result.nodes = {{1, operation, {}, {}}};
  result.outputs = {{"out", 1, "value"}};
  return result;
}
int parameters(const std::shared_ptr<OperationRegistry>& registry) {
  Compiler compiler(registry);
  ExecutionContext context(registry);
  auto doc = document("contract.parameters");
  doc.nodes[0].parameters = {{"offset", INT64_C(1)}, {"scale", 2.0}};
  GraphContext graph(doc);
  auto compiled = compiler.compile(graph);
  PS_REQUIRE_OK(compiled);
  auto prepared = context.prepare(compiled.value().plan.value());
  PS_REQUIRE_OK(prepared);
  auto run = context.submit(prepared.value());
  PS_REQUIRE_OK(run);
  auto completed = run.value().join();
  PS_REQUIRE_OK(completed);
  const auto& output = completed.value().results.at("out");
  PS_CHECK(output.schema().id == "contract.value" &&
           output.schema().tensors[0].key == "number");
  auto facts = output.descriptor();
  PS_REQUIRE_OK(facts);
  double value = 0;
  PS_REQUIRE_OK(output.read_tensor(facts.value(), 0, {0}, &value, 8));
  PS_CHECK(value == 62);
  for (const auto& values : std::vector<std::map<std::string, ParameterValue>>{
           {{"offset", 1.0}},
           {{"scale", INT64_C(2)}},
           {{"offset", INT64_C(3)}},
           {{"offset", INT64_C(-3)}},
           {{"scale", 5.0}},
           {{"scale", -1.0}},
           {{"unknown", INT64_C(1)}}}) {
    doc.nodes[0].parameters = values;
    GraphContext invalid(doc);
    auto failed = compiler.compile(invalid);
    PS_CHECK(!failed.ok() &&
             failed.status().code == ErrorCode::InvalidArgument);
  }
  return 0;
}
int vocabulary(const std::shared_ptr<OperationRegistry>& registry) {
  auto prepared = registry->prepare_operation("contract.vocabulary", {}, {});
  PS_REQUIRE_OK(prepared);
  const auto& schema = *prepared.value()->traits().outputs[0].result_schema;
  const ElementType types[] = {ElementType::UInt8,   ElementType::Int64,
                               ElementType::Float64, ElementType::Float32,
                               ElementType::Int8,    ElementType::UInt16,
                               ElementType::Int16};
  PS_CHECK(schema.tensors.size() == 7);
  for (unsigned i = 0; i < 7; ++i)
    PS_CHECK(schema.tensors[i].descriptor.element_type == types[i]);
  PS_CHECK(schema.publication_scopes.size() == 3);
  PS_CHECK(schema.publication_scopes[0].policy ==
           ResultPublicationPolicy::CompleteBundle);
  PS_CHECK(schema.publication_scopes[1].policy ==
           ResultPublicationPolicy::IndependentChunks);
  PS_CHECK(schema.publication_scopes[2].policy ==
           ResultPublicationPolicy::StablePrefix);
  const FailureReason reasons[] = {FailureReason::None,
                                   FailureReason::ArithmeticOverflow,
                                   FailureReason::InvalidDomain,
                                   FailureReason::MalformedEnvelope,
                                   FailureReason::UnauthorizedRead,
                                   FailureReason::StaleHandle,
                                   FailureReason::HostException,
                                   FailureReason::ShortIo,
                                   FailureReason::WorkLimit,
                                   FailureReason::CapacityLimit,
                                   FailureReason::StageLimit,
                                   FailureReason::Cancelled,
                                   FailureReason::StaleVersion,
                                   FailureReason::ViewUnavailable};
  for (std::int64_t i = 0; i < 14; ++i) {
    auto failed =
        registry->prepare_operation("contract.vocabulary", {}, {{"reason", i}});
    PS_CHECK(!failed.ok());
    PS_CHECK(failed.status().code == ErrorCode::OperationFailed);
    PS_CHECK(failed.status().reason == reasons[i]);
    PS_CHECK(failed.status().detail.domain_code ==
             UINT64_C(0xfedcba9876543210));
  }
  auto unknown = registry->prepare_operation("contract.vocabulary", {},
                                             {{"reason", INT64_C(14)}});
  PS_CHECK(!unknown.ok());
  PS_CHECK(unknown.status().code == ErrorCode::Internal);
  PS_CHECK(unknown.status().reason == FailureReason::MalformedEnvelope);
  return 0;
}
int sparse(const std::shared_ptr<OperationRegistry>& registry) {
  constexpr std::uint64_t extent = UINT64_C(1) << 61;
  ResourceBudget root;
  {
    GraphContext graph(document("contract.sparse"));
    PlanningOptions options;
    options.tile_extent = 1;
    auto selected = TileSet::from_indices(ResourceBudget{}, {0, extent - 1});
    PS_REQUIRE_OK(selected);
    options.output_requests["out"].members.emplace(0, selected.take_value());
    auto compiled = Compiler(registry).compile(graph, options);
    PS_REQUIRE_OK(compiled);
    PS_CHECK(compiled.value().plan.value().program().tasks.size() == 2);
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Payload] = 65536;
    ExecutionContext context(registry, config);
    root = context.resource_budget().take_value();
    for (auto coordinate : {std::uint64_t{0}, extent - 1}) {
      auto tile = TileSet::from_indices(root, {coordinate});
      PS_REQUIRE_OK(tile);
      TileRequest request;
      request.members.emplace(0, tile.take_value());
      auto plan =
          compiled.value().plan.value().select_outputs({{"out", request}});
      PS_REQUIRE_OK(plan);
      auto prepared = context.prepare(plan.value());
      PS_REQUIRE_OK(prepared);
      auto run = context.submit(prepared.value());
      PS_REQUIRE_OK(run);
      auto finished = run.value().join();
      PS_REQUIRE_OK(finished);
      PS_CHECK(finished.value().diagnostics.completed_tasks == 1);
      const auto& output = finished.value().results.at("out");
      PS_CHECK(output.schema().tensors[0].descriptor.shape ==
               std::vector<std::uint64_t>({1, extent}));
      auto facts = output.descriptor();
      PS_REQUIRE_OK(facts);
      const Region region({{0, 1}, {coordinate, 1}});
      auto coverage = Footprint::from_regions({1, extent}, {region});
      PS_REQUIRE_OK(coverage);
      PS_CHECK(facts.value().tensor_coverage(0) == coverage.value());
      double value = 0;
      PS_REQUIRE_OK(
          output.read_tensor(facts.value(), 0, {0, coordinate}, &value, 8));
      PS_CHECK(value == 11);
      PS_CHECK(!output
                    .read_tensor(facts.value(), 0, {0, coordinate ? 0U : 1U},
                                 &value, 8)
                    .ok());
      auto window = output.acquire_tensor(facts.value(), 0, region);
      PS_REQUIRE_OK(window);
      auto row = window.value().row_run({0, coordinate});
      PS_REQUIRE_OK(row);
      PS_CHECK(row.value().bytes == 8);
    }
    PS_CHECK(root.statistics().peak[ResourceKind::Payload] <= 65536);
  }
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
}  // namespace
int main() {
  Library library;
  PS_CHECK(library.handle && library.mutate);
  auto registry = std::make_shared<ps::OperationRegistry>();
  PS_REQUIRE_OK(registry->load_plugin(PS_CONTRACT_MODULE));
  PS_REQUIRE_OK(registry->freeze());
  const auto keys = registry->keys();
  library.mutate(1);
  PS_CHECK(registry->keys() == keys);
  PS_CHECK(members(registry) == 0);
  PS_CHECK(parameters(registry) == 0);
  PS_CHECK(sparse(registry) == 0);
  PS_CHECK(vocabulary(registry) == 0);
  return 0;
}
