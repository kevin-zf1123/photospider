// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "04-mask-morphology/common.hpp"
#include "07-grade/channel_mixer_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::grade::mixer {
using namespace generation;  // NOLINT(build/namespaces)
inline bool empty(const Region& r) {
  return r.dimensions().empty() ||
         std::any_of(r.dimensions().begin(), r.dimensions().end(),
                     [](const auto& d) { return d.extent == 0; });
}
inline void cancel(const ps_kernel_invocation_v8& inv) {
  if (inv.cancellation.is_cancelled &&
      inv.cancellation.is_cancelled(inv.cancellation.context)) {
    throw Status{ErrorCode::Cancelled, {}, FailureReason::Cancelled};
  }
}
inline void roles(const std::string& value, unsigned count) {
  unsigned n = 0;
  std::size_t start = 0;
  do {
    const auto end = value.find(',', start);
    const auto token =
        value.substr(start, end == std::string::npos ? end : end - start);
    require(!token.empty() &&
                token.find_first_of(" \t\r\n\v\f") == std::string::npos,
            "GRD15 output_roles requires nonempty comma-separated roles");
    ++n;
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  } while (true);
  require(n == count, "GRD15 output_roles must have D entries");
}
template <class T, class V>
inline auto address(const V& v, const Coordinate& at) {
  using Byte = std::conditional_t<std::is_same_v<V, ps_kernel_const_view_v8>,
                                  const unsigned char, unsigned char>;
  using Pointer = std::conditional_t<std::is_same_v<V, ps_kernel_const_view_v8>,
                                     const T*, T*>;
  return reinterpret_cast<Pointer>(static_cast<Byte*>(v.allocation) +
                                   offset(*v.layout, v.byte_offset, at));
}
struct ChannelMixer final : Program {
  unsigned axis = 0, destination_count = 0;
  ElementType type;
  std::vector<std::uint64_t> dims, selected;

  ChannelMixer(const std::vector<OperationMetadata>& input,
               const Parameters& p) {
    const auto& source = tensor(input.at(0), true);
    type = source.descriptor.element_type;
    dims = source.sample_shape();
    require(floating(type), "GRD15 input requires Float32/64");
    const unsigned batch_rank = static_cast<unsigned>(source.batch_axes.size());
    if (!p.count("input_description")) {
      require(p.count("axis") && p.count("components") && !p.count("group"),
              "GRD15 requires axis/components or input_description/group");
      const auto a = integer(p, "axis");
      require(a >= 0 && static_cast<std::uint64_t>(a) <
                            source.descriptor.shape.size(),
              "GRD15 invalid component axis");
      axis = static_cast<unsigned>(a) + batch_rank;
      selected = mask::channels(text(p, "components"), dims[axis]);
    } else {
      require(!p.count("axis") && !p.count("components") && p.count("group"),
              "GRD15 description selection requires only group");
      const auto d =
          take(tensor_description_from_parameter(text(p, "input_description")));
      check(validate_tensor_description(d, source.descriptor));
      require(d.channel_axis.has_value(),
              "GRD15 description requires a component axis");
      axis = batch_rank + *d.channel_axis;
      try {
        color_point::native_encoding(d.encoding);
        const TensorColorGroup* chosen = nullptr;
        for (const auto& group : d.groups) {
          if (group.name == text(p, "group")) {
            require(chosen == nullptr, "GRD15 ambiguous group");
            chosen = &group;
          }
        }
        require(chosen != nullptr, "GRD15 group not found");
        selected = chosen->indices;
        for (const auto& c : chosen->components) {
          color_point::native_encoding(c.encoding);
        }
        for (auto c : selected) {
          if (c < d.channels.size()) {
            color_point::native_encoding(d.channels[c].encoding);
          }
        }
      } catch (const basis_ops::CheckedFailure& failure) {
        throw failure.status;
      }
    }
    require(!selected.empty() && selected.size() <= 64,
            "GRD15 selected component count must be 1..64");
    for (unsigned c = 0; c < selected.size(); ++c) {
      require(selected[c] < dims[axis] &&
                  std::find(selected.begin(), selected.begin() + c,
                            selected[c]) == selected.begin() + c,
              "GRD15 components must be distinct valid slots");
    }
    if (source.layout.spatial) {
      require(source.layout.channel_axis ==
                  std::optional<std::uint32_t>(axis - batch_rank),
              "GRD15 selector must use the spatial component axis");
    }
    const auto& matrix = tensor(input.at(1));
    const auto& bias = tensor(input.at(2));
    const auto shape = matrix.sample_shape();
    require(matrix.descriptor.element_type == ElementType::Float64 &&
                shape.size() == 2 && shape[0] <= 64 &&
                shape[1] == selected.size(),
            "GRD15 matrix requires Float64[D,Csel], D=1..64");
    destination_count = static_cast<unsigned>(shape[0]);
    require(bias.descriptor.element_type == ElementType::Float64 &&
                bias.sample_shape() ==
                    std::vector<std::uint64_t>{destination_count},
            "GRD15 bias requires Float64[D]");
    roles(text(p, "output_roles"), destination_count);
    auto output = *input[0].result_schema;
    output.id = "photospider.tensor";
    output.version = 1;
    auto& t = output.tensors[0];
    t.key = "samples";
    t.atomic_trailing_axes = 0;
    t.descriptor.shape[axis - batch_rank] = destination_count;
    size_product(t.sample_shape());
    check(output.validate());
    add_output(this, std::move(output));
  }

  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    if (empty(r)) {
      return {};
    }
    std::vector<ReadRegion> result;
    auto slots = selected;
    std::sort(slots.begin(), slots.end());
    for (std::size_t i = 0; i < slots.size();) {
      const auto first = slots[i];
      std::size_t j = i + 1;
      while (j < slots.size() && slots[j] == slots[j - 1] + 1) {
        ++j;
      }
      auto source = r.dimensions();
      source[axis] = {first, j - i};
      result.push_back({0, 0, Region(std::move(source))});
      i = j;
    }
    const auto rows = r.dimensions()[axis];
    result.push_back({1, 0, Region({rows, {0, selected.size()}})});
    result.push_back({2, 0, Region({rows})});
    return result;
  }

  // The packed path consumes complete source tuples inside each bound view;
  // every other affine or fragmented layout retains the coordinate path.
  template <class T>
  bool packed(const mask::Reader& reader, const ps_kernel_invocation_v8& inv,
              const ps_kernel_mutable_view_v8& out, const Transform& op) const {
    const auto& l = *out.layout;
    if (axis == 0 || axis + 1 != l.rank || l.strides[axis] != sizeof(T) ||
        l.strides[axis - 1] != op.outputs * sizeof(T)) {
      return false;
    }
    Coordinate at{};
    for (unsigned a = 0; a < l.rank; ++a) {
      at[a] = l.origin[a];
    }
    const unsigned run = axis - 1;
    std::uint64_t rows = 1;
    for (unsigned a = 0; a < run; ++a) {
      rows *= l.shape[a];
    }
    // Prove all spans before writing; channel tiles can split at any extent.
    for (std::uint64_t row = 0; row < rows; ++row) {
      for (at[run] = l.origin[run]; at[run] < l.origin[run] + l.shape[run];) {
        auto q = at;
        q[axis] = selected[0];
        const auto& v = reader.view(0, 0, q);
        q[axis] = 0;
        if (v.layout->origin[axis] != 0 ||
            v.layout->shape[axis] != dims[axis] ||
            v.layout->strides[axis] != sizeof(T) ||
            v.layout->strides[run] != dims[axis] * sizeof(T) ||
            reinterpret_cast<std::uintptr_t>(address<T>(v, q)) % alignof(T) ||
            reinterpret_cast<std::uintptr_t>(address<T>(out, at)) %
                alignof(T)) {
          return false;
        }
        at[run] = std::min(l.origin[run] + l.shape[run],
                           v.layout->origin[run] + v.layout->shape[run]);
      }
      for (unsigned a = run; a-- > 0;) {
        if (++at[a] < l.origin[a] + l.shape[a]) {
          break;
        }
        at[a] = l.origin[a];
      }
    }
    for (unsigned a = 0; a < l.rank; ++a) {
      at[a] = l.origin[a];
    }
    for (std::uint64_t row = 0; row < rows; ++row) {
      for (at[run] = l.origin[run]; at[run] < l.origin[run] + l.shape[run];) {
        cancel(inv);
        auto q = at;
        q[axis] = selected[0];
        const auto& v = reader.view(0, 0, q);
        q[axis] = 0;
        const auto n = std::min<std::uint64_t>(
            {1024, l.origin[run] + l.shape[run] - at[run],
             v.layout->origin[run] + v.layout->shape[run] - at[run]});
        op.span(address<T>(v, q), address<T>(out, at), n, dims[axis]);
        at[run] += n;
      }
      for (unsigned a = run; a-- > 0;) {
        if (++at[a] < l.origin[a] + l.shape[a]) {
          break;
        }
        at[a] = l.origin[a];
      }
    }
    return true;
  }

  template <class T>
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{kernel, inv};
    for (unsigned output = 0; output < inv.output_count; ++output) {
      const auto& out = inv.outputs[output];
      const auto& l = *out.layout;
      std::uint64_t count = 1;
      Coordinate at{};
      for (unsigned a = 0; a < l.rank; ++a) {
        count *= l.shape[a];
        at[a] = l.origin[a];
      }
      if (!count) {
        continue;
      }
      Transform op;
      op.inputs = static_cast<unsigned>(selected.size());
      op.outputs = static_cast<unsigned>(l.shape[axis]);
      std::copy(selected.begin(), selected.end(), op.selected.begin());
      work.poll(count * (2 * op.inputs + 1) +
                op.outputs * ((op.outputs >= 8 ? 2 : 1) * op.inputs + 1));
      for (unsigned r = 0; r < op.outputs; ++r) {
        Coordinate q{};
        q[0] = l.origin[axis] + r;
        op.bias[r] = reader.value(2, 0, q);
        for (unsigned c = 0; c < op.inputs; ++c) {
          q[1] = c;
          op.matrix[r][c] = reader.value(1, 0, q);
        }
      }
      op.classify();
      if (packed<T>(reader, inv, out, op)) {
        continue;
      }
      const auto tuples = count / op.outputs;
      for (std::uint64_t i = 0; i < tuples; ++i) {
        if ((i & 63U) == 0) {
          cancel(inv);
        }
        std::array<std::uint64_t, 64> raw;
        std::array<double, 64> x;
        auto q = at;
        for (unsigned c = 0; c < op.inputs; ++c) {
          q[axis] = selected[c];
          raw[c] = reader.raw(0, 0, q);
          if (!op.copies) {
            T value;
            std::memcpy(&value, &raw[c], sizeof(T));
            x[c] = value;
          }
        }
        for (unsigned r = 0; r < op.outputs; ++r) {
          at[axis] = l.origin[axis] + r;
          store(out, at,
                op.copy[r] >= 0 ? raw[op.copy[r]]
                                : convert(op.row(x.data(), r), sizeof(T) == 4));
        }
        for (unsigned a = l.rank; a-- > 0;) {
          if (a == axis) {
            continue;
          }
          if (++at[a] < l.origin[a] + l.shape[a]) {
            break;
          }
          at[a] = l.origin[a];
        }
      }
    }
    cancel(inv);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& i) const override {
    if (type == ElementType::Float32) {
      compute<float>(k, i, Work{i, work_limit});
    } else {
      compute<double>(k, i, Work{i, work_limit});
    }
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& i) const override {
    if (type == ElementType::Float32) {
      compute<float>(k, i.views, Work{i.views, work_limit, 0, i.work});
    } else {
      compute<double>(k, i.views, Work{i.views, work_limit, 0, i.work});
    }
  }
};
OperationDefinition operation() {
  using Type = OperationParameterType;
  auto definition = budgeted_operation(generation::operation(
      "grade.channel_mixer", 3,
      {{"axis", Type::Int64, false},
       {"components", Type::String, false},
       {"input_description", Type::String, false},
       {"group", Type::String, false},
       {"output_roles", Type::String, true}},
      {{"values", tensor_schema(ElementType::Float64, {1})}},
      [](const auto& inputs, const auto& parameters) {
        return std::make_shared<ChannelMixer>(inputs, parameters);
      }));
  definition.reusable_tiles = true;
  return definition;
}
}  // namespace ps::plugin_internal::grade::mixer
namespace ps::plugin_internal {
Status register_grade_channel_mixer(OperationRegistry* registry) {
  return registry->register_operation(grade::mixer::operation());
}
}  // namespace ps::plugin_internal
