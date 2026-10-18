#include "psx/result_export.hpp"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "common/codec.hpp"
#include "common/process.hpp"

namespace ps::tools::psx {
void export_results(const ExecutionResult& results, const fs::path& directory,
                    const Files& files, const CancellationToken& cancellation,
                    const ResourceBudget& budget) {
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
  w.put(root, "version", w.integer(std::uint64_t{1}));
  auto* outputs = w.array();
  w.put(root, "outputs", outputs);
  std::size_t index = 0;
  for (const auto& output : results.results) {
    const auto& value = output.second;
    auto facts = take(value.descriptor());
    auto* entry = w.object();
    w.push(outputs, entry);
    w.put(entry, "name",
          w.str(std::string(output.first.data(), output.first.size())));
    w.put(entry, "schema", write_schema(&w, value.schema()));
    auto* tensors = w.array();
    w.put(entry, "tensors", tensors);
    for (std::uint32_t slot = 0; slot < value.schema().tensors.size(); ++slot) {
      const auto& spec = value.schema().tensors[slot];
      auto* tensor = w.object();
      w.push(tensors, tensor);
      w.put(tensor, "slot", w.integer(static_cast<std::uint64_t>(slot)));
      w.put(tensor, "encoding", w.str("raw-le"));
      w.put(tensor, "shape", dimensions(&w, spec.sample_shape()));
      w.put(tensor, "dtype", w.str(dtype_name(spec.descriptor.element_type)));
      auto* fragments = w.array();
      w.put(tensor, "fragments", fragments);
      std::size_t fi = 0;
      std::vector<Region> pending;
      for (const auto& covered : facts.tensor_coverage(slot).boxes())
        pending.push_back(covered);
      while (!pending.empty()) {
        interrupted();
        auto box = std::move(pending.back());
        pending.pop_back();
        auto size = byte_count(box, spec.descriptor.element_type);
        if (size > 1024 * 1024) {
          auto left = box.dimensions();
          auto right = left;
          std::size_t axis = 0;
          while (left[axis].extent < 2)
            ++axis;
          left[axis].extent /= 2;
          right[axis].offset += left[axis].extent;
          right[axis].extent -= left[axis].extent;
          pending.emplace_back(std::move(right));
          pending.emplace_back(std::move(left));
          continue;
        }
        if (fi >= 1048576)
          throw ToolError("result export fragment limit exceeded");
        auto capacity = ResourceCapacity::host(size + 1);
        capacity[ResourceKind::Payload] = size + 1;
        auto lease = take(budget.reserve(capacity));
        std::string bytes(size, '\0');
        auto window =
            take(value.acquire_tensor(facts, slot, box, cancellation));
        check(window.copy_to(reinterpret_cast<std::uint8_t*>(bytes.data()),
                             bytes.size()));
        endian(&bytes, TensorSpan::element_size(spec.descriptor.element_type));
        auto name = "output-" + std::to_string(index) + "-tensor-" +
                    std::to_string(slot) + "-fragment-" + std::to_string(fi++) +
                    ".bin";
        files.write(staging / name, bytes);
        auto* fragment = w.object();
        w.push(fragments, fragment);
        w.put(fragment, "path", w.str(name));
        w.put(fragment, "byte_length", w.integer(size));
        std::vector<std::uint64_t> offset, extent;
        for (const auto& dim : box.dimensions()) {
          offset.push_back(dim.offset);
          extent.push_back(dim.extent);
        }
        w.put(fragment, "offset", dimensions(&w, offset));
        w.put(fragment, "shape", dimensions(&w, extent));
      }
    }
    ++index;
  }
  files.write(staging / "manifest.json", w.finish(root, true));
  interrupted();
  if (fs::exists(directory))
    throw ToolError("result directory appeared during export", 4);
  publish(staging, directory, true);
}
}  // namespace ps::tools::psx
