#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "05-filter/advanced.hpp"
#include "photospider/ops/filter.hpp"
#include "photospider/ops/numeric/workflow_authoring.hpp"

namespace ps::filter {
namespace {
using plugin_internal::filter::parameter;
using plugin_internal::filter::Parameters;
using plugin_internal::filter::require;
using plugin_internal::filter::take;
constexpr const char* kProfile = "pyramid_binomial5_v1";
constexpr const char* kRounding = "RN_t_reduce_expand_residual_v1";
constexpr const char* kPadding = "reflect_half_even_phase_ceil_shape_v1";
void validate(const PyramidOptions& p) {
  plugin_internal::generation::size_product(p.original_shape);
  const auto rank = p.original_shape.size();
  parameter(rank >= 2 && p.y_axis >= 0 && p.x_axis >= 0 &&
                static_cast<std::size_t>(p.y_axis) < rank &&
                static_cast<std::size_t>(p.x_axis) < rank &&
                p.y_axis != p.x_axis,
            "pyramid logical axes");
  parameter(p.dtype == ElementType::Float32 || p.dtype == ElementType::Float64,
            "pyramid dtype");
  parameter(p.profile == "strict" || p.profile == "accelerated_apple_silicon" ||
                p.profile == "accelerated_x86_64",
            "pyramid backend profile");
  parameter(!p.source_grid.empty() && !p.unit.empty(),
            "pyramid grid identity/unit");
  for (unsigned a = 0; a < 2; ++a)
    parameter(std::isfinite(p.origin[a]) && std::isfinite(p.spacing[a]) &&
                  p.spacing[a] > 0,
              "pyramid grid origin/spacing");
  auto h = p.original_shape[p.y_axis], w = p.original_shape[p.x_axis];
  std::int64_t maximum = 0;
  while (h != 1 || w != 1) {
    h = (h + 1) / 2;
    w = (w + 1) / 2;
    ++maximum;
  }
  parameter(p.levels >= 0 && p.levels <= maximum, "pyramid levels after 1x1");
}
std::vector<std::uint64_t> level_shape(const PyramidOptions& p,
                                       unsigned level) {
  auto shape = p.original_shape;
  for (unsigned i = 0; i < level; ++i) {
    shape[p.y_axis] = (shape[p.y_axis] + 1) / 2;
    shape[p.x_axis] = (shape[p.x_axis] + 1) / 2;
  }
  return shape;
}
std::string shape_text(const std::vector<std::uint64_t>& shape) {
  std::string result;
  for (auto x : shape) {
    if (!result.empty())
      result += ',';
    result += std::to_string(x);
  }
  return result;
}
std::string dtype_text(const PyramidOptions& p) {
  return p.dtype == ElementType::Float32 ? "float32" : "float64";
}
WorkflowNodeOutput append(WorkflowDocument& d, const std::string& op,
                          std::vector<WorkflowInput> inputs, Parameters p,
                          const std::string& port) {
  const auto ids = take(numeric::available_workflow_node_ids(d, 1, inputs));
  d.nodes.push_back({ids[0], op, std::move(inputs), std::move(p)});
  return {ids[0], port};
}
WorkflowNodeOutput selected(WorkflowDocument& d, WorkflowInput value,
                            const std::string& member) {
  return append(d, "filter.select_band", {value}, {{"band", member}}, "output");
}
PyramidBand metadata(const PyramidOptions& p, unsigned l,
                     const std::string& role, WorkflowInput value) {
  PyramidBand b;
  b.band_id = role == "base" ? "base" : role + "_" + std::to_string(l);
  b.source_grid = p.source_grid;
  b.role = role;
  b.level = l;
  b.dtype = p.dtype;
  b.original_shape = p.original_shape;
  b.shape = level_shape(p, l);
  for (auto n : b.shape)
    b.valid_crop.push_back({0, n});
  b.sampling_step = {UINT64_C(1) << l, UINT64_C(1) << l};
  b.origin = p.origin;
  b.spacing = {std::ldexp(p.spacing[0], static_cast<int>(l)),
               std::ldexp(p.spacing[1], static_cast<int>(l))};
  for (auto step : b.spacing)
    parameter(std::isfinite(step), "pyramid spacing overflow");
  b.analysis_profile = kProfile;
  b.synthesis_profile = kProfile;
  b.rounding_profile = kRounding;
  b.padding_history = kPadding;
  b.value = std::move(value);
  return b;
}
Result<PyramidBands> analysis(WorkflowDocument& document, WorkflowInput input,
                              const PyramidOptions& p, bool laplacian) {
  try {
    validate(p);
    auto staged = document;
    PyramidBands result{"FilterBands/v1",
                        laplacian ? "laplacian" : "gaussian",
                        p,
                        {}};
    unsigned first = 0;
    do {
      const unsigned count =
          std::min<unsigned>(15, static_cast<unsigned>(p.levels) - first);
      const auto block =
          append(staged,
                 std::string("filter.") +
                     (laplacian ? "laplacian_pyramid_" : "gaussian_pyramid_") +
                     p.profile,
                 {input},
                 {{"y_axis", p.y_axis},
                  {"x_axis", p.x_axis},
                  {"levels", static_cast<std::int64_t>(count)},
                  {"expected_shape", shape_text(level_shape(p, first))},
                  {"expected_dtype", dtype_text(p)}},
                 "bands");
      if (laplacian) {
        for (unsigned i = 0; i < count; ++i) {
          const auto value =
              selected(staged, block, "detail_" + std::to_string(i));
          result.bands.push_back(metadata(p, first + i, "detail", value));
        }
        input = selected(staged, block, "base");
        if (first + count == static_cast<unsigned>(p.levels))
          result.bands.push_back(metadata(p, first + count, "base", input));
      } else {
        for (unsigned i = first ? 1 : 0; i <= count; ++i) {
          const auto value =
              selected(staged, block, "gaussian_" + std::to_string(i));
          result.bands.push_back(metadata(p, first + i, "gaussian", value));
        }
        input = result.bands.back().value;
      }
      first += count;
    } while (first < static_cast<unsigned>(p.levels));
    document = std::move(staged);
    return Result<PyramidBands>(std::move(result));
  } catch (const Status& e) {
    return Result<PyramidBands>(e);
  }
}
}  // namespace
Result<PyramidBands> gaussian_pyramid(WorkflowDocument& document,
                                      WorkflowInput input,
                                      const PyramidOptions& options) {
  return analysis(document, std::move(input), options, false);
}
Result<PyramidBands> laplacian_pyramid(WorkflowDocument& document,
                                       WorkflowInput input,
                                       const PyramidOptions& options) {
  return analysis(document, std::move(input), options, true);
}
Result<WorkflowNodeOutput> reconstruct_laplacian(
    WorkflowDocument& document, const PyramidBands& collection) {
  try {
    const auto& p = collection.options;
    validate(p);
    parameter(
        collection.schema == "FilterBands/v1" && collection.kind == "laplacian",
        "Laplacian collection schema/kind");
    parameter(collection.bands.size() == static_cast<std::size_t>(p.levels) + 1,
              "Laplacian complete directory");
    std::set<std::string> ids;
    std::vector<const PyramidBand*> by_level(collection.bands.size(), nullptr);
    for (const auto& b : collection.bands) {
      parameter(!b.band_id.empty() && ids.insert(b.band_id).second &&
                    b.level <= static_cast<std::uint64_t>(p.levels) &&
                    !by_level[b.level],
                "Laplacian duplicate ID/level");
      const auto expected = metadata(
          p, static_cast<unsigned>(b.level),
          b.level == static_cast<std::uint64_t>(p.levels) ? "base" : "detail",
          b.value);
      require(b.source_grid == expected.source_grid &&
                  b.role == expected.role && b.dtype == expected.dtype &&
                  b.original_shape == expected.original_shape &&
                  b.shape == expected.shape &&
                  b.valid_crop == expected.valid_crop &&
                  b.sampling_step == expected.sampling_step &&
                  b.origin == expected.origin &&
                  b.spacing == expected.spacing &&
                  b.analysis_profile == expected.analysis_profile &&
                  b.synthesis_profile == expected.synthesis_profile &&
                  b.rounding_profile == expected.rounding_profile &&
                  b.padding_history == expected.padding_history,
              "Laplacian band grid/shape/phase/profile mismatch");
      by_level[b.level] = &b;
    }
    std::vector<WorkflowInput> references;
    references.reserve(by_level.size());
    for (const auto* band : by_level)
      references.push_back(band->value);
    // Reserve every coefficient producer before allocating any block nodes.
    const unsigned blocks =
        std::max(1U, (static_cast<unsigned>(p.levels) + 14) / 15);
    const auto node_ids = take(
        numeric::available_workflow_node_ids(document, 2 * blocks, references));
    unsigned next_id = 0;
    auto staged = document;
    unsigned end = static_cast<unsigned>(p.levels);
    WorkflowInput current = by_level[end]->value;
    WorkflowNodeOutput output;
    do {
      const unsigned first = end > 15 ? end - 15 : 0;
      std::vector<WorkflowInput> inputs;
      for (unsigned l = first; l < end; ++l)
        inputs.push_back(by_level[l]->value);
      inputs.push_back(current);
      Parameters parameters{
          {"y_axis", p.y_axis},
          {"x_axis", p.x_axis},
          {"expected_shape", shape_text(level_shape(p, first))},
          {"expected_dtype", dtype_text(p)}};
      const WorkflowNodeOutput packed{node_ids[next_id++], "bands"};
      staged.nodes.push_back({packed.source_node,
                              "filter.assemble_laplacian_bands",
                              std::move(inputs), parameters});
      output = {node_ids[next_id++], "output"};
      staged.nodes.push_back({output.source_node,
                              "filter.reconstruct_laplacian_" + p.profile,
                              {packed},
                              std::move(parameters)});
      current = output;
      end = first;
    } while (end);
    document = std::move(staged);
    return Result<WorkflowNodeOutput>(std::move(output));
  } catch (const Status& e) {
    return Result<WorkflowNodeOutput>(e);
  }
}
}  // namespace ps::filter
