#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

#ifdef PIXELOE_PROFILE_IO
#include <chrono>
#include <cinttypes>
#endif

#include "photospider/data/tensor_description.hpp"
#include "runtime.hpp"  // NOLINT(build/include_subdir)
namespace {
using px::Failure;
enum class Profile : uint32_t { CpuWhole, MetalNative, CpuTiled, VulkanNative };
Profile profile(void* user) {
  return static_cast<Profile>(reinterpret_cast<uintptr_t>(user) / 3);
}
uint32_t selected_output(void* user) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user) % 3);
}
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
px::Options options(const ps_result_parameter_value_v2* p, uint32_t n) {
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
void validate_metadata(const ps_result_tensor_spec_v2& m) {
  if (m.element_type != 4 || m.rank != 3 || m.shape[2] != 3 ||
      m.height_axis != 0 || m.width_axis != 1 || m.channel_axis != 2 ||
      !m.spatial || m.batch_rank != 2) {
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
int protect(F fn) noexcept {
  try {
    px::Environment environment;
    fn();
    return 0;
  } catch (const Failure& failure) {
    return failure.code;
  } catch (const std::bad_alloc&) {
    return 4;
  } catch (...) {
    return 1;
  }
}
void require(int code, const char* message) {
  if (code)
    throw Failure(code, message);
}
const ps_result_tensor_spec_v2& image(const ps_result_port_v2& port) {
  if (port.kind != PS_RESULT_OBJECT_V2 || !port.schema ||
      port.schema->tensor_count != 1 || port.schema->field_count ||
      !port.schema->tensors)
    throw Failure(5, "PixelOE requires one Result image slot");
  return port.schema->tensors[0];
}
int infer(void* user, const ps_result_port_v2* inputs, uint32_t count,
          const ps_result_parameter_value_v2* params, uint32_t pc,
          const ps_result_port_v2* prototypes, uint32_t outputs,
          const ps_result_metadata_sink_v2* sink) {
  return protect([&] {
    if (count != 1 || outputs != 1)
      throw Failure(5, "PixelOE requires one input and output");
    const auto& input = image(inputs[0]);
    validate_metadata(input);
    const auto o = options(params, pc);
    uint64_t h = input.shape[0], w = input.shape[1], p = o.pixel_size;
    if (!h || !w || h > 65536 || w > 65536)
      throw Failure(6, "PixelOE dimension exceeds 65536");
    h = (h + p - 1) / p * p;
    w = (w + p - 1) / p * p;
    if (h * w > UINT32_MAX / 12)
      throw Failure(4, "PixelOE shader index bound exceeded");
    const auto selected = selected_output(user);
    if (selected == 0 && o.no_post_upscale) {
      h /= p;
      w /= p;
    }
    auto output = input;
    output.shape[0] = h;
    output.shape[1] = w;
    output.shape[2] = selected == 2 ? 1 : 3;
    output.storage_order = 1;
    output.row_pitch_bytes = 0;
    output.groups = nullptr;
    output.group_count = 0;
    output.facets = nullptr;
    output.facet_count = 0;
    ps_result_facet_view_v2 view{};
    if (selected != 2) {
      const auto& facet = rgb_facet();
      view = {sizeof(view),
              facet.key.data(),
              static_cast<uint32_t>(facet.key.size()),
              facet.version,
              facet.payload.data(),
              static_cast<uint32_t>(facet.payload.size())};
      output.facets = &view;
      output.facet_count = 1;
    }
    auto schema = *prototypes[0].schema;
    schema.tensors = &output;
    auto port = prototypes[0];
    port.schema = &schema;
    require(sink->set_output(sink->context, 0, &port),
            "PixelOE metadata publication failed");
  });
}
struct State {
  bool requested = false;
};
int start(void*, void* raw, const ps_result_query_v2*,
          const ps_result_services_v2*) {
  new (raw) State{};
  return 0;
}
void destroy_state(void*, void* raw) {
  static_cast<State*>(raw)->~State();
}
void destroy_api(void*) {}
int execute(void* user, void* raw, const ps_result_query_v2* query,
            const ps_result_services_v2* services) {
  auto& state = *static_cast<State*>(raw);
  if (query->requested_kind == 2 && query->requested_count == 0) {
    const auto code = protect([&] {
      require(services->begin_result(services->context),
              "PixelOE empty Result");
      const ps_result_relation_row_v2 row{
          0, 0, 8, PS_RESULT_TARGET_DESCRIPTOR_V2, 0, 0, 0};
      require(services->bind_descriptor(services->context, &row, 1,
                                        PS_RESULT_EXACT_V2),
              "PixelOE empty descriptor");
      require(services->publish_result(services->context, 1),
              "PixelOE empty seal");
    });
    return code ? code : PS_RESULT_PUBLISH_V2;
  }
  if (!state.requested) {
    const auto& source = image(query->inputs[0]);
    ps_result_region_v2 all{};
    all.struct_size = sizeof(all);
    all.rank = source.rank + source.batch_rank;
    all.extent[0] = source.batch_shape[0];
    all.extent[1] = source.batch_shape[1];
    for (unsigned axis = 0; axis < source.rank; ++axis)
      all.extent[axis + 2] = source.shape[axis];
    const auto code =
        services->need_tensor(services->context, 0, 0, 13, &all, 1);
    if (code)
      return code;
    state.requested = true;
    return PS_RESULT_NEED_V2;
  }
  const auto code = protect([&] {
    px::Context context(services);
    const auto target = profile(user);
    const auto required =
        target == Profile::MetalNative    ? PS_GPU_BACKEND_METAL_V1
        : target == Profile::VulkanNative ? PS_GPU_BACKEND_VULKAN_V1
                                          : 0;
    if ((required && (!services->gpu || services->gpu->backend != required)) ||
        (!required && services->gpu))
      throw Failure(3, "PixelOE requires its named numerical backend");
    const auto& source = image(query->inputs[0]);
    const auto& output = image(query->output->port);
    const auto selected = selected_output(user);
    const auto o = options(query->parameters, query->parameter_count);
    const auto h = static_cast<uint32_t>(source.shape[0]);
    const auto w = static_cast<uint32_t>(source.shape[1]);
    require(services->begin_result(services->context), "PixelOE begin Result");
    const ps_result_relation_row_v2 descriptor{
        0, 0, 8, PS_RESULT_TARGET_DESCRIPTOR_V2, 0, 0, 1};
    require(services->bind_descriptor(services->context, &descriptor, 1,
                                      PS_RESULT_EXACT_V2),
            "PixelOE descriptor witness");
    constexpr uint64_t block = 1024;
    auto packed = context.empty(block * output.shape[2]);
    uint64_t relation = 0;
    const auto source_count =
        source.batch_shape[0] * source.batch_shape[1] * h * w * 3;
    require(services->make_tensor_cartesian(
                services->context, 0, 0, 0, 5, 0, source_count,
                PS_RESULT_CONSERVATIVE_V2, &relation),
            "PixelOE Whole support");
    struct ReleaseRelation {
      const ps_result_services_v2* services;
      uint64_t handle;
      ~ReleaseRelation() {
        services->release_relation(services->context, handle);
      }
    } release_relation{services, relation};
    for (uint64_t frame = 0; frame < source.batch_shape[0]; ++frame)
      for (uint64_t layer = 0; layer < source.batch_shape[1]; ++layer) {
#ifdef PIXELOE_PROFILE_IO
        const auto input_start = std::chrono::steady_clock::now();
#endif
        auto input = context.image(h, w);
        auto* data = static_cast<float*>(input.data);
        ps_result_region_v2 source_region{};
        source_region.struct_size = sizeof(source_region);
        source_region.rank = 5;
        source_region.offset[0] = frame;
        source_region.offset[1] = layer;
        source_region.extent[0] = source_region.extent[1] = 1;
        source_region.extent[2] = h;
        source_region.extent[3] = w;
        source_region.extent[4] = 3;
        ps_result_tensor_window_v2 window{};
        window.struct_size = sizeof(window);
        uint64_t handle = 0;
        require(services->acquire_tensor_window(
                    services->context, 0, 0, &source_region, &window, &handle),
                "PixelOE input window");
        struct ReleaseWindow {
          const ps_result_services_v2* services;
          uint64_t handle;
          ~ReleaseWindow() {
            services->release_window(services->context, handle);
          }
        } release{services, handle};
        struct TransferSpan {
          const uint8_t* source;
          float* destination;
          uint64_t count;
          int64_t stride;
        };
        auto transfer_buffer = context.empty(1024, sizeof(TransferSpan));
        auto* spans = static_cast<TransferSpan*>(transfer_buffer.data);
        uint64_t span_count = 0;
        const auto flush = [&] {
          context.parallel_for(
              span_count, 32, [&](uint64_t begin, uint64_t end) {
                for (auto i = begin; i < end; ++i) {
                  context.check();
                  const auto& span = spans[i];
                  for (uint64_t lane = 0; lane < span.count; ++lane) {
                    float value;
                    std::memcpy(
                        &value,
                        span.source + static_cast<int64_t>(lane) * span.stride,
                        4);
                    if (!std::isfinite(value) || value < 0 || value > 1)
                      throw Failure(1, "PixelOE input must be finite in [0,1]");
                    span.destination[lane] = value;
                  }
                }
              });
          span_count = 0;
        };
        for (uint32_t ch = 0; ch < 3; ++ch)
          for (uint32_t y = 0; y < h; ++y) {
            context.charge(16ULL * w);
            for (uint32_t x = 0; x < w;) {
              uint64_t at[]{frame, layer, y, x, ch};
              ps_result_tensor_row_v2 row{};
              row.struct_size = sizeof(row);
              require(window.row(window.context, at, 5, &row),
                      "PixelOE input row");
              const auto count = std::min<uint64_t>({row.samples, w - x, 1024});
              if (!count)
                throw Failure(1, "PixelOE empty input row");
              spans[span_count++] = {row.data,
                                     data + static_cast<uint64_t>(ch) * h * w +
                                         static_cast<uint64_t>(y) * w + x,
                                     count, row.sample_stride_bytes};
              x += static_cast<uint32_t>(count);
              if (span_count == 1024)
                flush();
            }
          }
        if (span_count)
          flush();
#ifdef PIXELOE_PROFILE_IO
        const auto input_end = std::chrono::steady_clock::now();
#endif
        auto result = px::run(context, std::move(input), o, selected);
#ifdef PIXELOE_PROFILE_IO
        const auto output_start = std::chrono::steady_clock::now();
#endif
        const auto& out = selected == 2   ? result.weight
                          : selected == 1 ? result.expanded
                                          : result.image;
        if (out.height != output.shape[0] || out.width != output.shape[1] ||
            out.channels != output.shape[2])
          throw Failure(1, "PixelOE output shape mismatch");
        const auto* values = static_cast<const float*>(out.data);
        const auto rows_per_block = std::max<uint64_t>(1, block / out.width);
        for (uint64_t y = 0; y < out.height; y += rows_per_block)
          for (uint64_t x = 0; x < out.width; x += block) {
            const auto width = std::min(block, out.width - x);
            const auto height = std::min(rows_per_block, out.height - y);
            context.charge(width * height * out.channels * 12);
            auto* destination = static_cast<float*>(packed.data);
            context.parallel_for(
                width * height, 64, [&](uint64_t begin, uint64_t end) {
                  context.check();
                  for (auto pixel = begin; pixel < end; ++pixel)
                    for (uint64_t ch = 0; ch < out.channels; ++ch) {
                      const auto source_index =
                          ch * out.height * out.width +
                          (y + pixel / width) * out.width + x + pixel % width;
                      const float value = values[source_index];
                      if (!std::isfinite(value))
                        throw Failure(1, "PixelOE produced nonfinite samples");
                      destination[pixel * out.channels + ch] = value;
                    }
                });
            ps_result_region_v2 region{};
            region.struct_size = sizeof(region);
            region.rank = 5;
            region.offset[0] = frame;
            region.offset[1] = layer;
            region.offset[2] = y;
            region.offset[3] = x;
            region.extent[0] = region.extent[1] = 1;
            region.extent[2] = height;
            region.extent[3] = width;
            region.extent[4] = out.channels;
            require(services->publish_tensor_with_relation(
                        services->context, 0, &region,
                        static_cast<const uint8_t*>(packed.data),
                        width * height * out.channels * 4, relation,
                        PS_RESULT_FINAL_V2),
                    "PixelOE image publication");
          }
#ifdef PIXELOE_PROFILE_IO
        const auto output_end = std::chrono::steady_clock::now();
        const auto input_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(input_end -
                                                                 input_start)
                .count();
        const auto output_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(output_end -
                                                                 output_start)
                .count();
        std::fprintf(stderr,
                     "PS_PIXELOE_IO {\"profile\":%u,\"input_bytes\":%" PRIu64
                     ",\"input_host_ns\":%" PRIu64 ",\"output_bytes\":%" PRIu64
                     ",\"output_host_ns\":%" PRIu64 "}\n",
                     static_cast<unsigned>(target), uint64_t{h} * w * 3 * 4,
                     static_cast<uint64_t>(std::max<int64_t>(0, input_ns)),
                     uint64_t{out.height} * out.width * out.channels * 4,
                     static_cast<uint64_t>(std::max<int64_t>(0, output_ns)));
#endif
      }
    require(services->publish_result(services->context, 1),
            "PixelOE seal Result");
  });
  return code ? code : PS_RESULT_PUBLISH_V2;
}
struct Tables {
  std::vector<ps_result_parameter_descriptor_v2> parameters;
  ps_result_tensor_spec_v2 pixels{};
  ps_result_schema_v2 image_schema{};
  ps_result_port_v2 input{};
  ps_result_output_v2 output{};
  ps_result_operation_v2 operations[12]{};
  ps_result_operation_plugin_api_v2 api{};
  Tables() {
    for (const auto& p : schema)
      parameters.push_back({sizeof(ps_result_parameter_descriptor_v2), p.name,
                            static_cast<uint32_t>(std::strlen(p.name)), p.type,
                            1, p.type <= 2 ? 1u : 0u, p.lo, p.hi});
    pixels.struct_size = sizeof(pixels);
    pixels.key = "pixels";
    pixels.key_size = 6;
    pixels.element_type = 4;
    pixels.rank = 3;
    pixels.shape[0] = pixels.shape[1] = 1;
    pixels.shape[2] = 3;
    pixels.batch_rank = 2;
    pixels.batch_shape[0] = pixels.batch_shape[1] = 1;
    pixels.spatial = 1;
    pixels.width_axis = 1;
    pixels.channel_axis = 2;
    pixels.storage_order = 1;
    image_schema.struct_size = sizeof(image_schema);
    image_schema.id = "photospider.image";
    image_schema.id_size = 17;
    image_schema.version = 1;
    image_schema.publication = PS_RESULT_COMPLETE_BUNDLE_V2;
    image_schema.tensors = &pixels;
    image_schema.tensor_count = 1;
    input.struct_size = sizeof(input);
    input.kind = PS_RESULT_OBJECT_V2;
    input.schema = &image_schema;
    output.struct_size = sizeof(output);
    output.key = "values";
    output.key_size = 6;
    output.port = input;
    output.input_count = UINT32_MAX;
    output.execution = PS_RESULT_WHOLE_V2;
    const char* keys[]{"pixeloe.pixelize",
                       "pixeloe.expanded",
                       "pixeloe.weight",
                       "pixeloe.pixelize_metal_native_fp32",
                       "pixeloe.expanded_metal_native_fp32",
                       "pixeloe.weight_metal_native_fp32",
                       "pixeloe.pixelize_cpu_tiled",
                       "pixeloe.expanded_cpu_tiled",
                       "pixeloe.weight_cpu_tiled",
                       "pixeloe.pixelize_vulkan_native_fp32",
                       "pixeloe.expanded_vulkan_native_fp32",
                       "pixeloe.weight_vulkan_native_fp32"};
    for (uint32_t i = 0; i < 12; ++i) {
      auto& op = operations[i];
      op.struct_size = sizeof(op);
      op.key = keys[i];
      op.key_size = std::strlen(keys[i]);
      op.flags = (i < 3 || (i >= 6 && i < 9) ? PS_RESULT_FLAG_CPU_V2
                                             : PS_RESULT_FLAG_GPU_V2) |
                 PS_RESULT_FLAG_DETERMINISTIC_V2 |
                 PS_RESULT_FLAG_SIDE_EFFECT_FREE_V2;
      op.inputs = &input;
      op.input_count = 1;
      op.outputs = &output;
      op.output_count = 1;
      op.parameters = parameters.data();
      op.parameter_count = parameters.size();
      op.state_bytes = sizeof(State);
      op.workspace_bytes = UINT64_C(16) << 30;
      op.maximum_stages = 2;
      op.cpu_staged_tiles = i >= 6 && i < 9;
      op.user_data = reinterpret_cast<void*>(uintptr_t(i));
      op.resolve_metadata = infer;
      op.start = start;
      op.poll = execute;
      op.destroy = destroy_state;
    }
    api = {sizeof(api), PS_RESULT_OPERATION_ABI_VERSION_2,
           operations,  12,
           nullptr,     destroy_api};
  }
};
}  // namespace
extern "C" PS_RESULT_EXPORT const ps_result_operation_plugin_api_v2*
ps_result_operation_plugin_get_api_v2() {
  try {
    static Tables tables;
    return &tables.api;
  } catch (...) {
    return nullptr;
  }
}
