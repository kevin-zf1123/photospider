#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "02-format-color/result_mapping.hpp"
#include "photospider/ops.hpp"
#include "photospider/ops/registry.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
int fragmented_channels() {
  using namespace ps;  // NOLINT(build/namespaces)
  using multi_result::check;
  using multi_result::take;
  auto registry = make_default_operation_registry();
  ExecutionContext context(registry);
  auto root = take(context.resource_budget());
  auto schema = multi_result::schema(ElementType::UInt8, {2, 3});
  auto builder = take(ResultBuilder::start(root, schema, "mapping.fragments"));
  for (std::uint64_t row = 0; row < 2; ++row) {
    const std::uint8_t bytes[] = {static_cast<std::uint8_t>(row * 3),
                                  static_cast<std::uint8_t>(row * 3 + 1),
                                  static_cast<std::uint8_t>(row * 3 + 2)};
    check(builder.publish_tensor(0, Region({{row, 1}, {0, 3}}),
                                 ByteView(bytes, 3)));
  }
  auto source = take(builder.seal());
  for (const auto* extraction : {"auto", "view", "materialize"})
    for (const auto* assembly : {"auto", "materialize"}) {
      WorkflowDocument document;
      document.inputs = {multi_result::declaration(1, "input", schema)};
      OperationMetadata metadata;
      metadata.result_schema = std::make_shared<SchemaTemplate>(schema);
      format::ChannelExtractOptions extract;
      extract.axis = 1;
      extract.metadata_mode = "raw";
      extract.layout = extraction;
      auto channels = take(format::split_channels(
          document, WorkflowInputReference{1}, metadata, extract));
      format::ChannelAssemblyOptions assemble;
      assemble.metadata_mode = "raw";
      assemble.layout = assembly;
      auto output = take(format::assemble_channels(
          document, {channels[2].output, channels[0].output}, 1, assemble));
      document.outputs = {{"out", output.source_node, output.source_port}};
      GraphContext graph(document);
      auto plan = take(Compiler(registry).compile(graph)).plan;
      auto result = context.execute(plan, {{{"input", source}}});
      PS_REQUIRE_OK(result);
      const auto& value = result.value().results.at("out");
      auto facts = take(value.descriptor());
      for (std::uint64_t row = 0; row < 2; ++row)
        for (std::uint64_t column = 0; column < 2; ++column) {
          std::uint8_t actual = 255;
          check(value.read_tensor(facts, 0, {row, column}, &actual, 1));
          PS_CHECK(actual == row * 3 + (column == 0 ? 2 : 0));
        }
    }
  return 0;
}
}  // namespace

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  using plugin_internal::format_result::mapped_preimage;
  PS_CHECK(fragmented_channels() == 0);
  const Region output({{1, 7}, {2, 5}});
  for (int axis = -1; axis < 2; ++axis)
    for (int step = -2; step <= 2; ++step)
      for (std::uint64_t low = 0; low < 25; ++low)
        for (std::uint64_t width = 1; width <= 4; ++width) {
          const std::vector<ResultViewAxis> map{{axis, 16, step, 1},
                                                {1, 1, 1, 1, 2}};
          const Region source({{low, width}, {2, 3}});
          auto selected = mapped_preimage(output, source, map);
          for (std::uint64_t y = 1; y < 8; ++y)
            for (std::uint64_t x = 2; x < 7; ++x) {
              const auto coordinate =
                  axis < 0
                      ? 16
                      : 16 +
                            step * static_cast<std::int64_t>(axis == 0 ? y : x);
              const bool expected =
                  coordinate >= static_cast<std::int64_t>(low) &&
                  coordinate < static_cast<std::int64_t>(low + width) &&
                  x - 1 >= 2 && x - 1 < 5;
              bool actual = false;
              if (selected) {
                const auto& dims = selected->dimensions();
                actual = y >= dims[0].offset &&
                         y - dims[0].offset < dims[0].extent &&
                         x >= dims[1].offset &&
                         x - dims[1].offset < dims[1].extent;
              }
              PS_CHECK(actual == expected);
            }
        }
  const auto edge =
      mapped_preimage(Region({{UINT64_MAX - 3, 3}}), Region({{0, 2}}),
                      {{0, 2, -1, 1, UINT64_MAX - 3}});
  PS_CHECK(edge && edge->dimensions()[0].offset == UINT64_MAX - 2 &&
           edge->dimensions()[0].extent == 2);
  return 0;
}
