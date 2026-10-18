#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/codec.hpp"

namespace ps::tools {
std::uint32_t u32(Json value) {
  auto n = value.u64();
  if (n > UINT32_MAX)
    throw ToolError("integer exceeds uint32");
  return static_cast<std::uint32_t>(n);
}
std::vector<std::uint64_t> shape(Json value) {
  auto items = value.array();
  if (items.size() > 8)
    throw ToolError("tensor rank exceeds eight");
  std::vector<std::uint64_t> result;
  for (auto item : items) {
    auto n = item.u64();
    if (!n)
      throw ToolError("zero tensor extent");
    result.push_back(n);
  }
  return result;
}
ElementType dtype(const std::string& name) {
  if (name == "u8")
    return ElementType::UInt8;
  if (name == "i8")
    return ElementType::Int8;
  if (name == "u16")
    return ElementType::UInt16;
  if (name == "i16")
    return ElementType::Int16;
  if (name == "i64")
    return ElementType::Int64;
  if (name == "f32")
    return ElementType::Float32;
  if (name == "f64")
    return ElementType::Float64;
  throw ToolError("unknown tensor dtype: " + name);
}
std::string dtype_name(ElementType type) {
  switch (type) {
    case ElementType::UInt8:
      return "u8";
    case ElementType::Int8:
      return "i8";
    case ElementType::UInt16:
      return "u16";
    case ElementType::Int16:
      return "i16";
    case ElementType::Int64:
      return "i64";
    case ElementType::Float32:
      return "f32";
    case ElementType::Float64:
      return "f64";
  }
  throw ToolError("unknown tensor dtype");
}
std::shared_ptr<const SchemaTemplate> schema(Json json) {
  json.fields({"id", "version", "tensors", "publication_scopes"});
  auto result = std::make_shared<SchemaTemplate>();
  auto id = json.at("id").text();
  result->id.assign(id.begin(), id.end());
  result->version = u32(json.at("version"));
  auto tensors = json.at("tensors").array();
  if (tensors.empty() || tensors.size() > 16)
    throw ToolError("schema requires 1..16 tensors");
  for (auto tensor : tensors) {
    tensor.fields({"key", "dtype", "shape", "batch", "atomic", "layout"});
    ResultTensorSpec spec;
    auto key = tensor.at("key").text();
    spec.key.assign(key.begin(), key.end());
    spec.descriptor = {dtype(tensor.at("dtype").text()),
                       shape(tensor.at("shape"))};
    if (auto batch = tensor.optional("batch")) {
      auto dims = shape(batch);
      spec.batch_axes.assign(dims.begin(), dims.end());
    }
    if (auto atomic = tensor.optional("atomic"))
      spec.atomic_trailing_axes = u32(atomic);
    spec.layout = {false, 0, 0, {}};
    if (auto layout = tensor.optional("layout")) {
      layout.fields({"spatial", "height_axis", "width_axis", "channel_axis"});
      spec.layout.spatial = layout.at("spatial").boolean();
      if (spec.layout.spatial) {
        spec.layout.height_axis = u32(layout.at("height_axis"));
        spec.layout.width_axis = u32(layout.at("width_axis"));
        if (auto channel = layout.optional("channel_axis");
            channel && !channel.null())
          spec.layout.channel_axis = u32(channel);
      } else {
        layout.fields({"spatial"});
      }
    }
    result->tensors.push_back(std::move(spec));
  }
  if (auto scopes = json.optional("publication_scopes")) {
    for (auto item : scopes.array()) {
      item.fields({"policy", "members"});
      ResultPublicationScope scope;
      auto policy = item.at("policy").text();
      if (policy == "complete_bundle")
        scope.policy = ResultPublicationPolicy::CompleteBundle;
      else if (policy == "independent_chunks")
        scope.policy = ResultPublicationPolicy::IndependentChunks;
      else if (policy == "stable_prefix")
        scope.policy = ResultPublicationPolicy::StablePrefix;
      else
        throw ToolError("invalid publication policy");
      for (auto member : item.at("members").array())
        scope.members.push_back(u32(member));
      result->publication_scopes.push_back(std::move(scope));
    }
  }
  check(result->validate());
  return result;
}
void format(Json root, const std::string& name) {
  if (root.at("format").text() != name || root.at("version").u64() != 1)
    throw ToolError("unsupported " + name + " format/version");
}
fs::path relative(const std::string& manifest, const std::string& path) {
  if (path.find('\0') != std::string::npos || path.empty() || path == "-")
    throw ToolError("invalid manifest file path");
  return fs::path(manifest).parent_path() / fs::path(path);
}
std::uint64_t byte_count(const Region& region, ElementType type) {
  auto count = take(region.element_count());
  auto width = TensorSpan::element_size(type);
  if (count > SIZE_MAX / width)
    throw ToolError("tensor byte size overflow");
  return count * width;
}
void endian(std::string* bytes, std::size_t width) {
  std::uint16_t marker = 1;
  if (*reinterpret_cast<unsigned char*>(&marker) == 1 || width == 1)
    return;
  for (std::size_t i = 0; i < bytes->size(); i += width)
    std::reverse(bytes->begin() + i, bytes->begin() + i + width);
}
yyjson_mut_val* dimensions(JsonWriter* w,
                           const std::vector<std::uint64_t>& values) {
  auto* a = w->array();
  for (auto n : values)
    w->push(a, w->integer(n));
  return a;
}
yyjson_mut_val* write_schema(JsonWriter* w, const SchemaTemplate& schema) {
  auto* root = w->object();
  w->put(root, "id", w->str(std::string(schema.id.data(), schema.id.size())));
  w->put(root, "version",
         w->integer(static_cast<std::uint64_t>(schema.version)));
  auto* tensors = w->array();
  w->put(root, "tensors", tensors);
  for (const auto& tensor : schema.tensors) {
    auto* item = w->object();
    w->push(tensors, item);
    w->put(item, "key",
           w->str(std::string(tensor.key.data(), tensor.key.size())));
    w->put(item, "dtype", w->str(dtype_name(tensor.descriptor.element_type)));
    w->put(item, "shape", dimensions(w, tensor.descriptor.shape));
    w->put(item, "batch",
           dimensions(w, {tensor.batch_axes.begin(), tensor.batch_axes.end()}));
    w->put(item, "atomic",
           w->integer(static_cast<std::uint64_t>(tensor.atomic_trailing_axes)));
    auto* layout = w->object();
    w->put(item, "layout", layout);
    w->put(layout, "spatial", w->boolean(tensor.layout.spatial));
    if (tensor.layout.spatial) {
      w->put(layout, "height_axis",
             w->integer(static_cast<std::uint64_t>(tensor.layout.height_axis)));
      w->put(layout, "width_axis",
             w->integer(static_cast<std::uint64_t>(tensor.layout.width_axis)));
      if (tensor.layout.channel_axis)
        w->put(layout, "channel_axis",
               w->integer(
                   static_cast<std::uint64_t>(*tensor.layout.channel_axis)));
    }
  }
  auto* scopes = w->array();
  w->put(root, "publication_scopes", scopes);
  for (const auto& scope : schema.publication_scopes) {
    auto* item = w->object();
    w->push(scopes, item);
    w->put(item, "policy",
           w->str(scope.policy == ResultPublicationPolicy::CompleteBundle
                      ? "complete_bundle"
                  : scope.policy == ResultPublicationPolicy::StablePrefix
                      ? "stable_prefix"
                      : "independent_chunks"));
    auto* members = w->array();
    w->put(item, "members", members);
    for (auto n : scope.members)
      w->push(members, w->integer(static_cast<std::uint64_t>(n)));
  }
  return root;
}
}  // namespace ps::tools
