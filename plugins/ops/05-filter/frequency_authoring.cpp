#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "05-filter/frequency.hpp"
#include "photospider/ops/frequency.hpp"
#include "photospider/ops/numeric/workflow_authoring.hpp"

namespace ps::frequency {
namespace {
using plugin_internal::frequency::parameter;
using plugin_internal::frequency::take;
using Parameters = std::map<std::string, ParameterValue>;
std::string list(const std::vector<std::uint64_t>& values) {
  std::string result;
  for (auto v : values) {
    if (!result.empty())
      result += ',';
    result += std::to_string(v);
  }
  return result;
}
Parameters parameters(const Grid& g) {
  Parameters p{{"schema_revision", INT64_C(1)},
               {"original_shape", list(g.original_shape)},
               {"y_axis", g.y_axis},
               {"x_axis", g.x_axis},
               {"packing", g.packing},
               {"index_order", g.index_order},
               {"norm", g.norm},
               {"unit", g.unit},
               {"grid_id", g.identity},
               {"spacing_y", g.spacing[0]},
               {"spacing_x", g.spacing[1]},
               {"origin_y", g.origin[0]},
               {"origin_x", g.origin[1]}};
  const plugin_internal::frequency::Grid checked(p);
  return p;
}
std::string axes(std::vector<std::int64_t> values, const Grid& g,
                 bool spatial) {
  std::sort(values.begin(), values.end());
  std::vector<std::uint64_t> selected;
  for (auto a : values) {
    parameter(a >= 0 &&
                  static_cast<std::uint64_t>(a) < g.original_shape.size() &&
                  (selected.empty() ||
                   selected.back() != static_cast<std::uint64_t>(a)),
              "frequency authoring axes");
    if (!spatial)
      parameter(a != g.y_axis && a != g.x_axis, "frequency broadcast axes");
    selected.push_back(a);
  }
  return list(selected);
}
std::vector<WorkflowNodeOutput> append(
    WorkflowDocument& document, const std::string& member,
    const std::string& profile, const std::vector<WorkflowInput>& inputs,
    Parameters p, const std::vector<std::string>& outputs) {
  parameter(profile == "strict" || profile == "accelerated_apple_silicon" ||
                profile == "accelerated_x86_64",
            "frequency authoring profile");
  const auto ids =
      take(numeric::available_workflow_node_ids(document, 1, inputs));
  auto staged = document;
  staged.nodes.push_back(
      {ids[0], "frequency." + member + '_' + profile, inputs, std::move(p)});
  std::vector<WorkflowNodeOutput> result;
  for (const auto& name : outputs)
    result.push_back({ids[0], name});
  document = std::move(staged);
  return result;
}
}  // namespace
Result<std::vector<WorkflowNodeOutput>> transform(
    WorkflowDocument& document, const std::string& member,
    const std::vector<WorkflowInput>& inputs, const Grid& grid) {
  try {
    parameter(member == "fft2" || member == "ifft2" || member == "rfft2" ||
                  member == "irfft2",
              "frequency transform member");
    parameter(inputs.size() == (member == "rfft2" ? 1U : 2U),
              "frequency transform input arity");
    parameter(
        grid.index_order == "unshifted" &&
            grid.packing ==
                (member == "rfft2" || member == "irfft2" ? "r2c_x" : "full"),
        "frequency transform packing/order");
    const std::vector<std::string> out =
        member == "rfft2"    ? std::vector<std::string>{"real", "imag"}
        : member == "irfft2" ? std::vector<std::string>{"output"}
                             : std::vector<std::string>{"out_real", "out_imag"};
    auto p = parameters(grid);
    const auto ids =
        take(numeric::available_workflow_node_ids(document, 1, inputs));
    auto staged = document;
    staged.nodes.push_back(
        {ids[0], "frequency." + member, inputs, std::move(p)});
    std::vector<WorkflowNodeOutput> result;
    for (const auto& name : out)
      result.push_back({ids[0], name});
    document = std::move(staged);
    return Result<std::vector<WorkflowNodeOutput>>(std::move(result));
  } catch (const Status& s) {
    return Result<std::vector<WorkflowNodeOutput>>(s);
  }
}
Result<Spectrum> shift(WorkflowDocument& document, const Spectrum& input,
                       const std::vector<std::int64_t>& selected,
                       bool inverse) {
  try {
    auto p = parameters(input.grid);
    parameter(
        input.grid.packing == "full" &&
            input.grid.index_order == (inverse ? "shifted" : "unshifted") &&
            !selected.empty(),
        "frequency shift grid");
    p["axes"] = axes(selected, input.grid, true);
    const auto out = append(document, inverse ? "ifftshift" : "fftshift",
                            "strict", {input.real, input.imag}, std::move(p),
                            {"out_real", "out_imag"});
    auto grid = input.grid;
    grid.index_order = inverse ? "unshifted" : "shifted";
    return Result<Spectrum>(Spectrum{out[0], out[1], std::move(grid)});
  } catch (const Status& s) {
    return Result<Spectrum>(s);
  }
}
Result<Spectrum> project_hermitian(WorkflowDocument& document,
                                   const Spectrum& input,
                                   const std::string& profile) {
  try {
    auto p = parameters(input.grid);
    parameter(
        input.grid.packing == "full" && input.grid.index_order == "unshifted",
        "frequency projection grid");
    const auto out =
        append(document, "project_hermitian", profile, {input.real, input.imag},
               std::move(p), {"out_real", "out_imag"});
    return Result<Spectrum>(Spectrum{out[0], out[1], input.grid});
  } catch (const Status& s) {
    return Result<Spectrum>(s);
  }
}
Result<Spectrum> multiply(WorkflowDocument& document, const Spectrum& input,
                          const Spectrum& response,
                          const std::vector<std::int64_t>& broadcast_axes,
                          const std::string& response_unit,
                          const std::string& profile) {
  try {
    auto p = parameters(input.grid);
    parameters(response.grid);
    parameter(input.grid.y_axis == response.grid.y_axis &&
                  input.grid.x_axis == response.grid.x_axis &&
                  input.grid.packing == response.grid.packing &&
                  input.grid.index_order == response.grid.index_order &&
                  input.grid.original_shape.size() ==
                      response.grid.original_shape.size(),
              "frequency multiplication grid structure");
    p["broadcast_axes"] = axes(broadcast_axes, input.grid, false);
    for (unsigned a = 0; a < input.grid.original_shape.size(); ++a) {
      const bool broadcast =
          std::find(broadcast_axes.begin(), broadcast_axes.end(),
                    static_cast<std::int64_t>(a)) != broadcast_axes.end();
      parameter(response.grid.original_shape[a] ==
                    (broadcast ? 1 : input.grid.original_shape[a]),
                "frequency multiplication original shape");
    }
    parameter(response_unit == "dimensionless", "frequency response unit");
    p["response_unit"] = response_unit;
    const auto out =
        append(document, "frequency_multiply", profile,
               {input.real, input.imag, response.real, response.imag},
               std::move(p), {"out_real", "out_imag"});
    return Result<Spectrum>(Spectrum{out[0], out[1], input.grid});
  } catch (const Status& s) {
    return Result<Spectrum>(s);
  }
}
}  // namespace ps::frequency
