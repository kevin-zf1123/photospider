#include "psx/result_export.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "common/codec.hpp"
#include "common/process.hpp"
#include "data/physical_piece.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/tools/result_files.hpp"

namespace ps::tools::psx {
namespace {
constexpr std::uint64_t kHeaderBytes = 24;
constexpr std::uint64_t kTileRecordBytes = 24;
void write_u64_le(std::ostream& stream, std::uint64_t value) {
  std::uint8_t bytes[8];
  for (unsigned byte = 0; byte < 8; ++byte)
    bytes[byte] = static_cast<std::uint8_t>(value >> (8 * byte));
  stream.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}
void write_header(std::ostream& stream, std::uint64_t tile_extent,
                  const TileRequest& selected,
                  const ResourceVector<std::uint64_t>& lengths,
                  const CancellationToken& cancellation) {
  stream.seekp(0);
  stream.write("PSXTILE2", 8);
  write_u64_le(stream, tile_extent);
  write_u64_le(stream, selected.size());
  std::size_t index = 0;
  for (const auto& member : selected.members)
    for (auto id : member.second) {
      interrupted();
      if (cancellation.cancelled())
        check(Status{ErrorCode::Cancelled, {}});
      write_u64_le(stream, member.first);
      write_u64_le(stream, id);
      write_u64_le(stream, lengths.at(index++));
    }
  if (!stream)
    throw ToolError("result header write failed", 4, "PS-IO-WRITE");
}
// All writes for this tile are adjacent. Only the I/O batch size depends
// on the temporary buffer; the tile directory contains one record either way.
void write_tile(std::ostream& stream, const ResultTensorReadWindow& window,
                const TensorSpan& span, std::uint64_t payload,
                MutableBuffer& buffer, const CancellationToken& cancellation,
                const ResourceBudget& budget) {
  if (payload <= buffer.size()) {
    check(window.copy_to(buffer.data(), payload));
    stream.write(reinterpret_cast<const char*>(buffer.data()),
                 static_cast<std::streamsize>(payload));
  } else {
    const auto source = take(data_internal::physical_piece(span));
    if (source.size() != payload)
      throw ToolError("physical tile length differs from directory");
    const auto width = TensorSpan::element_size(span.descriptor().element_type);
    for (std::uint64_t offset = 0; offset < payload;) {
      interrupted();
      if (cancellation.cancelled())
        check(Status{ErrorCode::Cancelled, {}});
      const auto n = std::min<std::uint64_t>(payload - offset, buffer.size());
      // Charge a sample when its first byte enters a batch, even for a
      // buffer smaller than one scalar.
      const auto end = offset + n;
      const auto first_sample = offset / width + (offset % width != 0);
      const auto past_sample = (end - 1) / width + 1;
      check(budget.consume({past_sample - first_sample}));
      std::memcpy(buffer.data(), source.data() + offset, n);
      stream.write(reinterpret_cast<const char*>(buffer.data()),
                   static_cast<std::streamsize>(n));
      if (!stream)
        break;
      offset = end;
    }
  }
  if (!stream)
    throw ToolError("result payload write failed", 4, "PS-IO-WRITE");
}
struct OutputSelection {
  const PlanOutput& output;
  const TileRequest& tiles;
};
void write_directory(
    std::uint64_t tile_extent,
    const std::function<OutputSelection(const std::string&)>& selection,
    const ExecutionResult& results, const fs::path& directory,
    const Files& files, const CancellationToken& cancellation,
    const ResourceBudget& budget, std::uint64_t buffer_limit) {
  if (!buffer_limit)
    throw ToolError("result export buffer must be positive");
  if (directory == "-" || fs::exists(directory))
    throw ToolError("result directory must be new: " + directory.string(), 4);
  auto parent = fs::absolute(directory).parent_path();
  auto staging =
      parent /
      (".psx-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  if (!fs::create_directory(staging))
    throw ToolError("cannot stage results", 4);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup{staging};
  fs::permissions(staging, fs::perms::owner_all);
  JsonWriter w;
  auto* root = w.object();
  w.put(root, "format", w.str("photospider.results"));
  w.put(root, "version", w.integer(std::uint64_t{2}));
  const std::uint16_t endian_marker = 1;
  w.put(root, "byte_order",
        w.str(*reinterpret_cast<const std::uint8_t*>(&endian_marker) ? "little"
                                                                     : "big"));
  auto* outputs = w.array();
  w.put(root, "outputs", outputs);
  std::size_t index = 0;
  for (const auto& output : results.results) {
    const auto& value = output.second;
    auto facts = take(value.descriptor());
    auto* entry = w.object();
    w.push(outputs, entry);
    const std::string output_name(output.first.data(), output.first.size());
    w.put(entry, "name", w.str(output_name));
    const auto [output_plan, selected] = selection(output_name);
    const auto name = "output-" + std::to_string(index) + ".bin";
    w.put(entry, "path", w.str(name));
    // This stream is private until exclusive publication of the result
    // directory.
    std::ofstream stream(staging / name, std::ios::binary);
    if (!stream)
      throw ToolError("cannot stage result file: " + name, 4, "PS-IO-WRITE");
    // Reserve directory space; fill its lengths after observing each actual
    // backing once. Physical padding is not derivable from the logical shape.
    std::uint64_t position = 0;
    if (!core_internal::checked_multiply_add(selected.size(), kTileRecordBytes,
                                             kHeaderBytes, &position))
      throw ToolError("result tile directory exceeds file offset range");
    stream.seekp(static_cast<std::streamoff>(position));
    ResourceVector<std::uint64_t> lengths{
        ResourceAllocator<std::uint64_t>(budget)};
    lengths.reserve(selected.size());
    MutableBuffer buffer;
    w.put(entry, "schema", write_schema(&w, value.schema()));
    auto* tensors = w.array();
    w.put(entry, "tensors", tensors);
    for (std::uint32_t slot = 0; slot < value.schema().tensors.size(); ++slot) {
      const auto& spec = value.schema().tensors[slot];
      auto* tensor = w.object();
      w.push(tensors, tensor);
      w.put(tensor, "slot", w.integer(static_cast<std::uint64_t>(slot)));
      w.put(tensor, "encoding", w.str("physical-tile"));
      w.put(tensor, "shape", dimensions(&w, spec.sample_shape()));
      w.put(tensor, "dtype", w.str(dtype_name(spec.descriptor.element_type)));
      auto* tiles = w.array();
      w.put(tensor, "tiles", tiles);
      auto member = selected.members.find(slot);
      if (member == selected.members.end())
        continue;
      const auto& grid = output_plan.tile_grids.at(slot);
      for (auto id : member->second) {
        interrupted();
        auto box = take(grid.region(id));
        auto window =
            take(value.acquire_tensor(facts, slot, box, cancellation));
        auto span = take(window.affine_view());
        const auto payload = take(window.physical_byte_size());
        if (!core_internal::can_add(position, payload))
          throw ToolError("result file exceeds representable size");
        lengths.push_back(payload);
        const auto batch =
            std::min({payload, buffer_limit,
                      static_cast<std::uint64_t>(
                          std::numeric_limits<std::streamsize>::max())});
        if (buffer.size() < batch) {
          buffer = {};
          buffer = take(budget.allocator().allocate(batch));
        }
        write_tile(stream, window, span, payload, buffer, cancellation, budget);
        auto* tile = w.object();
        w.push(tiles, tile);
        w.put(tile, "index", w.integer(id));
        w.put(tile, "file_offset", w.integer(position));
        w.put(tile, "byte_length", w.integer(payload));
        position += payload;
        std::vector<std::uint64_t> offset, extent;
        for (const auto& dim : box.dimensions()) {
          offset.push_back(dim.offset);
          extent.push_back(dim.extent);
        }
        w.put(tile, "offset", dimensions(&w, offset));
        w.put(tile, "shape", dimensions(&w, extent));
        auto* strides = w.array();
        for (auto stride : span.layout().byte_strides)
          w.push(strides, w.integer(stride));
        w.put(tile, "byte_strides", strides);
      }
    }
    write_header(stream, tile_extent, selected, lengths, cancellation);
    stream.close();
    if (!stream)
      throw ToolError("result file close failed: " + name, 4, "PS-IO-WRITE");
    w.put(entry, "byte_length", w.integer(position));
    ++index;
  }
  files.write(staging / "manifest.json", w.finish(root, true));
  interrupted();
  if (fs::exists(directory))
    throw ToolError("result directory appeared during export", 4);
  publish(staging, directory, true);
}
}  // namespace
void export_results(const ExecutionPlan& plan, const ExecutionResult& results,
                    const fs::path& directory, const Files& files,
                    const CancellationToken& cancellation,
                    const ResourceBudget& budget, std::uint64_t buffer_limit) {
  write_directory(
      plan.tile_extent(),
      [&](const std::string& name) {
        return OutputSelection{plan.output(name),
                               plan.output_requests().at(name)};
      },
      results, directory, files, cancellation, budget, buffer_limit);
}
}  // namespace ps::tools::psx

namespace ps::tools {
Status export_job_results(const ExecutionPlan& plan,
                          const ExecutionResult& results,
                          const std::filesystem::path& directory,
                          const ResourceBudget& budget) try {
  psx::export_results(plan, results, directory, Files{}, {}, budget);
  return Status::success();
} catch (const ToolError& e) {
  return {e.exit_code == 4 && e.status.empty() ? ErrorCode::OperationFailed
                                               : e.kernel_code,
          e.what(), e.reason};
} catch (const std::filesystem::filesystem_error& e) {
  return {ErrorCode::OperationFailed, e.what()};
} catch (const std::bad_alloc&) {
  return {ErrorCode::ResourceExhausted, {}};
}
Status export_tiled_image(const ResultRef& image, std::uint64_t tile_extent,
                          const std::filesystem::path& directory,
                          const ResourceBudget& budget) try {
  if (!image.valid() || !tile_extent || (tile_extent & (tile_extent - 1)))
    return {ErrorCode::InvalidArgument, "invalid image or tile extent"};
  PlanOutput output;
  output.name = "image";
  output.result_schema = std::make_shared<const SchemaTemplate>(image.schema());
  output.tile_grids = take(TileGrid::members(image.schema(), tile_extent));
  // An imported image certifies complete coverage. Bound the grid before
  // directory construction or enumeration of untrusted dimensions.
  std::uint64_t count = 0;
  for (const auto& grid : output.tile_grids) {
    if (!core_internal::checked_add(count, grid.tile_count(), &count, 65536))
      return {ErrorCode::ResourceExhausted,
              "image exceeds export tile count limit"};
  }
  auto tiles = take(TileRequest::all(output.tile_grids, budget));
  ExecutionResult results;
  results.results.emplace("image", image);
  psx::write_directory(
      tile_extent,
      [&](const auto&) { return psx::OutputSelection{output, tiles}; }, results,
      directory, Files{}, {}, budget, psx::kDefaultExportBufferBytes);
  return Status::success();
} catch (const ToolError& e) {
  return {e.exit_code == 4 && e.status.empty() ? ErrorCode::OperationFailed
                                               : e.kernel_code,
          e.what(), e.reason};
} catch (const std::filesystem::filesystem_error& e) {
  return {ErrorCode::OperationFailed, e.what()};
} catch (const std::bad_alloc&) {
  return {ErrorCode::ResourceExhausted, {}};
}
}  // namespace ps::tools
