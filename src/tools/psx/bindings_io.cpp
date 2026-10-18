#include "psx/bindings_io.hpp"

#include <map>
#include <string>
#include <utility>

#include "common/codec.hpp"

namespace ps::tools::psx {
ExecutionBindings read_bindings(const std::string& manifest,
                                const ExecutionPlan& plan, Files* files,
                                const ResourceBudget& root) {
  ExecutionBindings result;
  if (manifest.empty()) {
    if (!plan.input_declarations().empty())
      throw ToolError("Plan requires --bindings");
    return result;
  }
  JsonDocument document(files->read(manifest));
  auto json = document.root();
  json.fields({"format", "version", "inputs"});
  format(json, "photospider.bindings");
  std::map<std::string, Json> inputs;
  for (auto item : json.at("inputs").array()) {
    item.fields({"name", "tensors"});
    if (!inputs.emplace(item.at("name").text(), item).second)
      throw ToolError("duplicate input binding");
  }
  if (inputs.size() != plan.input_declarations().size())
    throw ToolError("binding count mismatch");
  for (const auto& declaration : plan.input_declarations()) {
    auto found = inputs.find(declaration.name);
    if (found == inputs.end())
      throw ToolError("missing input binding: " + declaration.name);
    auto builder =
        take(ResultBuilder::start(root, *declaration.result_schema,
                                  "psx.binding", {}, plan.tile_extent()));
    std::map<std::uint32_t, Json> tensors;
    for (auto item : found->second.at("tensors").array()) {
      item.fields({"slot", "path", "encoding", "dtype", "shape"});
      if (!tensors.emplace(u32(item.at("slot")), item).second)
        throw ToolError("duplicate binding slot");
    }
    if (tensors.size() != declaration.result_schema->tensors.size())
      throw ToolError("binding tensor count mismatch");
    for (std::uint32_t slot = 0;
         slot < declaration.result_schema->tensors.size(); ++slot) {
      if (!tensors.count(slot))
        throw ToolError("missing binding slot");
      auto item = tensors.at(slot);
      const auto& spec = declaration.result_schema->tensors[slot];
      if (item.at("encoding").text() != "raw-le" ||
          dtype(item.at("dtype").text()) != spec.descriptor.element_type ||
          shape(item.at("shape")) != spec.sample_shape())
        throw ToolError("binding descriptor mismatch: " + declaration.name);
      auto region = Region::whole(spec.sample_shape());
      auto size = byte_count(region, spec.descriptor.element_type);
      if (size > UINT64_MAX / 2)
        throw ToolError("binding size exceeds capacity");
      auto capacity = ResourceCapacity::host(size * 2);
      capacity[ResourceKind::Payload] = size * 2;
      auto lease = take(root.reserve(capacity));
      auto bytes =
          files->read(relative(manifest, item.at("path").text()), size);
      if (bytes.size() != size)
        throw ToolError("binding byte length mismatch");
      endian(&bytes, TensorSpan::element_size(spec.descriptor.element_type));
      check(builder.publish_tensor(
          slot, region,
          ByteView(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                   bytes.size())));
    }
    result.inputs.push_back({declaration.name, take(builder.seal())});
  }
  return result;
}
}  // namespace ps::tools::psx
