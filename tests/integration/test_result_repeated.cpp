#include <dlfcn.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../../examples/numeric_workflow/result_fixture.hpp"
#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
using Counter = std::uint32_t (*)();
ExecutionBinding binding(const ResourceBudget& root, std::string name,
                         const SchemaTemplate& schema, unsigned input,
                         unsigned layout) {
  const auto& descriptor = schema.tensors[0].descriptor;
  const auto count = descriptor.shape[0];
  const auto width = Value::element_size(descriptor.element_type);
  const bool zero = layout == 2;
  std::vector<std::uint8_t> bytes((zero ? 1 : count) * width);
  for (std::uint64_t i = 0; i < (zero ? 1 : count); ++i) {
    const double value = input + 1 + i;
    auto* out = bytes.data() + (layout == 1 ? count - 1 - i : i) * width;
    if (descriptor.element_type == ElementType::UInt8) {
      *out = static_cast<std::uint8_t>(value);
    } else {
      multi_result::write_number(out, descriptor.element_type, value);
    }
  }
  StridedLayout strides;
  strides.byte_offset = layout == 1 ? (count - 1) * width : 0;
  strides.byte_strides = {zero          ? 0
                          : layout == 1 ? -static_cast<std::int64_t>(width)
                                        : static_cast<std::int64_t>(width)};
  auto backing = take(Value::create(descriptor, Region::whole({count}), strides,
                                    std::move(bytes)));
  ExecutionBinding result;
  result.name = std::move(name);
  result.result = numeric_result_fixture::source(root, backing, &schema);
  return result;
}
WorkflowDocument document(const std::string& key,
                          const std::vector<SchemaTemplate>& schemas) {
  WorkflowDocument doc;
  std::vector<WorkflowInput> inputs;
  for (unsigned i = 0; i < schemas.size(); ++i) {
    doc.inputs.push_back(multi_result::declaration(
        i + 1, "input" + std::to_string(i), schemas[i]));
    inputs.push_back(WorkflowInputReference{i + 1});
  }
  doc.nodes = {{1, key, std::move(inputs), {}}};
  doc.outputs = {{"sum", 1, "value"}};
  return doc;
}
int execute_groups(const std::shared_ptr<OperationRegistry>& registry,
                   Counter starts, Counter destroys) {
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  for (bool prefix : {false, true}) {
    const std::string key =
        prefix ? "fixture.repeated.resolved" : "fixture.repeated.fixed";
    for (auto type : {ElementType::UInt8, ElementType::Int64,
                      ElementType::Float32, ElementType::Float64}) {
      if (!prefix && type != ElementType::Float64)
        continue;
      for (unsigned count = 1; count <= 4; ++count) {
        for (unsigned layout = 0; layout < 3; ++layout) {
          const auto shape = prefix ? 5U : 3U;
          std::vector<SchemaTemplate> schemas(
              count, multi_result::schema(type, {shape}));
          if (prefix)
            schemas.insert(schemas.begin(), multi_result::schema());
          auto doc = document(key, schemas);
          GraphContext graph(doc);
          auto plan = take(Compiler(registry).compile(graph)).plan;
          ExecutionBindings bindings;
          for (unsigned i = 0; i < schemas.size(); ++i) {
            auto source =
                prefix && i == 0
                    ? multi_result::binding(root, doc.inputs[i].name, 3,
                                            schemas[i])
                    : binding(
                          root, doc.inputs[i].name, schemas[i],
                          i - (prefix ? 1U : 0U),
                          layout == 2 && i != schemas.size() - 1 ? 0 : layout);
            bindings.inputs.push_back(std::move(source));
          }
          const auto before_start = starts(), before_destroy = destroys();
          auto executed = context.execute(plan, bindings);
          if (!executed.ok())
            std::cerr << key << ": " << executed.status().message << '\n';
          PS_CHECK(executed.ok() && starts() == before_start + 1 &&
                   destroys() == before_destroy + 1);
          const auto& output = executed.value().results.at("sum");
          PS_CHECK(output.schema().tensors[0].descriptor.shape ==
                   std::vector<uint64_t>{shape});
          for (std::uint64_t j = 0; j < shape; ++j) {
            const double expected =
                (prefix ? 3 : 1) * (count * (count + 1) / 2 +
                                    (count - (layout == 2 ? 1U : 0U)) * j);
            PS_CHECK(multi_result::number(output, {j}) == expected);
          }
          auto observations =
              take(executed.value().dependencies.source_observations());
          for (unsigned i = 0; i < schemas.size(); ++i) {
            bool tensor = false, descriptor = false;
            for (const auto& observation : observations) {
              if (std::string_view(observation.input) != doc.inputs[i].name)
                continue;
              tensor |= observation.target == ResultSupportTarget::Tensor &&
                        observation.slot == 0 && (observation.roles & 1);
              descriptor |=
                  observation.target == ResultSupportTarget::Descriptor &&
                  (observation.roles & 8);
            }
            PS_CHECK(tensor && descriptor);
            auto changed = take(Footprint::from_regions(
                schemas[i].tensors[0].sample_shape(), {Region({{0, 1}})}));
            auto dirty = take(executed.value().dependencies.potential_dirty(
                doc.inputs[i].name, changed, 1, {}, ResultSupportTarget::Tensor,
                0));
            PS_CHECK(dirty.at("sum") ==
                     (prefix && i == 0 ? take(Footprint::all({shape}))
                                       : take(Footprint::from_regions(
                                             {shape}, {Region({{0, 1}})}))));
          }
        }
      }
    }
  }
  return 0;
}
int heterogeneous(const std::shared_ptr<OperationRegistry>& registry,
                  Counter starts, Counter destroys) {
  auto schemas = std::vector<SchemaTemplate>{
      multi_result::schema(), multi_result::schema(ElementType::Float64, {3}),
      multi_result::schema(ElementType::Float32, {4})};
  auto doc = document("fixture.repeated.heterogeneous", schemas);
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  ExecutionBindings inputs{
      {multi_result::binding(root, "input0", 3, schemas[0]),
       binding(root, "input1", schemas[1], 0, 1),
       binding(root, "input2", schemas[2], 1, 2)}};
  const auto before_start = starts(), before_destroy = destroys();
  auto result = context.execute(plan, inputs);
  PS_CHECK(result.ok() && starts() == before_start + 1 &&
           destroys() == before_destroy + 1 &&
           multi_result::number(result.value().results.at("sum")) == 9);
  return 0;
}
int admission(const std::shared_ptr<OperationRegistry>& registry,
              Counter starts) {
  for (const auto* key :
       {"fixture.repeated.fixed", "fixture.repeated.resolved"}) {
    const unsigned fixed = std::string(key) == "fixture.repeated.fixed" ? 0 : 1;
    auto template_traits = take(registry->find_traits(key));
    PS_CHECK(template_traits.input_count == fixed &&
             template_traits.input_schema.size() == fixed + 1 &&
             template_traits.repeated_minimum == 1 &&
             template_traits.repeated_maximum == 4 &&
             !template_traits.repeated_resolved);
    for (unsigned repeated : {0U, 1U, 4U, 5U}) {
      std::vector<OperationMetadata> inputs;
      for (unsigned i = 0; i < fixed + repeated; ++i) {
        OperationMetadata metadata;
        metadata.result_schema =
            std::make_shared<SchemaTemplate>(multi_result::schema(
                ElementType::Float64, {fixed && i == 0 ? 1U : 3U}));
        inputs.push_back(std::move(metadata));
      }
      const auto before = starts();
      auto resolved = registry->resolve_traits(key, inputs, {});
      PS_CHECK(starts() == before);
      if (repeated == 0 || repeated == 5) {
        PS_CHECK(resolved.status().code == ErrorCode::InvalidArgument);
      } else {
        PS_CHECK(resolved.ok() &&
                 resolved.value().input_count == inputs.size() &&
                 resolved.value().repeated_resolved == repeated &&
                 resolved.value().input_schema.size() == inputs.size());
      }
    }
  }
  for (unsigned bad = 0; bad < 3; ++bad) {
    auto schemas = std::vector<SchemaTemplate>{
        multi_result::schema(), multi_result::schema(ElementType::Float64, {3}),
        multi_result::schema(
            bad == 0 ? ElementType::Float32 : ElementType::Float64,
            {bad == 1 ? 4U : 3U})};
    if (bad == 2)
      schemas[2].tensors[0].descriptor.shape = {1, 3};
    std::vector<OperationMetadata> inputs;
    for (const auto& schema : schemas) {
      OperationMetadata metadata;
      metadata.result_schema = std::make_shared<SchemaTemplate>(schema);
      inputs.push_back(std::move(metadata));
    }
    const auto before = starts();
    auto resolved =
        registry->resolve_traits("fixture.repeated.resolved", inputs, {});
    PS_CHECK(resolved.status().code == ErrorCode::TypeMismatch &&
             starts() == before);
  }
  // No resolver: every repeated input keeps the full registered schema body.
  ResultProgramMetadata metadata;
  metadata.inputs.resize(2);
  for (auto& input : metadata.inputs)
    input.result_schema = std::make_shared<SchemaTemplate>(
        multi_result::schema(ElementType::Float64, {4}));
  metadata.output.result_schema = std::make_shared<SchemaTemplate>(
      take(registry->find_traits("fixture.repeated.fixed"))
          .outputs[0]
          .result_schema.value());
  const std::map<std::string, ParameterValue> parameters;
  ResultProgramQuery query(metadata, parameters);
  query.semantic_key = "fixture.repeated.direct";
  ResourceBudget root;
  const auto before = starts();
  auto invalid =
      registry->start_result("fixture.repeated.fixed", query, root.allocator());
  PS_CHECK(invalid.status().code == ErrorCode::TypeMismatch &&
           starts() == before &&
           root.statistics().peak[ResourceKind::Host] == 0);
  return 0;
}
}  // namespace
int main() try {
  auto module = std::shared_ptr<void>(
      dlopen(PS_REPEATED_FIXTURE, RTLD_NOW | RTLD_LOCAL), [](void* value) {
        if (value)
          dlclose(value);
      });
  PS_CHECK(module != nullptr);
  auto starts =
      reinterpret_cast<Counter>(dlsym(module.get(), "fixture_repeated_starts"));
  auto destroys = reinterpret_cast<Counter>(
      dlsym(module.get(), "fixture_repeated_destroys"));
  PS_CHECK(starts && destroys);
  auto registry = std::make_shared<ps::OperationRegistry>();
  check(registry->load_plugin(PS_REPEATED_FIXTURE));
  check(registry->freeze());
  PS_CHECK(admission(registry, starts) == 0);
  PS_CHECK(execute_groups(registry, starts, destroys) == 0);
  PS_CHECK(heterogeneous(registry, starts, destroys) == 0);
#ifdef PS_BAD_REPEATED_1
  for (const char* path :
       {PS_BAD_REPEATED_1, PS_BAD_REPEATED_2, PS_BAD_REPEATED_3,
        PS_BAD_REPEATED_4, PS_BAD_REPEATED_5, PS_BAD_REPEATED_6}) {
    ps::OperationRegistry empty;
    auto failed = empty.load_plugin(path);
    PS_CHECK(failed.code == ps::ErrorCode::InvalidArgument &&
             empty.keys().empty());
  }
#endif
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
