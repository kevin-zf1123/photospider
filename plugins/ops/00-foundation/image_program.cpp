#include "00-foundation/image_program.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "00-foundation/basic_common.hpp"
#include "00-foundation/image_native.hpp"
#include "data/input_validation.hpp"
#include "data/result_window_access.hpp"
#include "photospider/data/tensor_description.hpp"

namespace ps::plugin_internal::image_ops {
namespace {
using Poll = Result<ResultProgramPoll>;
constexpr const char* kSchema = "photospider.image";
Status invalid(const char* message) {
  return {ErrorCode::TypeMismatch, message};
}
template <class T>
T take(Result<T> result) {
  if (!result.ok()) {
    throw result.status();
  }
  return result.take_value();
}
void require(Status status) {
  if (!status.ok()) {
    throw status;
  }
}
const ResultTensorSpec& spec(const OperationMetadata& input) {
  return input.result_schema->tensors[0];
}
OperationPortConstraint image_port() {
  OperationPortConstraint port;
  port.kind = OperationPortKind::Result;
  port.result_schema_id = kSchema;
  port.result_schema_version = 1;
  return port;
}
bool control_port(ImageAlgorithm algorithm, std::uint32_t port) {
  return port != 0 && (algorithm == ImageAlgorithm::Exposure ||
                       algorithm == ImageAlgorithm::Opacity ||
                       algorithm == ImageAlgorithm::Brush);
}
OperationPortConstraint scalar_port(float minimum, float maximum) {
  OperationPortConstraint port;
  port.kind = OperationPortKind::Result;
  port.element_type = static_cast<std::uint32_t>(ElementType::Float32);
  port.scalar_bounds = true;
  port.minimum = minimum;
  port.maximum = maximum;
  return port;
}
Region source_box(const Region& output, const ResultTensorSpec& input,
                  ImageAlgorithm algorithm, std::uint64_t factor,
                  std::uint64_t split, std::uint32_t output_index,
                  std::uint64_t radius = 0) {
  auto dims = output.dimensions();
  if (input.descriptor.shape.size() == 2) {
    dims.resize(4);
  } else {
    dims[4] = {0, input.descriptor.shape[2]};
  }
  if (algorithm == ImageAlgorithm::Downsample) {
    for (unsigned axis = 2; axis < 4; ++axis) {
      auto& d = dims[axis];
      const auto length = input.descriptor.shape[axis - 2];
      const auto first = d.offset * factor;
      const auto remaining = length - first;
      d.extent = d.extent > remaining / factor ? remaining : d.extent * factor;
      d.offset = first;
    }
  }
  if (algorithm == ImageAlgorithm::Gaussian) {
    for (unsigned axis = 2; axis < 4; ++axis) {
      auto& d = dims[axis];
      const auto end = d.offset + d.extent;
      const auto length = input.descriptor.shape[axis - 2];
      const auto last = end + std::min(radius, length - end);
      d.offset -= std::min(radius, d.offset);
      d.extent = last - d.offset;
    }
  }
  if (algorithm == ImageAlgorithm::Split && output_index == 2) {
    dims[3].offset += split;
  }
  return Region(std::move(dims));
}
struct GaussianWeights {
  unsigned radius = 0;
  std::array<double, 129> values{};
};
std::uint64_t clamped(std::uint64_t coordinate, int tap, std::uint64_t length) {
  if (tap < 0)
    return coordinate - std::min(coordinate, static_cast<std::uint64_t>(-tap));
  return coordinate +
         std::min(length - 1 - coordinate, static_cast<std::uint64_t>(tap));
}
struct Program final {
  ImageAlgorithm algorithm;
  bool requested = false;
  Footprint outputs;
  std::uint64_t factor = 1, split = 0;
  const GaussianWeights* weights;
  explicit Program(ImageAlgorithm kind,
                   const GaussianWeights* gaussian = nullptr)
      : algorithm(kind), weights(gaussian) {}

  float scalar(const ResultProgramPhase& phase, std::uint32_t input) {
    float value = 0;
    require(phase.read_tensor(input, 0, {0}, &value, sizeof(value)));
    if (!std::isfinite(value)) {
      throw Status{ErrorCode::OperationFailed, "nonfinite image control"};
    }
    return value;
  }
  std::array<float, 4> pixel(const ResultProgramPhase& phase,
                             std::uint32_t input, std::uint64_t frame,
                             std::uint64_t layer, std::uint64_t y,
                             std::uint64_t x) {
    const auto& image = spec(phase.query.inputs[input]);
    std::array<float, 4> result{};
    std::vector<std::uint64_t> at{frame, layer, y, x};
    const bool rgba = image.descriptor.shape.size() == 3;
    if (rgba) {
      at.push_back(0);
    }
    for (unsigned c = 0; c < (rgba ? 4U : 1U); ++c) {
      if (rgba) {
        at[4] = c;
      }
      require(phase.read_tensor(input, 0, at, &result[c], sizeof(float)));
      if (!std::isfinite(result[c])) {
        throw Status{ErrorCode::OperationFailed, "nonfinite image sample"};
      }
    }
    if ((rgba && (result[3] < 0 || result[3] > 1 ||
                  (result[3] == 0 &&
                   (result[0] != 0 || result[1] != 0 || result[2] != 0)))) ||
        (!rgba && (result[0] < 0 || result[0] > 1))) {
      throw Status{ErrorCode::OperationFailed, "invalid image coverage"};
    }
    return result;
  }
  Poll need(const ResultProgramPhase& phase) {
    const auto& query = phase.query;
    const auto shape = spec(query.output).sample_shape();
    outputs = query.tensor_outputs ? *query.tensor_outputs
                                   : take(Footprint::all(shape));
    if (algorithm == ImageAlgorithm::Downsample) {
      factor = static_cast<std::uint64_t>(
          std::get<std::int64_t>(query.parameters.at("factor")));
    }
    if (algorithm == ImageAlgorithm::Split) {
      split = static_cast<std::uint64_t>(
          std::get<std::int64_t>(query.parameters.at("split_x")));
    }
    ResultProgramNeed need;
    for (std::uint32_t i = 0; i < query.inputs.size(); ++i) {
      if (!control_port(algorithm, i)) {
        std::vector<Region> boxes;
        for (const auto& box : outputs.boxes()) {
          boxes.push_back(source_box(box, spec(query.inputs[i]), algorithm,
                                     factor, split, query.output_index,
                                     weights ? weights->radius : 0));
        }
        if (!boxes.empty()) {
          need.tensors.push_back(
              {i, 0,
               take(Footprint::from_regions(
                   spec(query.inputs[i]).sample_shape(), boxes)),
               5});
        }
      } else {
        // Controls are complete observations, including Empty image demand.
        need.tensors.push_back({i, 0, take(Footprint::all({1})), 13});
      }
    }
    requested = true;
    if (need.tensors.empty()) {
      return finish(phase);
    }
    return Poll(std::move(need));
  }
  ResultRelation descriptor(const ResultProgramPhase& phase) {
    std::vector<ResultRelation> relations;
    for (std::uint32_t i = 0; i < phase.query.inputs.size(); ++i) {
      if (phase.query.inputs[i].result_schema) {
        relations.push_back(take(ResultRelation::cartesian(
            phase.resources, 1,
            {i, 8, 0, 1, ResultSupportTarget::Descriptor})));
      }
    }
    return relations.empty()
               ? take(ResultRelation::cartesian(phase.resources, 1,
                                                {0, 1, 0, 0}))
               : take(ResultRelation::unite(phase.resources, relations));
  }
  ResultRelation relation(const ResultProgramPhase& phase, const Region&) {
    const auto shape = spec(phase.query.output).sample_shape();
    if (algorithm == ImageAlgorithm::Gaussian) {
      std::vector<std::uint64_t> radii(shape.size());
      radii[2] = radii[3] = weights->radius;
      radii[4] = 3;
      return take(ResultRelation::neighborhood(
          phase.resources, shape, radii, false,
          {0, 5, 0, 0, ResultSupportTarget::Tensor, 0}));
    }
    const auto total = take(spec(phase.query.output).sample_count());
    std::vector<ResultRelation> relations;
    for (std::uint32_t input = 0; input < phase.query.inputs.size(); ++input) {
      const auto& metadata = phase.query.inputs[input];
      if (control_port(algorithm, input)) {
        relations.push_back(take(ResultRelation::cartesian(
            phase.resources, total,
            {input, 5, 0, 1, ResultSupportTarget::Tensor, 0})));
        continue;
      }
      const auto& image = spec(metadata);
      const auto input_shape = image.sample_shape();
      std::vector<ResultMappedAxis> axes(input_shape.size());
      for (std::size_t axis = 0; axis < axes.size(); ++axis) {
        axes[axis].output_axis = static_cast<std::int32_t>(axis);
      }
      if (algorithm == ImageAlgorithm::Downsample) {
        for (unsigned axis = 2; axis < 4; ++axis) {
          axes[axis].step = factor;
          axes[axis].extent = factor;
        }
      }
      if (input_shape.size() == 5 && algorithm != ImageAlgorithm::Split) {
        axes[4].output_axis = -1;
        axes[4].extent = input_shape[4];
      }
      if (algorithm == ImageAlgorithm::Split && phase.query.output_index == 2) {
        axes[3].source_origin = split;
      }
      relations.push_back(take(ResultRelation::mapped(
          phase.resources, shape, Region::whole(shape), input_shape, axes,
          {input, 5, 0, 0, ResultSupportTarget::Tensor, 0})));
    }
    return take(ResultRelation::unite(phase.resources, relations));
  }
  void gaussian(const ResultProgramPhase& phase, const Region& tile,
                std::uint8_t* out) {
    const auto& image = spec(phase.query.inputs[0]);
    const auto source =
        source_box(tile, image, algorithm, 1, 0, 0, weights->radius);
    const auto window = take(
        phase.tensors->at({0, 0}).acquire(source, phase.query.cancellation));
    const auto read_work =
        take(execution_internal::ResultWindowAccess::read_work(window));
    const auto& d = tile.dimensions();
    const auto first_row = source.dimensions()[2].offset;
    const auto rows = source.dimensions()[2].extent;
    const auto width = d[3].extent;
    auto scratch = take(phase.allocator.allocate(rows * width * 16));
    auto coordinate_memory =
        take(phase.resources.reserve(ResourceCapacity::host(5 * 8, 5 * 8)));
    std::vector<std::uint64_t> at{d[0].offset, d[1].offset, 0, 0, 0};
    const auto pixel = [&](std::uint64_t y, std::uint64_t x) {
      at[2] = y;
      at[3] = x;
      std::array<float, 4> value{};
      for (unsigned c = 0; c < 4; ++c) {
        at[4] = c;
        require(phase.consume_work(read_work));
        const auto run = take(window.row_run(at));
        std::memcpy(&value[c], run.data, 4);
        if (!std::isfinite(value[c]))
          throw Status{ErrorCode::OperationFailed, "nonfinite image sample"};
      }
      if (value[3] < 0 || value[3] > 1 ||
          (value[3] == 0 && (value[0] != 0 || value[1] != 0 || value[2] != 0)))
        throw Status{ErrorCode::OperationFailed, "invalid image coverage"};
      return value;
    };
    const int radius = static_cast<int>(weights->radius);
    for (auto y = first_row; y < first_row + rows; ++y) {
      require(phase.consume_work(0));
      for (auto x = d[3].offset; x < d[3].offset + width; ++x) {
        std::array<double, 4> sums{};
        for (int tap = -radius; tap <= radius; ++tap) {
          const auto value =
              pixel(y, clamped(x, tap, image.descriptor.shape[1]));
          require(phase.consume_work(8));
          for (unsigned c = 0; c < 4; ++c) {
            const double product = value[c] * weights->values[tap + radius];
            sums[c] += product;
          }
        }
        for (unsigned c = 0; c < 4; ++c) {
          const float rounded = static_cast<float>(sums[c]);
          const auto index =
              (((y - first_row) * width + x - d[3].offset) * 4 + c) * 4;
          std::memcpy(scratch.data() + index, &rounded, 4);
        }
      }
    }
    std::uint64_t destination = 0;
    for (auto y = d[2].offset; y < d[2].offset + d[2].extent; ++y)
      for (auto x = d[3].offset; x < d[3].offset + width; ++x)
        for (unsigned c = 0; c < 4; ++c) {
          double sum = 0;
          require(phase.consume_work((2 * radius + 1) * 3));
          for (int tap = -radius; tap <= radius; ++tap) {
            const auto row = clamped(y, tap, image.descriptor.shape[0]);
            float value;
            const auto index =
                (((row - first_row) * width + x - d[3].offset) * 4 + c) * 4;
            std::memcpy(&value, scratch.data() + index, 4);
            const double product = value * weights->values[tap + radius];
            sum += product;
          }
          const float rounded = static_cast<float>(sum);
          if (!std::isfinite(rounded))
            throw Status{ErrorCode::OperationFailed, "image output overflow"};
          std::memcpy(out + destination, &rounded, 4);
          destination += 4;
        }
  }
  void compute(const ResultProgramPhase& phase, const Region& tile,
               std::uint8_t* out) {
    if (algorithm == ImageAlgorithm::Gaussian) {
      gaussian(phase, tile, out);
      return;
    }
    const auto& dims = tile.dimensions();
    const auto& image = spec(phase.query.inputs[0]);
    const auto channels =
        image.descriptor.shape.size() == 3 ? image.descriptor.shape[2] : 1U;
    std::array<float, 7> controls{};
    if (algorithm == ImageAlgorithm::Exposure ||
        algorithm == ImageAlgorithm::Opacity) {
      controls[0] = scalar(phase, 1);
    }
    if (algorithm == ImageAlgorithm::Brush) {
      for (unsigned i = 0; i < 7; ++i) {
        controls[i] = scalar(phase, i + 1);
      }
    }
    std::uint64_t next = 0;
    for (auto frame = dims[0].offset; frame < dims[0].offset + dims[0].extent;
         ++frame) {
      for (auto layer = dims[1].offset; layer < dims[1].offset + dims[1].extent;
           ++layer) {
        for (auto y = dims[2].offset; y < dims[2].offset + dims[2].extent;
             ++y) {
          require(phase.consume_work(0));
          for (auto x = dims[3].offset; x < dims[3].offset + dims[3].extent;
               ++x) {
            if (algorithm == ImageAlgorithm::Split) {
              for (auto c = dims[4].offset; c < dims[4].offset + dims[4].extent;
                   ++c) {
                const auto width =
                    Value::element_size(image.descriptor.element_type);
                require(phase.read_tensor(
                    0, 0,
                    {frame, layer, y,
                     x + (phase.query.output_index == 2 ? split : 0), c},
                    out + next, width));
                next += width;
              }
              continue;
            }
            auto a = algorithm == ImageAlgorithm::Downsample
                         ? std::array<float, 4>{}
                         : pixel(phase, 0, frame, layer, y,
                                 x + (algorithm == ImageAlgorithm::Split &&
                                              phase.query.output_index == 2
                                          ? split
                                          : 0));
            if (algorithm == ImageAlgorithm::Downsample) {
              const auto y0 = y * factor, x0 = x * factor;
              const auto h = std::min(factor, image.descriptor.shape[0] - y0);
              const auto w = std::min(factor, image.descriptor.shape[1] - x0);
              std::array<double, 4> sum{};
              for (auto yy = y0; yy < y0 + h; ++yy) {
                for (auto xx = x0; xx < x0 + w; ++xx) {
                  const auto p = pixel(phase, 0, frame, layer, yy, xx);
                  for (unsigned c = 0; c < channels; ++c) {
                    sum[c] += p[c];
                  }
                }
              }
              for (unsigned c = 0; c < channels; ++c) {
                a[c] = static_cast<float>(sum[c] / static_cast<double>(h * w));
              }
            } else if (algorithm == ImageAlgorithm::Exposure ||
                       algorithm == ImageAlgorithm::Opacity) {
              for (unsigned c = 0;
                   c < (algorithm == ImageAlgorithm::Opacity ? 4U : 3U); ++c) {
                a[c] *= controls[0];
              }
            } else if (algorithm == ImageAlgorithm::Mask) {
              const auto m = pixel(phase, 1, frame, layer, y, x)[0];
              for (auto& c : a) {
                c *= m;
              }
            } else if (algorithm == ImageAlgorithm::SourceOver) {
              const auto b = pixel(phase, 1, frame, layer, y, x);
              const float remaining = 1 - a[3];
              for (unsigned c = 0; c < 4; ++c) {
                const float back = b[c] * remaining;
                a[c] += back;
              }
            } else if (algorithm == ImageAlgorithm::Mix) {
              const auto b = pixel(phase, 1, frame, layer, y, x);
              const auto t = pixel(phase, 2, frame, layer, y, x)[0];
              for (unsigned c = 0; c < 4; ++c) {
                a[c] = static_cast<float>(basic_internal::blend(a[c], b[c], t));
              }
            } else if (algorithm == ImageAlgorithm::Brush) {
              const double dx = static_cast<double>(x) + .5 - controls[0];
              const double dy = static_cast<double>(y) + .5 - controls[1];
              if (dx * dx + dy * dy <=
                  static_cast<double>(controls[2]) * controls[2]) {
                for (unsigned c = 0; c < 4; ++c) {
                  const float source =
                      c == 3 ? controls[6] : controls[3 + c] * controls[6];
                  const float back = a[c] * (1 - controls[6]);
                  a[c] = source + back;
                }
              }
            }
            require(phase.consume_work(channels));
            for (unsigned c = 0; c < channels; ++c) {
              if (!std::isfinite(a[c])) {
                throw Status{ErrorCode::OperationFailed,
                             "image output overflow"};
              }
              std::memcpy(out + next, &a[c], sizeof(float));
              next += sizeof(float);
            }
          }
        }
      }
    }
  }
  Poll finish(const ResultProgramPhase& phase) {
    input_internal::Float32Environment environment;
    if (!environment.active()) {
      return Poll(Status{ErrorCode::OperationFailed,
                         "image arithmetic environment unavailable"});
    }
    const auto& query = phase.query;
    auto builder = take(ResultBuilder::start(
        phase.resources, *query.output.result_schema, query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{},
        query.tile_height, query.tile_width, query.resources));
    require(builder.bind_descriptor_relation(descriptor(phase)));
    const auto witness =
        relation(phase, Region::whole(spec(query.output).sample_shape()));
    // Relation rows and packed work buffers stay bounded independently of the
    // logical image domain. Each frame/layer is published in separate blocks.
    for (const auto& box : outputs.boxes()) {
      const auto& d = box.dimensions();
      const auto channels = d.size() == 5 ? d[4].extent : 1;
      const auto block_width =
          std::min<std::uint64_t>(64, 65536 / (channels * factor));
      const auto block_height = query.backend == Backend::Gpu
                                    ? std::max<std::uint64_t>(1, 64 / factor)
                                : algorithm == ImageAlgorithm::Gaussian ? 64U
                                                                        : 1U;
      for (auto n = d[0].offset; n < d[0].offset + d[0].extent; ++n) {
        for (auto l = d[1].offset; l < d[1].offset + d[1].extent; ++l) {
          for (auto y = d[2].offset; y < d[2].offset + d[2].extent;) {
            const auto height =
                std::min(block_height, d[2].offset + d[2].extent - y);
            for (auto x = d[3].offset; x < d[3].offset + d[3].extent;) {
              const auto width =
                  std::min(block_width, d[3].offset + d[3].extent - x);
              auto dimensions = d;
              dimensions[0] = {n, 1};
              dimensions[1] = {l, 1};
              dimensions[2] = {y, height};
              dimensions[3] = {x, width};
              const Region tile(std::move(dimensions));
              MutableBuffer buffer;
              if (query.backend == Backend::Gpu) {
                std::vector<Region> sources;
                for (unsigned i = 0; i < query.inputs.size(); ++i)
                  sources.push_back(
                      control_port(algorithm, i)
                          ? Region::whole({1})
                          : source_box(tile, spec(query.inputs[i]), algorithm,
                                       factor, split, query.output_index,
                                       weights ? weights->radius : 0));
                const auto kind = algorithm == ImageAlgorithm::Exposure     ? 0U
                                  : algorithm == ImageAlgorithm::Opacity    ? 1U
                                  : algorithm == ImageAlgorithm::Gaussian   ? 2U
                                  : algorithm == ImageAlgorithm::Mask       ? 3U
                                  : algorithm == ImageAlgorithm::SourceOver ? 4U
                                  : algorithm == ImageAlgorithm::Downsample
                                      ? (channels == 1 ? 6U : 5U)
                                      : 7U;
                buffer = take(native_image(
                    phase, kind, tile, sources, weights ? weights->radius : 0,
                    weights ? weights->values.data() : nullptr,
                    static_cast<unsigned>(factor)));
              } else {
                buffer = take(phase.resources.allocator().allocate(
                    take(tile.element_count()) *
                    Value::element_size(
                        spec(query.output).descriptor.element_type)));
                compute(phase, tile, buffer.data());
              }
              auto layout_memory = take(phase.resources.reserve(
                  ResourceCapacity::host(d.size() * 16, d.size() * 16)));
              StridedLayout layout;
              layout.byte_strides.resize(d.size());
              layout.origin.resize(d.size());
              std::uint64_t stride = Value::element_size(
                  spec(query.output).descriptor.element_type);
              for (std::size_t axis = d.size(); axis-- > 0;) {
                layout.byte_strides[axis] = static_cast<std::int64_t>(stride);
                layout.origin[axis] = tile.dimensions()[axis].offset;
                stride *= tile.dimensions()[axis].extent;
              }
              require(builder.publish_tensor(
                  0, tile, std::move(layout), std::move(buffer).freeze(),
                  witness, {true, true, true, true}, query.cancellation));
              x += width;
            }
            y += height;
          }
        }
      }
    }
    return Poll(ResultPublication{take(builder.seal()), true});
  }
  Poll poll(const ResultProgramPhase& phase) try {
    return requested ? finish(phase) : need(phase);
  } catch (const Status& status) {
    return Poll(status);
  } catch (const basic_internal::Failure& failure) {
    return Poll(failure.status);
  }
};
Poll empty_gaussian(const ResultProgramPhase& phase) try {
  if (!phase.query.tensor_outputs || !phase.query.tensor_outputs->empty())
    return Poll(
        Status{ErrorCode::InvalidArgument, "empty Gaussian demand changed"});
  auto builder = take(ResultBuilder::start(
      phase.resources, *phase.query.output.result_schema,
      phase.query.semantic_key, {}, {}, phase.query.tile_height,
      phase.query.tile_width, phase.query.resources));
  require(builder.bind_descriptor_relation(
      take(ResultRelation::cartesian(phase.resources, 1, {}))));
  return Poll(ResultPublication{take(builder.seal()), true});
} catch (const Status& status) {
  return Poll(status);
}

}  // namespace

SchemaTemplate image_schema(ImageKind kind) {
  SchemaTemplate schema;
  schema.id = kSchema;
  ResultTensorSpec image;
  image.batch_axes = {1, 1};
  image.layout.spatial = true;
  image.key = "pixels";
  image.descriptor = {ElementType::Float32,
                      kind == ImageKind::Coverage
                          ? std::vector<std::uint64_t>{1, 1}
                          : std::vector<std::uint64_t>{1, 1, 4}};
  if (kind == ImageKind::Coverage) {
    image.layout.channel_axis.reset();
    image.facets = {take(encode_semantic(coverage_semantics()))};
  } else if (kind == ImageKind::Rgba) {
    image.facets = {take(encode_semantic(rgba_semantics()))};
  }
  schema.tensors.push_back(std::move(image));
  return schema;
}
Status check_image(const OperationMetadata& metadata, ImageKind kind) {
  if (!metadata.result_schema || metadata.result_schema->id != kSchema ||
      metadata.result_schema->version != 1 ||
      metadata.result_schema->tensors.size() != 1 ||
      !metadata.result_schema->fields.empty()) {
    return invalid(
        "operation requires photospider.image schema with one pixels slot");
  }
  const auto& image = spec(metadata);
  if (image.key != "pixels") {
    return invalid("image slot must be named pixels");
  }
  if (image.batch_axes.size() != 2 || !image.layout.spatial) {
    return invalid(
        "image operation requires frame/layer batch axes and spatial layout");
  }
  if (kind == ImageKind::Tensor) {
    if (image.descriptor.shape.size() != 3 || image.layout.height_axis != 0 ||
        image.layout.width_axis != 1 ||
        image.layout.channel_axis != std::optional<std::uint32_t>{2}) {
      return invalid("split requires an HWC image tensor");
    }
    return Status::success();
  }
  const bool mask = kind == ImageKind::Coverage;
  if (image.descriptor.element_type != ElementType::Float32 ||
      image.descriptor.shape.size() != (mask ? 2U : 3U) ||
      image.layout.height_axis != 0 || image.layout.width_axis != 1 ||
      image.layout.channel_axis != (mask ? std::optional<std::uint32_t>{}
                                         : std::optional<std::uint32_t>{2}) ||
      (!mask && image.descriptor.shape[2] != 4)) {
    return invalid("image operation requires Float32 HW coverage or HWC RGBA");
  }
  const auto expected =
      take(encode_semantic(mask ? coverage_semantics() : rgba_semantics()));
  if (!input_internal::same_facets(image.facets, {expected})) {
    return invalid(
        "image operation requires canonical linear premultiplied RGBA or "
        "coverage semantics");
  }
  return Status::success();
}

Status register_image_algorithm(OperationRegistry* registry,
                                const std::string& key,
                                ImageAlgorithm algorithm, ImageKind kind) {
  OperationDefinition op;
  op.key = key;
  auto& traits = op.traits;
  traits.input_count = algorithm == ImageAlgorithm::Brush ? 8
                       : algorithm == ImageAlgorithm::Mix ? 3
                       : algorithm == ImageAlgorithm::Exposure ||
                               algorithm == ImageAlgorithm::Opacity ||
                               algorithm == ImageAlgorithm::Mask ||
                               algorithm == ImageAlgorithm::SourceOver
                           ? 2
                           : 1;
  traits.input_schema.assign(traits.input_count, image_port());
  if (algorithm == ImageAlgorithm::Exposure ||
      algorithm == ImageAlgorithm::Opacity) {
    traits.input_schema[1] =
        scalar_port(0, algorithm == ImageAlgorithm::Opacity ? 1.0F : 16.0F);
  }
  if (algorithm == ImageAlgorithm::Brush) {
    const auto maximum = std::numeric_limits<float>::max();
    for (unsigned i = 1; i < 8; ++i) {
      traits.input_schema[i] = scalar_port(-maximum, maximum);
    }
    traits.input_schema[3].minimum = std::numeric_limits<float>::min();
    traits.input_schema[7] = scalar_port(0, 1);
  }
  if (algorithm == ImageAlgorithm::Gaussian) {
    traits.parameter_schema = {
        {"radius", OperationParameterType::Int64, true, true, 1, 64},
        {"sigma", OperationParameterType::Float64, true, true, .1, 64}};
  }
  if (algorithm == ImageAlgorithm::Downsample) {
    traits.parameter_schema = {
        {"factor", OperationParameterType::Int64, true, true, 1, 16}};
  }
  if (algorithm == ImageAlgorithm::Split) {
    traits.parameter_schema = {
        {"split_x", OperationParameterType::Int64, true}};
  }
  traits.requires_metadata_specialization = true;
  traits.supports_gpu =
      algorithm != ImageAlgorithm::Mix && algorithm != ImageAlgorithm::Split;
  traits.allows_cpu_fallback = traits.supports_gpu;
  traits.workspace_bytes = traits.supports_gpu ? 8 * 1024 * 1024 : 262144;
  if (algorithm == ImageAlgorithm::Split) {
    traits.outputs.resize(3);
  }
  constexpr const char* names[] = {"full", "left", "right"};
  for (unsigned i = 0; i < traits.outputs.size(); ++i) {
    auto& out = traits.outputs[i];
    out.key = algorithm == ImageAlgorithm::Split ? names[i] : "value";
    out.output_schema = image_port();
    out.result_schema = image_schema(kind);
    out.region_rule = OperationRegionRule::Dependency;
    out.dependency_version = 2;
    out.continuation_bytes = sizeof(Program);
    out.maximum_dependency_stages = traits.supports_gpu ? 4 : 2;
  }
  op.specialize_metadata = [algorithm, kind](const auto& inputs,
                                             const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto status = check_image(inputs[0], kind);
    if (!status.ok()) {
      return Answer(status);
    }
    const auto& first = spec(inputs[0]);
    for (unsigned i = 1; i < inputs.size(); ++i) {
      if (control_port(algorithm, i)) {
        continue;
      }
      status =
          check_image(inputs[i], (algorithm == ImageAlgorithm::Mask ||
                                  (algorithm == ImageAlgorithm::Mix && i == 2))
                                     ? ImageKind::Coverage
                                     : kind);
      if (!status.ok()) {
        return Answer(status);
      }
      const auto& other = spec(inputs[i]);
      if (first.batch_axes[0] != other.batch_axes[0] ||
          first.batch_axes[1] != other.batch_axes[1] ||
          first.descriptor.shape[0] != other.descriptor.shape[0] ||
          first.descriptor.shape[1] != other.descriptor.shape[1]) {
        return Answer(
            invalid("image inputs must share frame/layer and spatial extents"));
      }
    }
    std::vector<OperationOutputSpecialization> outputs(
        algorithm == ImageAlgorithm::Split ? 3 : 1);
    for (unsigned i = 0; i < outputs.size(); ++i) {
      auto schema = *inputs[0].result_schema;
      if (algorithm == ImageAlgorithm::Downsample) {
        const auto factor = static_cast<std::uint64_t>(
            std::get<std::int64_t>(parameters.at("factor")));
        for (unsigned axis = 0; axis < 2; ++axis) {
          auto& length = schema.tensors[0].descriptor.shape[axis];
          length = length / factor + (length % factor != 0);
        }
      }
      if (algorithm == ImageAlgorithm::Split) {
        const auto split = std::get<std::int64_t>(parameters.at("split_x"));
        const auto width = first.descriptor.shape[1];
        if (split <= 0 || static_cast<std::uint64_t>(split) >= width) {
          return Answer(Status{ErrorCode::InvalidArgument,
                               "split_x must be strictly inside image width"});
        }
        if (i) {
          schema.tensors[0].descriptor.shape[1] =
              i == 1 ? split : width - split;
        }
      }
      outputs[i].metadata.result_schema =
          std::make_shared<const SchemaTemplate>(std::move(schema));
    }
    return Answer(std::move(outputs));
  };
  if (algorithm == ImageAlgorithm::Gaussian) {
    auto specialize = std::exchange(op.specialize_metadata, {});
    op.prepare_static =
        [specialize](const auto& inputs,
                     const auto& parameters) -> Result<OperationPreparation> {
      auto outputs = specialize(inputs, parameters);
      if (!outputs.ok())
        return Result<OperationPreparation>(outputs.status());
      input_internal::Float32Environment environment;
      if (!environment.active())
        return Result<OperationPreparation>(Status{
            ErrorCode::OperationFailed, "image Gaussian floating environment"});
      auto weights = std::make_shared<GaussianWeights>();
      weights->radius = static_cast<unsigned>(
          std::get<std::int64_t>(parameters.at("radius")));
      const double sigma = std::get<double>(parameters.at("sigma"));
      const int radius = static_cast<int>(weights->radius);
      double total = 0;
      for (int tap = -radius; tap <= radius; ++tap) {
        const double value =
            std::exp(-static_cast<double>(tap * tap) / (2.0 * sigma * sigma));
        total += value;
        weights->values[tap + radius] = value;
      }
      for (int tap = -radius; tap <= radius; ++tap)
        weights->values[tap + radius] /= total;
      OperationPreparation prepared;
      prepared.outputs = outputs.take_value();
      prepared.state = std::move(weights);
      return Result<OperationPreparation>(std::move(prepared));
    };
  }
  op.start_result = [algorithm](const ResultProgramQuery& query,
                                const BufferAllocator& allocator) {
    if (algorithm == ImageAlgorithm::Gaussian && query.tensor_outputs &&
        query.tensor_outputs->empty())
      return ResultContinuation::stateless<empty_gaussian>();
    const auto* weights =
        algorithm == ImageAlgorithm::Gaussian && query.prepared
            ? static_cast<const GaussianWeights*>(query.prepared->state())
            : nullptr;
    if (algorithm == ImageAlgorithm::Gaussian && !weights)
      return Result<ResultContinuation>(
          Status{ErrorCode::Internal, "missing image Gaussian preparation"});
    return ResultContinuation::make<Program>(allocator, algorithm, weights);
  };
  return registry->register_operation(std::move(op));
}
}  // namespace ps::plugin_internal::image_ops
