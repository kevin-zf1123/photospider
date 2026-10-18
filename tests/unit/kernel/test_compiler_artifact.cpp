#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "photospider/compiler/artifact.hpp"
#include "photospider/compiler/compiler.hpp"
#include "support/kernel_registry_fixture.hpp"
#include "support/test_support.hpp"

namespace {
/** @brief Regression: decimal artifacts depended on the calling thread's mode.
 */
int roundtrip_in_foreign_environment() {
  const double value = std::nextafter(1.0, 2.0);
  auto registry = ps::test::kernel_registry();
  ps::Compiler compiler(registry);
  ps::GraphContext graph(ps::test::addition_document(value, 2));
  auto compiled = compiler.compile(graph);
  PS_REQUIRE_OK(compiled);
  auto canonical = ps::CompilerArtifact::export_ir(
      compiled.value().ir_versions.front().ir.value());
  PS_REQUIRE_OK(canonical);

  for (const auto mode : {FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
    PS_CHECK(std::fesetround(mode) == 0);
    PS_CHECK(std::feclearexcept(FE_ALL_EXCEPT) == 0);
    PS_CHECK(std::feraiseexcept(FE_DIVBYZERO) == 0);
    const int flags = std::fetestexcept(FE_ALL_EXCEPT);
    auto text = ps::CompilerArtifact::export_ir(
        compiled.value().ir_versions.front().ir.value());
    PS_REQUIRE_OK(text);
    PS_CHECK(text.value() == canonical.value());
    PS_CHECK(std::fegetround() == mode);
    PS_CHECK(std::fetestexcept(FE_ALL_EXCEPT) == flags);
    auto imported = ps::CompilerArtifact::import_ir(compiler, text.value());
    PS_REQUIRE_OK(imported);
    PS_CHECK(std::fegetround() == mode);
    PS_CHECK(std::fetestexcept(FE_ALL_EXCEPT) == flags);
    const auto& semantic = imported.value();
    PS_CHECK(std::get<double>(semantic.nodes()[0].parameters.at("value")) ==
             value);
    PS_CHECK(semantic.digest().value ==
             compiled.value().ir_versions.front().ir.value().digest().value);
  }
  return 0;
}
/** @brief Regression: an exported valid request exceeded a reader-only limit.
 */
int fragmented_request_roundtrip() {
  constexpr std::uint64_t count = 65537;
  const auto schema =
      multi_result::schema(ps::ElementType::Float64, {2 * count - 1});
  auto registry = ps::test::kernel_registry();
  ps::Compiler compiler(registry);
  ps::WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "input", schema)};
  document.nodes = {{1, "core.identity", {ps::WorkflowInputReference{1}}, {}}};
  document.outputs = {{"result", 1, "value"}};
  ps::GraphContext graph(document);
  std::vector<ps::TileInterval> intervals;
  intervals.reserve(count);
  for (std::uint64_t i = 0; i < count; ++i)
    intervals.push_back({2 * i, 2 * i + 1});
  auto tiles = ps::TileSet::from_intervals(ps::ResourceBudget{},
                                           intervals.data(), intervals.size());
  PS_REQUIRE_OK(tiles);
  ps::PlanningOptions options;
  options.tile_extent = 1;
  options.output_requests["result"].members.emplace(0, tiles.take_value());
  auto compiled = compiler.compile(graph, options);
  PS_REQUIRE_OK(compiled);
  auto text = ps::CompilerArtifact::export_plan(compiled.value().plan.value());
  PS_REQUIRE_OK(text);
  auto loaded =
      ps::CompilerArtifact::import_plan(compiler.operations(), text.value());
  PS_REQUIRE_OK(loaded);
  const auto& actual =
      loaded.value().output_requests().at("result").members.at(0);
  PS_CHECK(actual.intervals().size() == count);
  PS_CHECK(actual.contains(0) && !actual.contains(1) &&
           actual.contains(2 * count - 2));
  PS_CHECK(loaded.value().digest().value ==
           compiled.value().plan.value().digest().value);
  auto encoded = ps::CompilerArtifact::export_plan(loaded.value());
  PS_REQUIRE_OK(encoded);
  PS_CHECK(encoded.value() == text.value());
  return 0;
}
}  // namespace

int main() {
  std::fenv_t saved;
  if (std::fegetenv(&saved) != 0)
    return 1;
  const auto result = roundtrip_in_foreign_environment();
  std::fesetenv(&saved);
  return result ? result : fragmented_request_roundtrip();
}
