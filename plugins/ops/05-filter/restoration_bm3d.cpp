// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "04-mask-morphology/common.hpp"
#include "05-filter/restoration_bm3d_math.hpp"
#include "common/registry.hpp"
#include "photospider/ops/numeric/workflow_authoring.hpp"
#include "photospider/ops/restoration.hpp"

namespace ps::plugin_internal::restoration {
using namespace generation;  // NOLINT(build/namespaces)
namespace {
bool power_of_two(std::int64_t v) {
  return v > 0 && (v & (v - 1)) == 0;
}
void validate_bm3d_options(const ps::restoration::Bm3dOptions& o) {
  mask::parameter(o.profile == "haar_opponent_v1", "BM3D profile");
  mask::parameter(o.y_axis >= 0 && o.x_axis >= 0 && o.y_axis != o.x_axis,
                  "BM3D spatial axes");
  mask::parameter(power_of_two(o.block_size) && o.block_size <= 32,
                  "BM3D block_size must be a power of two in 1..32");
  mask::parameter(power_of_two(o.group_size) && o.group_size <= 64,
                  "BM3D group_size must be a power of two in 1..64");
  mask::parameter(o.search_radius >= 0, "BM3D search_radius");
  mask::parameter(o.reference_step >= 1 && o.reference_step <= o.block_size,
                  "BM3D reference_step must be in 1..block_size");
}
ps::restoration::Bm3dOptions bm3d_options(const Parameters& p) {
  return {integer(p, "y_axis"),         integer(p, "x_axis"),
          text(p, "profile"),           integer(p, "block_size"),
          integer(p, "search_radius"),  integer(p, "group_size"),
          integer(p, "reference_step"), real(p, "match_hard"),
          real(p, "match_wiener"),      real(p, "hard_threshold"),
          real(p, "kaiser_beta")};
}
struct Bm3d final : Program {
  ElementType type;
  unsigned ya, xa;
  int ca = -1;
  std::array<unsigned, 3> channels{0, 1, 2};
  std::vector<std::uint64_t> dims;
  bm3d::Options options{};
  std::uint64_t admitted = 0;
  std::size_t count = 0, workspace = 0;

  Bm3d(bool color, const std::vector<OperationMetadata>& in,
       const Parameters& p) {
    const auto& input = tensor(in.at(0), true);
    const auto& sigma = tensor(in.at(1), true);
    type = input.descriptor.element_type;
    dims = input.sample_shape();
    require(floating(type) && input.batch_axes.empty() &&
                dims.size() == (color ? 3u : 2u),
            "BM3D requires one Float32/Float64 plane or RGB image");
    require(sigma.descriptor.element_type == ElementType::Float64 &&
                sigma.batch_axes.empty() &&
                sigma.sample_shape() == std::vector<std::uint64_t>{1},
            "BM3D sigma requires one Float64 scalar");
    const auto o = bm3d_options(p);
    validate_bm3d_options(o);
    mask::parameter(
        static_cast<std::uint64_t>(std::max(o.y_axis, o.x_axis)) < dims.size(),
        "BM3D spatial axes outside rank");
    ya = static_cast<unsigned>(o.y_axis);
    xa = static_cast<unsigned>(o.x_axis);
    require(dims[ya] && dims[xa], "BM3D requires nonempty spatial axes");
    if (color) {
      try {
        const auto selection =
            color_point::select(in[0], p, {"red", "green", "blue"});
        require(selection.semantic && selection.axis >= 0 &&
                    static_cast<unsigned>(selection.axis) != ya &&
                    static_cast<unsigned>(selection.axis) != xa &&
                    dims[selection.axis] == 3,
                "CBM3D requires a complete three-component semantic RGB image");
        require(
            selection.interpretation.model == "rgb" &&
                (!selection.interpretation.primaries.empty() ||
                 (selection.interpretation.primaries_xy &&
                  selection.interpretation.white)) &&
                !selection.interpretation.transfer.empty() &&
                (selection.interpretation.reference == "scene" ||
                 selection.interpretation.reference == "display"),
            "CBM3D requires explicit RGB primaries, transfer and reference");
        require(selection.description->groups.size() == 1 &&
                    !selection.description->groups[0].alpha,
                "CBM3D requires one opaque RGB group without alpha or AOVs");
        ca = selection.axis;
        for (unsigned c = 0; c < 3; ++c)
          channels[c] = static_cast<unsigned>(selection.channels[c]);
      } catch (const basis_ops::CheckedFailure& failure) {
        throw failure.status;
      }
    }
    if (input.layout.spatial)
      require(input.layout.height_axis == ya && input.layout.width_axis == xa,
              "BM3D axes must match the spatial layout");
    try {
      if (dims[ya] > INT32_MAX / 4 || dims[xa] > INT32_MAX / 4 ||
          o.search_radius >= INT32_MAX / 4)
        throw std::length_error("BM3D coordinate capacity overflow");
      options = {static_cast<int>(dims[ya]),
                 static_cast<int>(dims[xa]),
                 color ? 3 : 1,
                 static_cast<int>(o.block_size),
                 static_cast<int>(o.search_radius),
                 static_cast<int>(o.group_size),
                 static_cast<int>(o.reference_step),
                 o.match_hard,
                 o.match_wiener,
                 o.hard_threshold,
                 o.kaiser_beta};
      const auto prepare = [&](auto native) {
        using T = decltype(native);
        const bm3d::Workspace<T> w(options, nullptr);
        using U = unsigned __int128;
        count =
            bm3d::checked(U(options.height) * options.width * options.channels);
        workspace = bm3d::checked(U(2) * count * sizeof(T));
        scratch_bytes = bm3d::checked(U(workspace) + w.bytes);
        const U ny = bm3d::references(options.height, options.step), nx = w.nx;
        const U patch = U(options.block) * options.block;
        // Upper bounds include distance rebases, sorted candidate insertion,
        // both cache populations, Haar passes, weighting, scatter and color.
        const U distance =
            U(w.offsets) * ny *
            ((U(options.block) + 2 * options.step) * w.columns * 8 +
             nx * (U(options.block) * 2 + U(options.group) * 8 + 24));
        const U filtering =
            ny * nx * options.channels * options.group * patch * 256;
        const U total = 2 * (distance + filtering) +
                        U(w.padded_area) * options.channels * 32 +
                        U(count) * 32 + U(options.block + 1) * 16384 +
                        patch * 8;
        if (total > UINT64_MAX)
          throw std::length_error("BM3D work capacity overflow");
        admitted = static_cast<std::uint64_t>(total);
      };
      if (type == ElementType::Float32)
        prepare(float{});
      else
        prepare(double{});
    } catch (const std::length_error& e) {
      throw Status{ErrorCode::ResourceExhausted, e.what(),
                   FailureReason::ArithmeticOverflow};
    }
    auto schema = *in[0].result_schema;
    schema.id = "photospider.tensor";
    schema.version = 1;
    schema.tensors[0].key = "samples";
    add_output(this, std::move(schema));
    collective = true;
    whole_inputs = true;
  }
  template <class T, class Poll>
  void gather(const mask::Reader& reader, T* input, Poll poll) const {
    const auto area = static_cast<std::size_t>(options.height) * options.width;
    Coordinate at{};
    for (int c = 0; c < options.channels; ++c) {
      if (ca >= 0)
        at[ca] = channels[c];
      for (int y = 0; y < options.height; ++y) {
        at[ya] = y;
        for (int x = 0; x < options.width;) {
          poll();
          at[xa] = x;
          const auto& v = reader.view(0, 0, at);
          const auto end = v.layout->origin[xa] + v.layout->shape[xa];
          const int n = static_cast<int>(std::min<std::uint64_t>(
              {end - x, static_cast<std::uint64_t>(options.width - x), 256}));
          T* dst = input + static_cast<std::size_t>(c) * area +
                   static_cast<std::size_t>(y) * options.width + x;
          if (v.layout->strides[xa] == sizeof(T)) {
            std::memcpy(dst,
                        static_cast<const unsigned char*>(v.allocation) +
                            offset(*v.layout, v.byte_offset, at),
                        n * sizeof(T));
          } else {
            for (int j = 0; j < n; ++j) {
              at[xa] = x + j;
              const auto bits = reader.raw(0, 0, at);
              std::memcpy(dst + j, &bits, sizeof(T));
            }
          }
          x += n;
        }
      }
    }
  }
  template <class T>
  void compute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    check(work.stage());
    const mask::Reader reader{k, inv};
    const double sigma = reader.value(1, 0, Coordinate{});
    const auto poll = [&] {
      if (inv.cancellation.is_cancelled &&
          inv.cancellation.is_cancelled(inv.cancellation.context))
        throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
    };
    if (sigma == 0) {
      std::uint64_t copies = 0;
      for (unsigned v = 0; v < inv.output_count; ++v) {
        std::uint64_t n = 1;
        for (unsigned a = 0; a < inv.outputs[v].layout->rank; ++a)
          n *= inv.outputs[v].layout->shape[a];
        copies += n;
      }
      work.poll(copies);
      for (unsigned v = 0; v < inv.output_count; ++v)
        samples(inv.outputs[v], [&](const Coordinate& at, std::uint64_t i) {
          if (!(i & 255))
            poll();
          store(inv.outputs[v], at, reader.raw(0, 0, at));
        });
      return;
    }
    work.poll(admitted);
    auto* input = static_cast<T*>(inv.scratch[0].allocation);
    auto* output = input + count;
    auto* scratch = reinterpret_cast<unsigned char*>(input) + workspace;
    gather(reader, input, poll);
    bm3d::compute(options, input, output, static_cast<T>(sigma), scratch, poll);
    const auto area = static_cast<std::size_t>(options.height) * options.width;
    std::array<unsigned, 3> inverse{};
    for (unsigned c = 0; c < static_cast<unsigned>(options.channels); ++c)
      inverse[channels[c]] = c;
    for (unsigned v = 0; v < inv.output_count; ++v) {
      samples(inv.outputs[v], [&](const Coordinate& at, std::uint64_t i) {
        if (!(i & 255))
          poll();
        const auto c = ca < 0 ? 0 : inverse[at[ca]];
        const auto pos = c * area + at[ya] * options.width + at[xa];
        std::uint64_t bits = 0;
        std::memcpy(&bits, output + pos, sizeof(T));
        store(inv.outputs[v], at, bits);
      });
    }
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& inv) const override {
    if (type == ElementType::Float32)
      compute<float>(k, inv, Work{inv, work_limit});
    else
      compute<double>(k, inv, Work{inv, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& inv) const override {
    const Work work{inv.views, work_limit, 0, inv.work};
    if (type == ElementType::Float32)
      compute<float>(k, inv.views, work);
    else
      compute<double>(k, inv.views, work);
  }
};
OperationDefinition bm3d_operation(bool color) {
  using Type = OperationParameterType;
  std::vector<OperationParameterSpec> parameters{
      {"y_axis", Type::Int64},         {"x_axis", Type::Int64},
      {"profile", Type::String},       {"block_size", Type::Int64},
      {"search_radius", Type::Int64},  {"group_size", Type::Int64},
      {"reference_step", Type::Int64}, {"match_hard", Type::Float64},
      {"match_wiener", Type::Float64}, {"hard_threshold", Type::Float64},
      {"kaiser_beta", Type::Float64}};
  if (color) {
    parameters.push_back({"input_description", Type::String});
    parameters.push_back({"group", Type::String});
  }
  return budgeted_operation(
      operation(color ? "restoration.cbm3d_native" : "restoration.bm3d_native",
                2, std::move(parameters),
                {{"output", tensor_schema(ElementType::Float64, {1, 1})}},
                [color](const auto& in, const auto& p) {
                  return std::make_shared<Bm3d>(color, in, p);
                }));
}
}  // namespace
}  // namespace ps::plugin_internal::restoration

namespace ps::plugin_internal {
Status register_restoration_bm3d(OperationRegistry* registry) {
  for (bool color : {false, true}) {
    const auto status =
        registry->register_operation(restoration::bm3d_operation(color));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal

namespace ps::restoration {
namespace {
Result<WorkflowInput> append_bm3d(WorkflowDocument& document,
                                  WorkflowInput input, WorkflowInput sigma,
                                  const Bm3dOptions& options,
                                  const TensorDescription* description,
                                  const std::string& group) {
  try {
    plugin_internal::restoration::validate_bm3d_options(options);
    auto ids =
        numeric::available_workflow_node_ids(document, 1, {input, sigma});
    if (!ids.ok())
      return Result<WorkflowInput>(ids.status());
    std::map<std::string, ParameterValue> params{
        {"y_axis", options.y_axis},
        {"x_axis", options.x_axis},
        {"profile", options.profile},
        {"block_size", options.block_size},
        {"search_radius", options.search_radius},
        {"group_size", options.group_size},
        {"reference_step", options.reference_step},
        {"match_hard", options.match_hard},
        {"match_wiener", options.match_wiener},
        {"hard_threshold", options.hard_threshold},
        {"kaiser_beta", options.kaiser_beta}};
    if (description) {
      auto encoded = tensor_description_parameter(*description);
      if (!encoded.ok())
        return Result<WorkflowInput>(encoded.status());
      params.emplace("input_description", encoded.take_value());
      params.emplace("group", group);
    }
    const auto id = ids.value()[0];
    document.nodes.push_back(
        {id,
         description ? "restoration.cbm3d_native" : "restoration.bm3d_native",
         {std::move(input), std::move(sigma)},
         std::move(params)});
    return Result<WorkflowInput>(WorkflowNodeOutput{id, "output"});
  } catch (const Status& failure) {
    return Result<WorkflowInput>(failure);
  }
}
}  // namespace
Result<WorkflowInput> bm3d_native(WorkflowDocument& document,
                                  WorkflowInput input, WorkflowInput sigma,
                                  const Bm3dOptions& options) {
  return append_bm3d(document, std::move(input), std::move(sigma), options,
                     nullptr, {});
}
Result<WorkflowInput> cbm3d_native(WorkflowDocument& document,
                                   WorkflowInput input, WorkflowInput sigma,
                                   const Bm3dOptions& options,
                                   const TensorDescription& description,
                                   const std::string& group) {
  return append_bm3d(document, std::move(input), std::move(sigma), options,
                     &description, group);
}
}  // namespace ps::restoration
