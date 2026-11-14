#include <string>
#include <utility>
#include <vector>

#include "common/codec.hpp"

namespace ps::tools {
ResourceSet read_resources(const std::string& manifest, Files* files,
                           const ResourceBudget& root) {
  if (manifest.empty())
    return {};
  JsonDocument document(files->read(manifest));
  auto json = document.root();
  json.fields({"format", "version", "resources"});
  format(json, "photospider.resources");
  std::vector<ImmutableResource> resources;
  for (auto item : json.at("resources").array()) {
    item.fields({"path", "byte_length", "sha256"});
    auto size = item.at("byte_length").u64();
    auto expected = item.at("sha256").text();
    if (expected.size() != 64)
      throw ToolError("SHA-256 requires 64 hexadecimal digits");
    if (!size || size > 64U * 1024U * 1024U)
      throw ToolError("resource size must be 1..64 MiB");
    auto capacity = ResourceCapacity::host(size * 2);
    auto lease = take(root.reserve(capacity));
    auto path = relative(manifest, item.at("path").text());
    auto data = files->read(path, size);
    if (data.size() != size)
      throw ToolError("resource byte length mismatch: " + path.string());
    auto resource = take(ImmutableResource::import(
        ByteView(reinterpret_cast<const std::uint8_t*>(data.data()),
                 data.size()),
        root));
    std::string hash;
    const char* hex = "0123456789abcdef";
    for (auto byte : resource.identity().sha256) {
      hash += hex[byte >> 4];
      hash += hex[byte & 15];
    }
    if (hash != expected)
      throw ToolError("resource SHA-256 mismatch: " + path.string());
    resources.push_back(std::move(resource));
  }
  return take(ResourceSet::create(resources, root));
}
}  // namespace ps::tools
