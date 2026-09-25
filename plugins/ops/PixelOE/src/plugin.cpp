#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "photospider/data/tensor_description.hpp"
#include "runtime.hpp"  // NOLINT(build/include_subdir)
namespace {
using px::Failure;
struct Parameter {
  const char* name;
  uint32_t type;
  double lo, hi;
};
// NOLINTBEGIN(whitespace/indent_namespace)
const Parameter schema[] = {{"pixel_size", 1, 2, 64},
                            {"thickness", 1, 0, 6},
                            {"mode", 4, 0, 0},
                            {"sharpen_mode", 4, 0, 0},
                            {"sharpen_factor", 2, 0, 16},
                            {"do_color_match", 3, 0, 0},
                            {"do_quant", 3, 0, 0},
                            {"num_colors", 1, 2, 256},
                            {"quant_mode", 4, 0, 0},
                            {"dither_mode", 4, 0, 0},
                            {"no_post_upscale", 3, 0, 0},
                            {"weight_mapping", 4, 0, 0},
                            {"weight_normalize", 4, 0, 0},
                            {"colorfix_blur", 4, 0, 0},
                            {"blur_impl", 4, 0, 0},
                            {"blur_rank", 1, 1, 65},
                            {"local_stats", 4, 0, 0},
                            {"stat_padding", 4, 0, 0}};
// NOLINTEND
void choice(const std::string& v, std::initializer_list<const char*> choices) {
  for (auto* c : choices) {
    if (v == c) {
      return;
    }
  }
  throw Failure(6, "unsupported PixelOE option: " + v);
}
px::Options options(const ps_operation_parameter_value_v9* p, uint32_t n) {
  px::Options o;
  for (uint32_t i = 0; i < n; ++i) {
    std::string key(p[i].key, p[i].key_size);
    std::string str =
        p[i].type == 4 ? std::string(p[i].string_value, p[i].string_size) : "";
#define INT(NAME)                                     \
  if (key == #NAME) {                                 \
    o.NAME = static_cast<uint32_t>(p[i].int64_value); \
  }
#define BOOL(NAME)                 \
  if (key == #NAME) {              \
    o.NAME = p[i].bool_value != 0; \
  }
#define STR(NAME)     \
  if (key == #NAME) { \
    o.NAME = str;     \
  }
    INT(pixel_size);
    INT(thickness);
    INT(num_colors);
    INT(blur_rank);
    BOOL(do_color_match);
    BOOL(do_quant);
    BOOL(no_post_upscale);
    STR(mode);
    STR(sharpen_mode);
    STR(quant_mode);
    STR(dither_mode);
    STR(weight_mapping);
    STR(weight_normalize);
    STR(colorfix_blur);
    STR(blur_impl);
    STR(local_stats);
    STR(stat_padding);
    if (key == "sharpen_factor") {
      o.sharpen_factor = static_cast<float>(p[i].float64_value);
    }
#undef INT
#undef BOOL
#undef STR
  }
  choice(o.mode, {"contrast", "k_centroid", "nearest", "nearest-exact",
                  "bilinear", "bicubic", "area", "lanczos"});
  choice(o.sharpen_mode, {"none", "unsharp", "laplacian"});
  choice(o.quant_mode, {"kmeans", "weighted-kmeans", "repeat-kmeans"});
  choice(o.dither_mode, {"none", "ordered", "error_diffusion"});
  choice(o.weight_mapping,
         {"current", "polarity", "contrast_ratio", "contrast_gated"});
  choice(o.weight_normalize, {"none", "global", "per_image"});
  choice(o.colorfix_blur, {"exact", "separable"});
  choice(o.blur_impl, {"lowrank", "direct", "sym", "tiled"});
  choice(o.local_stats, {"lattice", "sliding"});
  choice(o.stat_padding, {"zero", "replicate"});
  if (o.pixel_size < 2 || o.pixel_size > 64 || o.thickness > 6 ||
      o.num_colors < 2 || o.num_colors > 256 || o.blur_rank < 1 ||
      o.blur_rank > 65 || !std::isfinite(o.sharpen_factor) ||
      o.sharpen_factor < 0 || o.sharpen_factor > 16) {
    throw Failure(6, "invalid PixelOE parameter range");
  }
  return o;
}
const ps::ValueFacet& rgb_facet() {
  static const auto value = [] {
    ps::TensorDescription d;
    d.channel_axis = 2;
    d.model = "rgb";
    d.channels = {{"R", "red", "relative"},
                  {"G", "green", "relative"},
                  {"B", "blue", "relative"}};
    d.white = std::array<double, 2>{0.3127, 0.329};
    d.primaries = "srgb";
    d.transfer = "srgb";
    d.reference = "display";
    d.association = "straight";
    auto encoded = ps::encode_tensor_description(d);
    if (!encoded.ok()) {
      throw Failure(1, encoded.status().message);
    }
    return encoded.take_value();
  }();
  return value;
}
template <class Color>
void validate_color(const Color& d) {
  if ((!d.model.empty() && d.model != "RGB" && d.model != "rgb") ||
      (!d.primaries.empty() && d.primaries != "srgb") ||
      (!d.transfer.empty() && d.transfer != "srgb") ||
      (!d.reference.empty() && d.reference != "display") ||
      (!d.association.empty() && d.association != "straight") ||
      (d.white && *d.white != std::array<double, 2>{0.3127, 0.329}) ||
      (d.primaries_xy &&
       *d.primaries_xy !=
           std::array<double, 6>{0.64, 0.33, 0.30, 0.60, 0.15, 0.06}) ||
      d.profile || d.configured || d.analytic_binding ||
      d.convention != "relative-v1") {
    throw Failure(
        5, "PixelOE requires straight display sRGB/D65; convert explicitly");
  }
}
void validate_channel(const ps::TensorChannelDescription& ch, size_t index) {
  static const char* roles[]{"red", "green", "blue"};
  if ((!ch.role.empty() && ch.role != roles[index]) ||
      (!ch.unit.empty() && ch.unit != "relative") || ch.encoding) {
    throw Failure(
        5, "PixelOE requires RGB channel order and relative unencoded samples");
  }
  if (ch.interpretation) {
    validate_color(*ch.interpretation);
  }
}
void validate_metadata(const ps_planar_metadata_v1& m) {
  if (m.element_type != 4 || m.rank != 3 || m.shape[2] != 3 ||
      m.height_axis != 0 || m.width_axis != 1 || m.channel_axis != 2) {
    throw Failure(5, "PixelOE requires planar Float32 [H,W,3] RGB");
  }
  for (uint32_t i = 0; i < m.facet_count; ++i) {
    const auto& f = m.facets[i];
    std::string key(f.key, f.key_size);
    if (key != "photospider.tensor-description") {
      throw Failure(
          5, "PixelOE requires explicit tensor-description or untagged sRGB");
    }
    ps::ValueFacet facet{key,
                         f.version,
                         {f.payload, f.payload + f.payload_size}};
    auto decoded = ps::decode_tensor_description(facet);
    if (!decoded.ok()) {
      throw Failure(5, decoded.status().message);
    }
    const auto& d = decoded.value();
    if ((d.channel_axis && *d.channel_axis != 2) ||
        (!d.channels.empty() && d.channels.size() != 3)) {
      throw Failure(5, "PixelOE metadata channel axis/count mismatch");
    }
    validate_color(d);
    if (d.encoding || d.component || d.groups.size() > 1) {
      throw Failure(5, "PixelOE requires unencoded RGB metadata");
    }
    for (size_t channel = 0; channel < d.channels.size(); ++channel) {
      validate_channel(d.channels[channel], channel);
    }
    for (const auto& group : d.groups) {
      if (group.indices != std::vector<uint64_t>{0, 1, 2} || group.alpha ||
          group.components.size() != 3) {
        throw Failure(5,
                      "PixelOE requires one complete RGB group without alpha");
      }
      validate_color(group.interpretation);
      for (size_t channel = 0; channel < 3; ++channel) {
        validate_channel(group.components[channel], channel);
      }
    }
  }
}
template <class F>
int protect(F fn, char* diagnostic, size_t capacity) noexcept {
  try {
    px::Environment environment;
    fn();
    return 0;
  } catch (const Failure& e) {
    if (capacity) {
      std::snprintf(diagnostic, capacity, "%s", e.what());
    }
    return e.code;
  } catch (const std::bad_alloc&) {
    if (capacity) {
      std::snprintf(diagnostic, capacity, "PixelOE allocation failed");
    }
    return 4;
  } catch (const std::exception& e) {
    if (capacity) {
      std::snprintf(diagnostic, capacity, "%s", e.what());
    }
    return 1;
  } catch (...) {
    if (capacity) {
      std::snprintf(diagnostic, capacity, "PixelOE unexpected failure");
    }
    return 1;
  }
}
int infer(void* user, const ps_planar_metadata_v1* inputs, uint32_t count,
          const ps_operation_parameter_value_v9* params, uint32_t pc,
          ps_planar_metadata_v1* output, char* diagnostic, size_t capacity) {
  return protect(
      [&] {
        if (count != 1) {
          throw Failure(5, "PixelOE requires one input");
        }
        validate_metadata(inputs[0]);
        auto o = options(params, pc);
        uint32_t selected =
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user));
        uint64_t h = inputs[0].shape[0], w = inputs[0].shape[1],
                 p = o.pixel_size;
        // Shader indexing is uint32; bounds also keep 32.32 fixed sums
        // representable.
        if (!h || !w || h > 65536 || w > 65536) {
          throw Failure(6, "PixelOE dimension exceeds 65536");
        }
        h = (h + p - 1) / p * p;
        w = (w + p - 1) / p * p;
        if (h * w > UINT32_MAX / 12) {
          throw Failure(4, "PixelOE shader index bound exceeded");
        }
        if (selected == 0 && o.no_post_upscale) {
          h /= p;
          w /= p;
        }
        *output = {};
        output->struct_size = sizeof(*output);
        output->element_type = 4;
        output->rank = 3;
        output->shape[0] = h;
        output->shape[1] = w;
        output->shape[2] = selected == 2 ? 1 : 3;
        output->order = 1;
        output->height_axis = 0;
        output->width_axis = 1;
        output->channel_axis = 2;
        if (selected != 2) {
          const auto& f = rgb_facet();
          static const ps_operation_facet_view_v9 view{
              sizeof(view),
              f.key.data(),
              static_cast<uint32_t>(f.key.size()),
              f.version,
              f.payload.data(),
              static_cast<uint32_t>(f.payload.size())};
          output->facets = &view;
          output->facet_count = 1;
        }
      },
      diagnostic, capacity);
}
int execute(void* user, const ps_planar_metadata_v1* inputs, uint32_t count,
            const ps_operation_parameter_value_v9* params, uint32_t pc,
            const ps_planar_metadata_v1* output,
            const ps_planar_services_v1* services, char* diagnostic,
            size_t capacity) {
  return protect(
      [&] {
        if (count != 1) {
          throw Failure(5, "PixelOE requires one input");
        }
        px::Context c(services);
        auto o = options(params, pc);
        uint32_t h = inputs[0].shape[0], w = inputs[0].shape[1];
        auto img = c.image(h, w);
        auto* dst = static_cast<float*>(img.data);
        for (uint32_t ch = 0; ch < 3; ++ch) {
          for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w;) {
              c.check();
              uint64_t at[]{y, x, ch}, samples = 0;
              const uint8_t* data = nullptr;
              if (!services->read_row(services->context, 0, at, 3, &data,
                                      &samples) ||
                  !samples) {
                throw Failure(1, "PixelOE input row unavailable");
              }
              samples = std::min<uint64_t>(samples, w - x);
              for (uint64_t i = 0; i < samples; ++i) {
                float v;
                std::memcpy(&v, data + 4 * i, 4);
                if (!std::isfinite(v) || v < 0 || v > 1) {
                  throw Failure(1, "PixelOE input must be finite in [0,1]");
                }
                dst[static_cast<uint64_t>(ch) * h * w +
                    static_cast<uint64_t>(y) * w + x + i] = v;
              }
              x += samples;
            }
          }
        }
        uint32_t selected =
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user));
        auto result = px::run(c, std::move(img), o, selected);
        auto out = selected == 2
                       ? result.weight
                       : (selected == 1 ? result.expanded : result.image);
        if (out.height != output->shape[0] || out.width != output->shape[1] ||
            out.channels != output->shape[2]) {
          throw Failure(1, "PixelOE output shape mismatch");
        }
        const auto* values = static_cast<const float*>(out.data);
        for (uint32_t ch = 0; ch < out.channels; ++ch) {
          for (uint32_t y = 0; y < out.height; ++y) {
            for (uint32_t x = 0; x < out.width;) {
              c.check();
              uint64_t at[]{y, x, ch}, samples = 0;
              uint8_t* data = nullptr;
              if (!services->write_row(services->context, at, 3, &data,
                                       &samples) ||
                  !samples) {
                throw Failure(1, "PixelOE output row unavailable");
              }
              samples = std::min<uint64_t>(samples, out.width - x);
              const float* row =
                  values + static_cast<uint64_t>(ch) * out.height * out.width +
                  static_cast<uint64_t>(y) * out.width + x;
              for (uint64_t i = 0; i < samples; ++i) {
                if (!std::isfinite(row[i])) {
                  throw Failure(1, "PixelOE produced nonfinite samples");
                }
              }
              std::memcpy(data, row, samples * 4);
              x += samples;
            }
          }
        }
      },
      diagnostic, capacity);
}
int unused(void*, const ps_operation_value_view_v9*, uint32_t,
           const ps_operation_parameter_value_v9*, uint32_t, uint32_t,
           ps_operation_cancelled_v9, void*, const ps_operation_output_sink_v9*,
           char*, size_t) {
  return 1;
}
struct Tables {
  std::vector<ps_operation_parameter_descriptor_v9> parameters;
  ps_operation_port_constraint_v9 input{};
  ps_operation_descriptor_v9 operations[3]{};
  ps_planar_operation_v1 planar[3]{};
  ps_operation_plugin_api_v9 api{};
  ps_planar_operation_plugin_api_v1 extension{};
  Tables() {
    for (const auto& p : schema) {
      parameters.push_back({sizeof(ps_operation_parameter_descriptor_v9),
                            p.name, static_cast<uint32_t>(std::strlen(p.name)),
                            p.type, 1, p.type <= 2 ? 1u : 0u, p.lo, p.hi});
    }
    input.struct_size = sizeof(input);
    input.kind = PS_OPERATION_PORT_VALUE_V9;
    const char* keys[]{"pixeloe.pixelize", "pixeloe.expanded",
                       "pixeloe.weight"};
    for (uint32_t i = 0; i < 3; ++i) {
      auto& d = operations[i];
      d.struct_size = sizeof(d);
      d.key = keys[i];
      d.key_size = std::strlen(keys[i]);
      d.input_count = 1;
      d.flags = PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_DETERMINISTIC |
                PS_OPERATION_FLAG_SIDE_EFFECT_FREE;
      d.parameter_count = parameters.size();
      d.parameters = parameters.data();
      d.input_schema_count = 1;
      d.input_schema = &input;
      d.execute = unused;
      d.user_data = reinterpret_cast<void*>(uintptr_t(i));
      d.workspace_bytes = 4 * 1024 * 1024;
      d.workspace_input_multiplier = 16;
      d.output_count = 1;
      auto& out = d.outputs[0];
      out.struct_size = sizeof(out);
      out.key = "values";
      out.key_size = 6;
      out.output_element_type = 4;
      out.shape_rule = PS_OPERATION_SHAPE_PRESERVE_FIRST_V9;
      out.region_rule = PS_OPERATION_REGION_WHOLE_V9;
      out.output_schema = input;
      planar[i] = {sizeof(ps_planar_operation_v1), infer, execute};
    }
    api = {sizeof(api), 3, operations,
           [](const ps_operation_descriptor_v9*, uint32_t) {}};
    extension = {sizeof(extension), PS_PLANAR_OPERATION_ABI_VERSION_1, 3,
                 planar};
  }
};
Tables& tables() {
  static Tables t;
  return t;
}
}  // namespace
extern "C" PS_OPERATION_EXPORT uint32_t ps_operation_plugin_get_abi_version() {
  return PS_OPERATION_ABI_VERSION_9;
}
extern "C" PS_OPERATION_EXPORT const ps_operation_plugin_api_v9*
ps_operation_plugin_get_api_v9() {
  try {
    return &tables().api;
  } catch (...) {
    return nullptr;
  }
}
extern "C" PS_OPERATION_EXPORT const ps_planar_operation_plugin_api_v1*
ps_operation_plugin_get_planar_api_v1() {
  try {
    return &tables().extension;
  } catch (...) {
    return nullptr;
  }
}
