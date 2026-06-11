#include "kernel/services/compute-service/node_executor.hpp"

#include <algorithm>
#include <type_traits>

#include "adapter/buffer_adapter_opencv.hpp"
#include "kernel/param_utils.hpp"
#include "kernel/services/compute-service/compute_geometry.hpp"

namespace ps::compute {
namespace {

std::pair<int, DataType> infer_channels_and_type(
    const std::vector<const NodeOutput*>& inputs) {
  if (inputs.empty())
    return {1, DataType::FLOAT32};
  const auto& input = inputs.front()->image_buffer;
  return {input.channels, input.type};
}

cv::Size infer_output_size(const Node& node,
                           const std::vector<const NodeOutput*>& inputs,
                           const TiledExecutionConfig& config) {
  if (config.output_size)
    return *config.output_size;
  if (!inputs.empty()) {
    const auto& input = inputs.front()->image_buffer;
    return cv::Size(input.width, input.height);
  }
  return cv::Size(as_int_flexible(node.runtime_parameters, "width", 256),
                  as_int_flexible(node.runtime_parameters, "height", 256));
}

bool needs_gaussian_halo(const Node& node) {
  return node.type == "image_process" &&
         node.subtype.find("gaussian_blur") != std::string::npos;
}

[[noreturn]] void wrap_node_exception(const Node& node,
                                      const std::exception& e) {
  throw GraphError(GraphErrc::ComputeError,
                   "Node " + std::to_string(node.id) + " (" + node.name +
                       ") failed: " + std::string(e.what()));
}

}  // namespace

NodeOutput NodeExecutor::execute(GraphModel& graph, Node& node,
                                 const OpRegistry::OpVariant& op,
                                 const std::vector<const NodeOutput*>& inputs,
                                 const TiledExecutionConfig& config) {
  try {
    return std::visit(
        [&](auto&& op_func) -> NodeOutput {
          using T = std::decay_t<decltype(op_func)>;
          if constexpr (std::is_same_v<T, MonolithicOpFunc>) {
            return op_func(node, inputs);
          } else {
            TiledInputContext input_context =
                normalize_tiled_inputs(node, inputs);
            if (input_context.inputs.empty() &&
                node.type != "image_generator") {
              throw GraphError(GraphErrc::MissingDependency,
                               "Tiled node '" + node.name +
                                   "' requires at least one image input");
            }

            const cv::Size output_size =
                infer_output_size(node, input_context.inputs, config);
            auto [channels, dtype] =
                infer_channels_and_type(input_context.inputs);
            NodeOutput output;
            output.image_buffer = make_aligned_cpu_image_buffer(
                output_size.width, output_size.height, channels, dtype);

            TileTask task;
            task.node = &node;
            task.output_tile.buffer = &output.image_buffer;

            const cv::Rect full_roi(0, 0, output.image_buffer.width,
                                    output.image_buffer.height);
            const cv::Rect work_roi =
                config.output_roi ? clip_rect(*config.output_roi, output_size)
                                  : full_roi;
            for (int y = work_roi.y; y < work_roi.y + work_roi.height;
                 y += config.tile_size) {
              for (int x = work_roi.x; x < work_roi.x + work_roi.width;
                   x += config.tile_size) {
                task.output_tile.roi = clip_rect(
                    cv::Rect(x, y,
                             std::min(config.tile_size,
                                      work_roi.x + work_roi.width - x),
                             std::min(config.tile_size,
                                      work_roi.y + work_roi.height - y)),
                    output_size);
                if (is_rect_empty(task.output_tile.roi))
                  continue;
                task.input_tiles.clear();
                for (const auto* input : input_context.inputs) {
                  Tile input_tile;
                  input_tile.buffer =
                      const_cast<ImageBuffer*>(&input->image_buffer);
                  input_tile.roi =
                      input_roi_for_tile(graph, node, task.output_tile.roi,
                                         input->image_buffer, config);
                  task.input_tiles.push_back(input_tile);
                }
                execute_tile_task(task, op_func);
              }
            }
            return output;
          }
        },
        op);
  } catch (const GraphError&) {
    throw;
  } catch (const cv::Exception& e) {
    wrap_node_exception(node, e);
  } catch (const std::exception& e) {
    wrap_node_exception(node, e);
  } catch (...) {
    throw GraphError(GraphErrc::ComputeError,
                     "Node " + std::to_string(node.id) + " (" + node.name +
                         ") failed: unknown exception");
  }
}

TiledInputContext NodeExecutor::normalize_tiled_inputs(
    Node& node, const std::vector<const NodeOutput*>& inputs) {
  TiledInputContext context;
  context.inputs = inputs;
  const bool is_mixing = node.type == "image_mixing";
  if (!is_mixing || inputs.size() < 2)
    return context;

  const auto& base_buffer = inputs[0]->image_buffer;
  if (base_buffer.width == 0 || base_buffer.height == 0) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Base image for image_mixing node " +
                         std::to_string(node.id) + " is empty.");
  }

  const int base_w = base_buffer.width;
  const int base_h = base_buffer.height;
  const int base_c = base_buffer.channels;
  const std::string strategy =
      as_str(node.runtime_parameters, "merge_strategy", "resize");
  context.normalized_storage.reserve(inputs.size() - 1);

  for (size_t i = 1; i < inputs.size(); ++i) {
    const auto& current_buffer = inputs[i]->image_buffer;
    if (current_buffer.width == 0 || current_buffer.height == 0) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "Secondary image for image_mixing node " +
                           std::to_string(node.id) + " is empty.");
    }
    if (current_buffer.width == base_w && current_buffer.height == base_h &&
        current_buffer.channels == base_c) {
      continue;
    }

    cv::Mat current_mat = toCvMat(current_buffer);
    if (current_mat.cols != base_w || current_mat.rows != base_h) {
      if (strategy == "resize") {
        cv::resize(current_mat, current_mat, cv::Size(base_w, base_h), 0, 0,
                   cv::INTER_LINEAR);
      } else if (strategy == "crop") {
        cv::Rect crop_roi(0, 0, std::min(current_mat.cols, base_w),
                          std::min(current_mat.rows, base_h));
        cv::Mat cropped = cv::Mat::zeros(base_h, base_w, current_mat.type());
        current_mat(crop_roi).copyTo(cropped(crop_roi));
        current_mat = cropped;
      } else {
        throw GraphError(GraphErrc::InvalidParameter,
                         "Unsupported merge_strategy '" + strategy +
                             "' for tiled image_mixing.");
      }
    }

    if (current_mat.channels() != base_c) {
      if (current_mat.channels() == 1 && (base_c == 3 || base_c == 4)) {
        std::vector<cv::Mat> planes(base_c, current_mat);
        cv::merge(planes, current_mat);
      } else if ((current_mat.channels() == 3 || current_mat.channels() == 4) &&
                 base_c == 1) {
        cv::cvtColor(current_mat, current_mat, cv::COLOR_BGR2GRAY);
      } else if (current_mat.channels() == 4 && base_c == 3) {
        cv::cvtColor(current_mat, current_mat, cv::COLOR_BGRA2BGR);
      } else if (current_mat.channels() == 3 && base_c == 4) {
        cv::cvtColor(current_mat, current_mat, cv::COLOR_BGR2BGRA);
      } else if (current_mat.channels() != base_c) {
        throw GraphError(GraphErrc::InvalidParameter,
                         "Unsupported channel conversion for image_mixing: " +
                             std::to_string(current_mat.channels()) + " -> " +
                             std::to_string(base_c));
      }
    }

    NodeOutput temp_output;
    temp_output.image_buffer = fromCvMat(current_mat);
    context.normalized_storage.push_back(std::move(temp_output));
    context.inputs[i] = &context.normalized_storage.back();
  }

  return context;
}

cv::Rect NodeExecutor::input_roi_for_tile(GraphModel& graph, const Node& node,
                                          const cv::Rect& output_roi,
                                          const ImageBuffer& input_buffer,
                                          const TiledExecutionConfig& config) {
  OpMetadata meta;
  if (config.metadata) {
    meta = *config.metadata;
  } else if (auto op_meta =
                 OpRegistry::instance().get_metadata(node.type, node.subtype)) {
    meta = *op_meta;
  }

  cv::Rect input_roi;
  if (meta.access_pattern == OpMetadata::InputAccessPattern::RandomAccess) {
    auto prop_fn =
        OpRegistry::instance().get_dirty_propagator(node.type, node.subtype);
    input_roi = prop_fn(node, output_roi, graph);
  } else if (config.forced_halo.value_or(
                 needs_gaussian_halo(node) ? config.halo_size : 0) > 0) {
    input_roi =
        expand_rect(output_roi, config.forced_halo.value_or(config.halo_size));
  } else {
    input_roi = output_roi;
  }

  input_roi =
      clip_rect(input_roi, cv::Size(input_buffer.width, input_buffer.height));
  if (is_rect_empty(input_roi)) {
    input_roi = clip_rect(output_roi,
                          cv::Size(input_buffer.width, input_buffer.height));
  }
  return input_roi;
}

void NodeExecutor::execute_tile_task(const TileTask& task,
                                     const TileOpFunc& tiled_op) {
  tiled_op(*task.node, task.output_tile, task.input_tiles);
}

}  // namespace ps::compute
