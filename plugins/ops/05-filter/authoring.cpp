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
using plugin_internal::filter::take;
void axes(std::int64_t y, std::int64_t x) {
  parameter(y >= 0 && x >= 0 && y != x && y < 8 && x < 8,
            "filter authoring spatial axes");
}
}  // namespace
Result<WorkflowNodeOutput> canny_quantized4(WorkflowDocument& document,
                                            WorkflowInput input,
                                            const CannyOptions& p) {
  try {
    axes(p.y_axis, p.x_axis);
    parameter(p.norm == "l1" || p.norm == "l2", "Canny magnitude norm");
    parameter(p.connectivity == 4 || p.connectivity == 8, "Canny connectivity");
    plugin_internal::filter::radius_extent(p.radius_y);
    plugin_internal::filter::radius_extent(p.radius_x);
    const auto ids =
        take(numeric::available_workflow_node_ids(document, 5, {input}));
    auto staged = document;
    staged.nodes.push_back({ids[0],
                            "filter.gaussian_filter_strict",
                            {input},
                            {{"y_axis", p.y_axis},
                             {"x_axis", p.x_axis},
                             {"sigma_y", p.sigma_y},
                             {"sigma_x", p.sigma_x},
                             {"radius_y", p.radius_y},
                             {"radius_x", p.radius_x},
                             {"boundary", std::string("reflect_half")},
                             {"cval", 0.0}}});
    staged.nodes.push_back({ids[1],
                            "filter.sobel_gradient_strict",
                            {WorkflowNodeOutput{ids[0], "output"}},
                            {{"y_axis", p.y_axis},
                             {"x_axis", p.x_axis},
                             {"spacing_y", 1.0},
                             {"spacing_x", 1.0},
                             {"normalization", std::string("unit_ramp")}}});
    staged.nodes.push_back(
        {ids[2],
         "filter.gradient_magnitude_strict",
         {WorkflowNodeOutput{ids[1], "gx"}, WorkflowNodeOutput{ids[1], "gy"}},
         {{"norm", p.norm}}});
    staged.nodes.push_back(
        {ids[3],
         "filter.canny_nms_quantized4_strict",
         {WorkflowNodeOutput{ids[1], "gx"}, WorkflowNodeOutput{ids[1], "gy"},
          WorkflowNodeOutput{ids[2], "magnitude"}},
         {{"y_axis", p.y_axis},
          {"x_axis", p.x_axis},
          {"low", p.low},
          {"high", p.high}}});
    staged.nodes.push_back({ids[4],
                            "filter.hysteresis_edges_strict",
                            {WorkflowNodeOutput{ids[3], "weak"},
                             WorkflowNodeOutput{ids[3], "strong"}},
                            {{"y_axis", p.y_axis},
                             {"x_axis", p.x_axis},
                             {"connectivity", p.connectivity}}});
    document = std::move(staged);
    return Result<WorkflowNodeOutput>(WorkflowNodeOutput{ids[4], "edges"});
  } catch (const Status& e) {
    return Result<WorkflowNodeOutput>(e);
  }
}
Result<std::vector<GaborBand>> gabor_bank(
    WorkflowDocument& document, WorkflowInput input, std::int64_t y,
    std::int64_t x, const std::vector<GaborConfiguration>& configurations) {
  try {
    axes(y, x);
    parameter(!configurations.empty() && configurations.size() <= 32768,
              "Gabor bank graph capacity");
    std::set<std::string> names;
    for (const auto& c : configurations) {
      parameter(!c.bank_id.empty() && names.insert(c.bank_id).second,
                "Gabor bank IDs must be nonempty and unique");
      plugin_internal::filter::radius_extent(c.radius_y);
      plugin_internal::filter::radius_extent(c.radius_x);
      parameter(c.dc == "keep" || c.dc == "remove", "Gabor dc policy");
      parameter(c.normalization == "none" || c.normalization == "l1_envelope",
                "Gabor normalization");
    }
    const auto ids = take(numeric::available_workflow_node_ids(
        document, static_cast<unsigned>(2 * configurations.size()), {input}));
    auto staged = document;
    std::vector<GaborBand> result;
    for (unsigned i = 0; i < configurations.size(); ++i) {
      const auto& c = configurations[i];
      staged.nodes.push_back({ids[2 * i],
                              "filter.gabor_kernel_strict",
                              {},
                              {{"frequency", c.frequency},
                               {"theta", c.theta},
                               {"phase", c.phase},
                               {"sigma", c.sigma},
                               {"aspect", c.aspect},
                               {"radius_y", c.radius_y},
                               {"radius_x", c.radius_x},
                               {"dc", c.dc},
                               {"normalization", c.normalization}}});
      staged.nodes.push_back(
          {ids[2 * i + 1],
           "filter.gabor_response_strict",
           {input, WorkflowNodeOutput{ids[2 * i], "kernel_real"},
            WorkflowNodeOutput{ids[2 * i], "kernel_imag"}},
           {{"y_axis", y},
            {"x_axis", x},
            {"anchor_y", c.radius_y},
            {"anchor_x", c.radius_x}}});
      result.push_back({c.bank_id,
                        {ids[2 * i + 1], "response_real"},
                        {ids[2 * i + 1], "response_imag"}});
    }
    document = std::move(staged);
    return Result<std::vector<GaborBand>>(std::move(result));
  } catch (const Status& e) {
    return Result<std::vector<GaborBand>>(e);
  }
}
Result<WorkflowNodeOutput> select_band(WorkflowDocument& document,
                                       WorkflowInput bands,
                                       const std::string& band) {
  try {
    parameter(!band.empty(), "empty band selector");
    const auto ids =
        take(numeric::available_workflow_node_ids(document, 1, {bands}));
    auto staged = document;
    staged.nodes.push_back(
        {ids[0], "filter.select_band", {bands}, {{"band", band}}});
    document = std::move(staged);
    return Result<WorkflowNodeOutput>(WorkflowNodeOutput{ids[0], "output"});
  } catch (const Status& e) {
    return Result<WorkflowNodeOutput>(e);
  }
}

Result<GaborBank> gabor_bank(
    WorkflowDocument& document, WorkflowInput input, const FilterGrid& grid,
    const std::vector<GaborConfiguration>& configurations,
    const std::string& profile) {
  try {
    axes(grid.y_axis, grid.x_axis);
    plugin_internal::generation::size_product(grid.original_shape);
    parameter(
        grid.original_shape.size() >= 2 &&
            static_cast<std::size_t>(grid.y_axis) <
                grid.original_shape.size() &&
            static_cast<std::size_t>(grid.x_axis) < grid.original_shape.size(),
        "Gabor grid shape/axes");
    parameter(grid.dtype == ElementType::Float32 ||
                  grid.dtype == ElementType::Float64,
              "Gabor grid dtype");
    parameter(!grid.source_grid.empty() && !grid.unit.empty(),
              "Gabor grid identity/unit");
    for (unsigned a = 0; a < 2; ++a)
      parameter(std::isfinite(grid.origin[a]) &&
                    std::isfinite(grid.spacing[a]) && grid.spacing[a] > 0,
                "Gabor grid origin/spacing");
    parameter(profile == "strict" || profile == "accelerated_apple_silicon" ||
                  profile == "accelerated_x86_64",
              "Gabor backend profile");
    auto staged = document;
    const auto first = staged.nodes.size();
    auto references = take(
        gabor_bank(staged, input, grid.y_axis, grid.x_axis, configurations));
    std::string shape;
    for (auto n : grid.original_shape) {
      if (!shape.empty())
        shape += ',';
      shape += std::to_string(n);
    }
    for (std::size_t i = first; i < staged.nodes.size(); ++i) {
      auto& node = staged.nodes[i];
      const bool response = (i - first) & 1;
      node.operation = std::string(response ? "filter.gabor_response_"
                                            : "filter.gabor_kernel_") +
                       profile;
      if (response) {
        node.parameters["expected_shape"] = shape;
        node.parameters["expected_dtype"] = std::string(
            grid.dtype == ElementType::Float32 ? "float32" : "float64");
      }
    }
    GaborBank result{"FilterBands/v1", profile, grid, {}};
    for (std::size_t i = 0; i < references.size(); ++i) {
      GaborBankEntry b;
      b.configuration = configurations[i];
      b.response = references[i];
      b.grid = grid;
      for (auto n : grid.original_shape)
        b.valid_crop.push_back({0, n});
      b.sampling_step = {1, 1};
      b.real_role = "response_real";
      b.imaginary_role = "response_imag";
      b.analysis_profile = "gabor_v1";
      b.rounding_profile = "RN64_kernel_then_RN_t_correlation";
      b.padding_history = "reflect_half_no_padding";
      result.bands.push_back(std::move(b));
    }
    document = std::move(staged);
    return Result<GaborBank>(std::move(result));
  } catch (const Status& e) {
    return Result<GaborBank>(e);
  }
}
}  // namespace ps::filter
