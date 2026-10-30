// Copyright 2026 Photospider contributors
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "05-filter/frequency_native.hpp"
#include "photospider/ops/frequency.hpp"
#include "photospider/ops/numeric/workflow_authoring.hpp"
#include "photospider/ops/restoration.hpp"

namespace ps::frequency {
namespace {
using plugin_internal::frequency::parameter;
using plugin_internal::frequency::require;
using plugin_internal::frequency::take;
using Parameters = std::map<std::string, ParameterValue>;
WorkflowNodeOutput append(WorkflowDocument& d, const std::string& op,
                          std::vector<WorkflowInput> in, Parameters p,
                          const std::string& port) {
  const auto ids = take(numeric::available_workflow_node_ids(d, 1, in));
  d.nodes.push_back({ids[0], "frequency." + op, std::move(in), std::move(p)});
  return {ids[0], port};
}
void validate(const WaveletOptions& p) {
  plugin_internal::generation::size_product(p.original_shape);
  parameter(p.original_shape.size() >= 2 && p.y_axis >= 0 && p.x_axis >= 0 &&
                p.y_axis != p.x_axis &&
                static_cast<std::uint64_t>(std::max(p.y_axis, p.x_axis)) <
                    p.original_shape.size(),
            "wavelet axes");
  parameter(p.dtype == ElementType::Float32 || p.dtype == ElementType::Float64,
            "wavelet dtype");
  parameter(p.profile == "haar_mean_lifting_v1" ||
                p.profile == "cdf53_float_lifting_v1" ||
                p.profile == "stationary_haar_mean_v1",
            "wavelet named profile");
  parameter(!p.source_grid.empty() && !p.unit.empty() && p.levels >= 0,
            "wavelet grid/unit/levels");
  auto h = p.original_shape[p.y_axis], w = p.original_shape[p.x_axis];
  std::int64_t maximum = 0;
  if (p.profile == "stationary_haar_mean_v1") {
    std::uint64_t limit = 1;
    while (limit < std::max(h, w)) {
      limit *= 2;
    }
    for (auto n = limit; n; n >>= 1) {
      ++maximum;
    }
  } else {
    while (p.profile == "cdf53_float_lifting_v1" ? h >= 2 && w >= 2
                                                 : h > 1 || w > 1) {
      h = (h + 1) / 2;
      w = (w + 1) / 2;
      ++maximum;
    }
  }
  parameter(p.levels <= maximum, "wavelet excess levels");
}
std::vector<std::uint64_t> shape(const WaveletOptions& p, unsigned l) {
  auto d = p.original_shape;
  if (p.profile != "stationary_haar_mean_v1") {
    for (unsigned i = 0; i < l; ++i) {
      d[p.y_axis] = (d[p.y_axis] + 1) / 2;
      d[p.x_axis] = (d[p.x_axis] + 1) / 2;
    }
  }
  return d;
}
WaveletBand metadata(const WaveletOptions& p, unsigned l,
                     const std::string& orientation, WorkflowInput value) {
  WaveletBand b;
  b.level = l;
  b.orientation = orientation;
  b.role = orientation == "LL" ? "residual" : "detail";
  b.band_id = orientation + "_" + std::to_string(l);
  b.shape = shape(p, l);
  b.original_shape = p.original_shape;
  if (p.profile == "cdf53_float_lifting_v1" && orientation != "LL") {
    const auto prev = shape(p, l - 1);
    if (orientation != "HL_x") {
      b.shape[p.y_axis] = prev[p.y_axis] / 2;
    }
    if (orientation != "LH_y") {
      b.shape[p.x_axis] = prev[p.x_axis] / 2;
    }
  }
  for (auto n : b.shape) {
    b.valid_crop.push_back({0, n});
  }
  const auto step =
      p.profile == "stationary_haar_mean_v1" ? 1 : UINT64_C(1) << l;
  b.sampling_step = {step, step};
  b.boundary = p.profile == "stationary_haar_mean_v1" ? "wrap"
               : p.profile == "haar_mean_lifting_v1"
                   ? "duplicate_last"
                   : "replicate_even_detail_endpoints";
  b.phase = p.profile == "stationary_haar_mean_v1" ? "positive_dyadic_offset"
                                                   : "even_origin";
  b.axis_order = "analysis_x_y_synthesis_y_x";
  b.analysis_profile = b.synthesis_profile = p.profile;
  b.rounding_profile = "native_float_v1";
  b.value = std::move(value);
  return b;
}
bool matches(const WaveletBand& a, const WaveletBand& b) {
  return a.band_id == b.band_id && a.level == b.level && a.role == b.role &&
         a.orientation == b.orientation && a.shape == b.shape &&
         a.original_shape == b.original_shape &&
         a.valid_crop.size() == b.valid_crop.size() &&
         std::equal(a.valid_crop.begin(), a.valid_crop.end(),
                    b.valid_crop.begin(),
                    [](auto x, auto y) {
                      return x.offset == y.offset && x.extent == y.extent;
                    }) &&
         a.sampling_step == b.sampling_step && a.boundary == b.boundary &&
         a.phase == b.phase && a.axis_order == b.axis_order &&
         a.analysis_profile == b.analysis_profile &&
         a.synthesis_profile == b.synthesis_profile &&
         a.rounding_profile == b.rounding_profile;
}
Parameters wave_parameters(const WaveletOptions& p, unsigned first,
                           unsigned count) {
  std::string dimensions;
  for (auto n : shape(p, first)) {
    if (!dimensions.empty()) {
      dimensions += ',';
    }
    dimensions += std::to_string(n);
  }
  return {
      {"y_axis", p.y_axis},
      {"x_axis", p.x_axis},
      {"levels", static_cast<std::int64_t>(count)},
      {"first_level", static_cast<std::int64_t>(
                          p.profile == "stationary_haar_mean_v1" ? first : 0)},
      {"expected_shape", dimensions},
      {"expected_dtype",
       std::string(p.dtype == ElementType::Float32 ? "float32" : "float64")}};
}
std::map<std::string, const WaveletBand*> directory(
    const WaveletBands& collection) {
  const auto& p = collection.options;
  validate(p);
  parameter(
      collection.schema == "FilterBands/v1" &&
          collection.bands.size() == static_cast<std::size_t>(3 * p.levels + 1),
      "wavelet complete directory");
  std::map<std::string, const WaveletBand*> result;
  for (const auto& b : collection.bands) {
    parameter(b.level <= static_cast<std::uint64_t>(p.levels) &&
                  (b.orientation == "LL"
                       ? b.level == static_cast<std::uint64_t>(p.levels)
                       : b.level >= 1),
              "wavelet band level");
    parameter(b.orientation == "LL" || b.orientation == "LH_y" ||
                  b.orientation == "HL_x" || b.orientation == "HH",
              "wavelet orientation");
    require(matches(b, metadata(p, b.level, b.orientation, b.value)),
            "wavelet static metadata mismatch");
    parameter(result.emplace(b.band_id, &b).second,
              "wavelet duplicate band ID");
  }
  return result;
}
}  // namespace
Result<WorkflowNodeOutput> convolve(WorkflowDocument& document,
                                    const std::string& member,
                                    WorkflowInput input, WorkflowInput kernel,
                                    const ConvolutionOptions& o) {
  try {
    parameter(member == "linear_fft_convolution" ||
                  member == "circular_fft_convolution" ||
                  member == "overlap_add" || member == "overlap_save",
              "convolution member");
    Parameters p{{"y_axis", o.y_axis},
                 {"x_axis", o.x_axis},
                 {"anchor_y", o.anchor[0]},
                 {"anchor_x", o.anchor[1]},
                 {"normalization", o.normalization}};
    if (member != "circular_fft_convolution") {
      p["shape"] = o.shape;
    }
    if (member == "linear_fft_convolution") {
      p["fft_padding_height"] = o.fft_shape[0];
      p["fft_padding_width"] = o.fft_shape[1];
    } else if (member != "circular_fft_convolution") {
      p["fft_height"] = o.fft_shape[0];
      p["fft_width"] = o.fft_shape[1];
      p["block_payload_height"] = o.payload_shape[0];
      p["block_payload_width"] = o.payload_shape[1];
    }
    auto staged = document;
    const auto out =
        append(staged, member, {input, kernel}, std::move(p), "output");
    document = std::move(staged);
    return Result<WorkflowNodeOutput>(out);
  } catch (const Status& s) {
    return Result<WorkflowNodeOutput>(s);
  }
}
Result<WorkflowNodeOutput> dct(WorkflowDocument& document, WorkflowInput input,
                               std::int64_t y, std::int64_t x,
                               const std::string& type, const std::string& norm,
                               bool inverse) {
  try {
    parameter(type == "I" || type == "II" || type == "III" || type == "IV",
              "DCT type");
    parameter(norm == "backward" || norm == "ortho", "DCT normalization");
    auto staged = document;
    const auto out =
        append(staged, inverse ? "idct2" : "dct2", {input},
               {{"y_axis", y}, {"x_axis", x}, {"type", type}, {"norm", norm}},
               "output");
    document = std::move(staged);
    return Result<WorkflowNodeOutput>(out);
  } catch (const Status& s) {
    return Result<WorkflowNodeOutput>(s);
  }
}
Result<WaveletBands> decompose_wavelet(WorkflowDocument& document,
                                       WorkflowInput input,
                                       const WaveletOptions& p) {
  try {
    validate(p);
    auto staged = document;
    WaveletBands result{"FilterBands/v1", p, {}};
    unsigned first = 0;
    do {
      const auto count =
          std::min<unsigned>(5, static_cast<unsigned>(p.levels) - first);
      const auto packed = append(staged, "decompose_" + p.profile, {input},
                                 wave_parameters(p, first, count), "bands");
      for (unsigned i = 0; i < count; ++i) {
        for (const auto* orientation : {"LH_y", "HL_x", "HH"}) {
          const auto value = append(staged, "select_" + p.profile, {packed},
                                    {{"band", std::string(orientation) + "_" +
                                                  std::to_string(i + 1)}},
                                    "output");
          result.bands.push_back(
              metadata(p, first + i + 1, orientation, value));
        }
      }
      input = append(staged, "select_" + p.profile, {packed},
                     {{"band", std::string("LL")}}, "output");
      first += count;
    } while (first < static_cast<unsigned>(p.levels));
    result.bands.push_back(metadata(p, first, "LL", input));
    document = std::move(staged);
    return Result<WaveletBands>(std::move(result));
  } catch (const Status& s) {
    return Result<WaveletBands>(s);
  }
}
Result<WorkflowNodeOutput> inverse_wavelet(WorkflowDocument& document,
                                           const WaveletBands& collection) {
  try {
    const auto& p = collection.options;
    const auto directory = ps::frequency::directory(collection);
    std::vector<WorkflowInput> references;
    for (const auto& b : collection.bands)
      references.push_back(b.value);
    auto staged = document;
    // Reserve all producer IDs even when edited bands originate outside nodes
    // already present in this document.
    const auto ids = take(numeric::available_workflow_node_ids(
        staged, 2 * std::max<std::int64_t>(1, (p.levels + 4) / 5), references));
    unsigned cursor = 0, end = p.levels;
    WorkflowInput current =
        directory.at("LL_" + std::to_string(p.levels))->value;
    WorkflowNodeOutput output;
    do {
      const unsigned first = end > 5 ? end - 5 : 0;
      std::vector<WorkflowInput> inputs;
      for (unsigned l = first + 1; l <= end; ++l) {
        for (const auto* orientation : {"LH_y", "HL_x", "HH"}) {
          inputs.push_back(
              directory.at(std::string(orientation) + "_" + std::to_string(l))
                  ->value);
        }
      }
      inputs.push_back(current);
      const WorkflowNodeOutput packed{ids[cursor++], "bands"};
      staged.nodes.push_back({packed.source_node,
                              "frequency.assemble_" + p.profile,
                              std::move(inputs),
                              {}});
      auto args = wave_parameters(p, first, end - first);
      const auto d = shape(p, first);
      args["original_height"] = static_cast<std::int64_t>(d[p.y_axis]);
      args["original_width"] = static_cast<std::int64_t>(d[p.x_axis]);
      output = {ids[cursor++], "output"};
      staged.nodes.push_back({output.source_node,
                              "frequency.inverse_" + p.profile,
                              {packed},
                              std::move(args)});
      current = output;
      end = first;
    } while (end);
    document = std::move(staged);
    return Result<WorkflowNodeOutput>(output);
  } catch (const Status& s) {
    return Result<WorkflowNodeOutput>(s);
  }
}
}  // namespace ps::frequency

namespace ps::restoration {
namespace {
using plugin_internal::frequency::parameter;
using plugin_internal::frequency::take;
WorkflowNodeOutput append(WorkflowDocument& d, const std::string& op,
                          std::vector<WorkflowInput> in,
                          std::map<std::string, ParameterValue> p,
                          const std::string& port = "output") {
  const auto ids = take(numeric::available_workflow_node_ids(d, 1, in));
  d.nodes.push_back({ids[0], op, std::move(in), std::move(p)});
  return {ids[0], port};
}
// Reserve producers that may not yet be part of the document. A temporary
// unconnected guard node participates only in ID allocation during authoring.
std::uint64_t reserve(WorkflowDocument& d,
                      const std::vector<WorkflowInput>& refs) {
  const auto id = take(numeric::available_workflow_node_ids(d, 1, refs))[0];
  d.nodes.push_back({id, "", refs, {}});
  return id;
}
}  // namespace
Result<frequency::WaveletBands> threshold_wavelet_details(
    WorkflowDocument& document, const frequency::WaveletBands& bands,
    WorkflowInput thresholds, const std::string& mode) {
  try {
    const auto dir = frequency::directory(bands);
    const auto& p = bands.options;
    parameter(p.levels > 0, "threshold requires positive levels");
    parameter(mode == "hard" || mode == "soft", "threshold mode");
    auto staged = document;
    std::vector<WorkflowInput> refs{thresholds};
    for (const auto& b : bands.bands)
      refs.push_back(b.value);
    const auto guard = reserve(staged, refs);
    auto result = bands;
    for (unsigned first = 0; first < static_cast<unsigned>(p.levels);
         first += 5) {
      const unsigned end = std::min<unsigned>(first + 5, p.levels);
      std::vector<WorkflowInput> inputs;
      for (unsigned l = first + 1; l <= end; ++l)
        for (const auto* role : {"LH_y", "HL_x", "HH"})
          inputs.push_back(
              dir.at(std::string(role) + "_" + std::to_string(l))->value);
      WorkflowInput residual = dir.at("LL_" + std::to_string(p.levels))->value;
      if (end != static_cast<unsigned>(p.levels)) {
        // Reconstruct a structurally complete local directory. This residual
        // is not selected by threshold tasks, so its producer is demand-pruned.
        frequency::WaveletBands tail{"FilterBands/v1", p, {}};
        tail.options.original_shape = frequency::shape(p, end);
        tail.options.levels -= end;
        for (unsigned l = end + 1; l <= static_cast<unsigned>(p.levels); ++l)
          for (const auto* role : {"LH_y", "HL_x", "HH"})
            tail.bands.push_back(frequency::metadata(
                tail.options, l - end, role,
                dir.at(std::string(role) + "_" + std::to_string(l))->value));
        tail.bands.push_back(frequency::metadata(
            tail.options, tail.options.levels, "LL", residual));
        residual = take(frequency::inverse_wavelet(staged, tail));
      }
      inputs.push_back(residual);
      const auto packed = append(staged, "frequency.assemble_" + p.profile,
                                 std::move(inputs), {}, "bands");
      const auto edited = append(
          staged, "restoration.threshold_wavelet_details", {packed, thresholds},
          {{"mode", mode},
           {"first_level", static_cast<std::int64_t>(first)},
           {"total_levels", p.levels},
           {"y_axis", p.y_axis},
           {"x_axis", p.x_axis},
           {"original_height",
            static_cast<std::int64_t>(frequency::shape(p, first)[p.y_axis])},
           {"original_width",
            static_cast<std::int64_t>(frequency::shape(p, first)[p.x_axis])}},
          "bands");
      for (auto& b : result.bands) {
        if (b.orientation == "LL" || b.level <= first || b.level > end)
          continue;
        b.value = append(
            staged, "frequency.select_" + p.profile, {edited},
            {{"band", b.orientation + "_" + std::to_string(b.level - first)}});
      }
    }
    staged.nodes.erase(
        std::find_if(staged.nodes.begin(), staged.nodes.end(),
                     [guard](const auto& n) { return n.id == guard; }));
    document = std::move(staged);
    return Result<frequency::WaveletBands>(std::move(result));
  } catch (const Status& s) {
    return Result<frequency::WaveletBands>(s);
  }
}
Result<WorkflowInput> cycle_spin_wavelet_denoise(
    WorkflowDocument& document, WorkflowInput input, WorkflowInput thresholds,
    const frequency::WaveletOptions& p,
    const std::vector<std::array<std::int64_t, 2>>& shifts,
    const std::string& mode) {
  try {
    frequency::validate(p);
    parameter(p.profile == "haar_mean_lifting_v1" ||
                  p.profile == "cdf53_float_lifting_v1",
              "cycle spin decimated profile");
    parameter(!shifts.empty() && shifts.size() <= 32768,
              "cycle spin shift graph capacity");
    parameter(mode == "hard" || mode == "soft", "cycle spin threshold mode");
    if (!p.levels)
      return Result<WorkflowInput>(input);
    auto staged = document;
    const auto guard = reserve(staged, {input, thresholds});
    std::string shape;
    for (auto n : p.original_shape) {
      if (!shape.empty())
        shape += ',';
      shape += std::to_string(n);
    }
    const auto permute = [&](WorkflowInput value, std::array<std::int64_t, 2> s,
                             bool inverse) {
      // Reduction before negation also handles INT64_MIN without overflow.
      for (unsigned a = 0; a < 2; ++a) {
        const auto extent = p.original_shape[a == 0 ? p.y_axis : p.x_axis];
        auto v = static_cast<__int128>(s[a]) % extent;
        if (inverse)
          v = -v;
        s[a] = static_cast<std::int64_t>(v);
      }
      return append(
          staged, "restoration.wrap_shift", {value},
          {{"y_axis", p.y_axis},
           {"x_axis", p.x_axis},
           {"shift_y", s[0]},
           {"shift_x", s[1]},
           {"expected_shape", shape},
           {"expected_dtype",
            std::string(p.dtype == ElementType::Float32 ? "float32"
                                                        : "float64")}});
    };
    WorkflowInput state = input;
    for (std::size_t i = 0; i < shifts.size(); ++i) {
      auto bands = take(frequency::decompose_wavelet(
          staged, permute(input, shifts[i], false), p));
      bands = take(threshold_wavelet_details(staged, bands, thresholds, mode));
      const auto branch = permute(
          take(frequency::inverse_wavelet(staged, bands)), shifts[i], true);
      state =
          i == 0 ? WorkflowInput(branch)
                 : WorkflowInput(append(
                       staged, "restoration.accumulate_native", {state, branch},
                       {{"divisor", i + 1 == shifts.size()
                                        ? static_cast<double>(shifts.size())
                                        : 1.0}}));
    }
    staged.nodes.erase(
        std::find_if(staged.nodes.begin(), staged.nodes.end(),
                     [guard](const auto& n) { return n.id == guard; }));
    document = std::move(staged);
    return Result<WorkflowInput>(std::move(state));
  } catch (const Status& s) {
    return Result<WorkflowInput>(s);
  }
}
}  // namespace ps::restoration
