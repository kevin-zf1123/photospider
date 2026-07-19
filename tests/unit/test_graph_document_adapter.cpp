#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "graph/graph_definition.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_io_service.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_model.hpp"       // NOLINT(build/include_subdir)
#include "graph/in_memory_graph_document_adapter.hpp"  // NOLINT(build/include_subdir)
#include "support/graph_document_test_dependencies.hpp"

namespace ps {
namespace {

/**
 * @brief Owns one unique temporary directory for graph-document tests.
 *
 * @throws std::filesystem::filesystem_error when setup cannot create the
 *         directory.
 * @note Destruction uses the error-code overload and never masks assertions.
 */
class ScopedGraphDocumentDirectory final {
 public:
  /**
   * @brief Creates an empty directory below the platform temporary root.
   * @throws std::filesystem::filesystem_error when cleanup or creation fails.
   */
  ScopedGraphDocumentDirectory()
      : root_(std::filesystem::temp_directory_path() /
              ("photospider_graph_document_adapter_" +
               std::to_string(std::chrono::high_resolution_clock::now()
                                  .time_since_epoch()
                                  .count()))) {
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
  }

  /**
   * @brief Removes the owned directory without throwing.
   * @throws Nothing.
   * @note Cleanup failure is deliberately ignored because the helper is
   *       leaving scope and must not mask a test result.
   */
  ~ScopedGraphDocumentDirectory() noexcept {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  /**
   * @brief Prevents duplicate directory ownership.
   * @param other Owner that retains its directory.
   * @throws Nothing because construction is unavailable.
   */
  ScopedGraphDocumentDirectory(const ScopedGraphDocumentDirectory& other) =
      delete;

  /**
   * @brief Prevents replacement of directory ownership.
   * @param other Owner that retains its directory.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedGraphDocumentDirectory& operator=(
      const ScopedGraphDocumentDirectory& other) = delete;

  /**
   * @brief Returns the owned directory.
   * @return Stable path reference valid for this helper's lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

 private:
  /** @brief Unique directory removed by this owner. */
  std::filesystem::path root_;
};

/**
 * @brief Creates one minimal persistent node definition.
 *
 * @param id Stable graph-local identifier.
 * @param name Human-readable persistent name.
 * @return Detached source-node definition with no topology edges.
 * @throws std::bad_alloc when copied string or parameter storage cannot
 *         allocate.
 * @note The returned value owns every field and can outlive name.
 */
NodeDefinition make_definition_node(int id, const std::string& name) {
  NodeDefinition node;
  node.id = id;
  node.name = name;
  node.type = "fixture";
  node.subtype = "source";
  node.parameters["width"] = 16;
  return node;
}

/**
 * @brief Executes one action and checks its exact graph-error category.
 *
 * @tparam Action Nullary callable type.
 * @param action Operation expected to fail.
 * @param expected_code Exact GraphErrc category required.
 * @return Nothing.
 * @throws Nothing when the tested action follows its declared GraphError
 *         contract; GoogleTest records unexpected outcomes.
 * @note Non-GraphError exceptions are reported as assertion failures rather
 *       than translated.
 */
template <typename Action>
void expect_graph_error(Action&& action, GraphErrc expected_code) {
  try {
    std::forward<Action>(action)();
    FAIL() << "expected GraphError";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), expected_code) << error.what();
  } catch (const std::exception& error) {
    FAIL() << "unexpected exception: " << error.what();
  } catch (...) {
    FAIL() << "unexpected non-standard exception";
  }
}

/**
 * @brief Writes one exact graph YAML fixture.
 *
 * @param path Destination file below an existing test directory.
 * @param contents Complete UTF-8 fixture text.
 * @return Nothing.
 * @throws std::ios_base::failure when opening, writing, or closing fails.
 * @note The stream reports every persistence failure through exceptions.
 */
void write_document(const std::filesystem::path& path,
                    const std::string& contents) {
  std::ofstream stream;
  stream.exceptions(std::ios::failbit | std::ios::badbit);
  stream.open(path);
  stream << contents;
  stream.close();
}

/**
 * @brief Proves apply/capture ownership, ordering, and runtime exclusion.
 * @throws Nothing when the adapter contract holds; GoogleTest records
 *         unexpected exceptions and mismatches.
 * @note The definition is mutated after apply and runtime state is populated
 *       before capture so both ownership directions are observed.
 */
TEST(GraphDocumentAdapter,
     AppliesAndCapturesPersistentValuesWithoutRuntimeState) {
  GraphDefinition definition;
  NodeDefinition source = make_definition_node(9, "source-nine");
  source.preserved = true;

  plugin::ParameterValue::Array levels;
  levels.emplace_back(1);
  levels.emplace_back(2.5);
  levels.emplace_back(nullptr);
  plugin::ParameterValue::Object output_parameters;
  output_parameters.emplace("mode", "precise");
  output_parameters.emplace("levels", std::move(levels));

  OutputPort nested_output;
  nested_output.output_id = 4;
  nested_output.output_type = "metadata";
  nested_output.output_parameters =
      plugin::ParameterValue(std::move(output_parameters));
  source.outputs.push_back(std::move(nested_output));
  source.caches.push_back(CacheEntry{"image", "source-nine.bin"});

  NodeDefinition child = make_definition_node(2, "child-two");
  child.image_inputs.push_back(ImageInput{9, "image"});
  OutputPort explicit_null;
  explicit_null.output_id = 1;
  explicit_null.output_type = "nullable";
  explicit_null.output_parameters = plugin::ParameterValue(nullptr);
  child.outputs.push_back(std::move(explicit_null));
  OutputPort absent;
  absent.output_id = 2;
  absent.output_type = "absent";
  child.outputs.push_back(std::move(absent));

  definition.nodes.push_back(std::move(source));
  definition.nodes.push_back(std::move(child));

  const InMemoryGraphDocumentAdapter adapter;
  GraphModel graph;
  const uint64_t initial_generation = graph.topology_generation();
  adapter.apply(graph, definition);
  EXPECT_EQ(graph.topology_generation(), initial_generation + 1U);

  definition.nodes.front().name = "mutated-source";
  definition.nodes.front().outputs.front().output_parameters =
      plugin::ParameterValue("mutated-output");
  EXPECT_EQ(graph.node(9).name, "source-nine");
  const auto& stored_output = graph.node(9).outputs.front().output_parameters;
  ASSERT_TRUE(stored_output.has_value());
  ASSERT_TRUE(stored_output->is_object());
  EXPECT_EQ(stored_output->as_object().at("mode").as_string(), "precise");

  graph.mutate_node_runtime_state(9, [](GraphModel::NodeRuntimeState& state) {
    state.runtime_parameters["transient"] = "runtime-only";
    state.hp_version = 27;
    state.hp_roi = PixelRect{1, 2, 3, 4};
  });
  graph.timing_results.total_ms = 42.0;

  const GraphDefinition captured = adapter.capture(graph);
  ASSERT_EQ(captured.nodes.size(), 2U);
  EXPECT_EQ(captured.nodes[0].id, 2);
  EXPECT_EQ(captured.nodes[1].id, 9);
  ASSERT_EQ(captured.nodes[0].outputs.size(), 2U);
  ASSERT_TRUE(captured.nodes[0].outputs[0].output_parameters.has_value());
  EXPECT_TRUE(captured.nodes[0].outputs[0].output_parameters->is_null());
  EXPECT_FALSE(captured.nodes[0].outputs[1].output_parameters.has_value());
  ASSERT_EQ(captured.nodes[0].image_inputs.size(), 1U);
  EXPECT_EQ(captured.nodes[0].image_inputs[0].from_node_id, 9);
  EXPECT_EQ(captured.nodes[1].parameters.at("width").as_int64(), 16);
  EXPECT_TRUE(captured.nodes[1].preserved);
  ASSERT_EQ(captured.nodes[1].caches.size(), 1U);
  EXPECT_EQ(captured.nodes[1].caches[0].location, "source-nine.bin");

  Node changed = graph.node(9);
  changed.name = "changed-after-capture";
  graph.replace_node(changed);
  EXPECT_EQ(captured.nodes[1].name, "source-nine");
  const uint64_t reload_generation = graph.topology_generation();
  adapter.apply(graph, captured);
  EXPECT_EQ(graph.topology_generation(), reload_generation + 1U);
  EXPECT_EQ(graph.node(9).name, "source-nine");
  EXPECT_TRUE(graph.node(9).runtime_parameters.empty());
  EXPECT_EQ(graph.node(9).hp_version, 0);
  EXPECT_FALSE(graph.node(9).hp_roi.has_value());
  EXPECT_DOUBLE_EQ(graph.timing_results.total_ms, 0.0);

  GraphModel restored;
  adapter.apply(restored, captured);
  EXPECT_EQ(restored.topology_generation(), 1U);
  const Node& restored_source = restored.node(9);
  EXPECT_TRUE(restored_source.runtime_parameters.empty());
  EXPECT_EQ(restored_source.hp_version, 0);
  EXPECT_FALSE(restored_source.hp_roi.has_value());
  EXPECT_DOUBLE_EQ(restored.timing_results.total_ms, 0.0);
  ASSERT_TRUE(restored_source.outputs.front().output_parameters.has_value());
  EXPECT_EQ(restored_source.outputs.front()
                .output_parameters->as_object()
                .at("levels")
                .as_array()
                .size(),
            3U);
}

/**
 * @brief Proves definition-schema failures preserve published model state.
 * @throws Nothing when invalid definitions retain the prior graph; GoogleTest
 *         records unexpected exceptions and mismatches.
 * @note Duplicate ids and empty parameter endpoints exercise independent
 *       adapter-owned schema rules.
 */
TEST(GraphDocumentAdapter, RejectsInvalidDefinitionBeforePublication) {
  const InMemoryGraphDocumentAdapter adapter;
  GraphModel graph;
  GraphDefinition seed;
  seed.nodes.push_back(make_definition_node(1, "published"));
  adapter.apply(graph, seed);
  graph.mutate_node_runtime_state(1, [](GraphModel::NodeRuntimeState& state) {
    state.runtime_parameters["marker"] = 99;
    state.hp_version = 11;
  });
  const uint64_t generation = graph.topology_generation();

  GraphDefinition duplicate;
  duplicate.nodes.push_back(make_definition_node(2, "first"));
  duplicate.nodes.push_back(make_definition_node(2, "second"));
  expect_graph_error([&]() { adapter.apply(graph, duplicate); },
                     GraphErrc::InvalidParameter);

  GraphDefinition empty_endpoint;
  NodeDefinition invalid = make_definition_node(2, "invalid-edge");
  invalid.parameter_inputs.push_back(ParameterInput{1, "", "threshold"});
  empty_endpoint.nodes.push_back(std::move(invalid));
  expect_graph_error([&]() { adapter.apply(graph, empty_endpoint); },
                     GraphErrc::InvalidParameter);

  ASSERT_EQ(graph.node_count(), 1U);
  EXPECT_EQ(graph.node(1).name, "published");
  EXPECT_EQ(graph.topology_generation(), generation);
  EXPECT_EQ(graph.node(1).runtime_parameters.at("marker").as_int64(), 99);
  EXPECT_EQ(graph.node(1).hp_version, 11);
}

/**
 * @brief Proves topology failures retain exact categories and old state.
 * @throws Nothing when topology rejection retains the prior graph; GoogleTest
 *         records unexpected exceptions and mismatches.
 * @note Missing dependencies and cycles remain GraphModel-owned categories
 *       rather than being normalized to definition schema failures.
 */
TEST(GraphDocumentAdapter, PreservesExactTopologyFailuresAndOldGraph) {
  const InMemoryGraphDocumentAdapter adapter;
  GraphModel graph;
  GraphDefinition seed;
  seed.nodes.push_back(make_definition_node(1, "published"));
  adapter.apply(graph, seed);
  const uint64_t generation = graph.topology_generation();

  GraphDefinition missing;
  NodeDefinition missing_child = make_definition_node(2, "missing");
  missing_child.image_inputs.push_back(ImageInput{99, "image"});
  missing.nodes.push_back(std::move(missing_child));
  expect_graph_error([&]() { adapter.apply(graph, missing); },
                     GraphErrc::MissingDependency);

  GraphDefinition cycle;
  NodeDefinition first = make_definition_node(2, "cycle-two");
  first.image_inputs.push_back(ImageInput{3, "image"});
  NodeDefinition second = make_definition_node(3, "cycle-three");
  second.parameter_inputs.push_back(ParameterInput{2, "value", "threshold"});
  cycle.nodes.push_back(std::move(first));
  cycle.nodes.push_back(std::move(second));
  expect_graph_error([&]() { adapter.apply(graph, cycle); }, GraphErrc::Cycle);

  ASSERT_EQ(graph.node_count(), 1U);
  EXPECT_EQ(graph.node(1).name, "published");
  EXPECT_EQ(graph.topology_generation(), generation);
}

/**
 * @brief Proves the real YAML load/save path preserves optional nested output
 *        parameter semantics through GraphDefinition.
 * @throws Nothing when load/save/reload round-trips the exact value kinds;
 *         GoogleTest records unexpected exceptions and mismatches.
 * @note The fixture distinguishes absent output parameters from an explicitly
 *       present null and a nested object/array value.
 */
TEST(GraphDocumentAdapter, YamlProductPathRoundTripsOutputParameters) {
  ScopedGraphDocumentDirectory directory;
  const std::filesystem::path source = directory.root() / "source.yaml";
  const std::filesystem::path saved = directory.root() / "saved.yaml";
  write_document(source,
                 "- id: 4\n"
                 "  name: nested\n"
                 "  type: fixture\n"
                 "  subtype: source\n"
                 "  outputs:\n"
                 "    - output_id: 7\n"
                 "      output_type: metadata\n"
                 "      output_parameters:\n"
                 "        mode: precise\n"
                 "        levels: [1, 2.5, null, true]\n"
                 "- id: 2\n"
                 "  name: optional\n"
                 "  type: fixture\n"
                 "  subtype: source\n"
                 "  outputs:\n"
                 "    - output_id: 1\n"
                 "      output_type: nullable\n"
                 "      output_parameters: null\n"
                 "    - output_id: 2\n"
                 "      output_type: absent\n");

  const GraphIOService io = ps::testing::make_yaml_graph_io_service();
  GraphModel loaded;
  ASSERT_NO_THROW(io.load(loaded, source));
  ASSERT_NO_THROW(io.save(loaded, saved));

  GraphModel reloaded;
  ASSERT_NO_THROW(io.load(reloaded, saved));
  const InMemoryGraphDocumentAdapter adapter;
  const GraphDefinition captured = adapter.capture(reloaded);
  ASSERT_EQ(captured.nodes.size(), 2U);
  EXPECT_EQ(captured.nodes[0].id, 2);
  EXPECT_EQ(captured.nodes[1].id, 4);

  ASSERT_EQ(captured.nodes[0].outputs.size(), 2U);
  ASSERT_TRUE(captured.nodes[0].outputs[0].output_parameters.has_value());
  EXPECT_TRUE(captured.nodes[0].outputs[0].output_parameters->is_null());
  EXPECT_FALSE(captured.nodes[0].outputs[1].output_parameters.has_value());

  ASSERT_EQ(captured.nodes[1].outputs.size(), 1U);
  const auto& nested = captured.nodes[1].outputs[0].output_parameters;
  ASSERT_TRUE(nested.has_value());
  ASSERT_TRUE(nested->is_object());
  EXPECT_EQ(nested->as_object().at("mode").as_string(), "precise");
  const auto& levels = nested->as_object().at("levels").as_array();
  ASSERT_EQ(levels.size(), 4U);
  EXPECT_EQ(levels[0].as_int64(), 1);
  EXPECT_DOUBLE_EQ(levels[1].as_double(), 2.5);
  EXPECT_TRUE(levels[2].is_null());
  EXPECT_TRUE(levels[3].as_bool());
}

}  // namespace
}  // namespace ps
