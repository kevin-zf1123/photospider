#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/file_io.hpp"
#include "common/json.hpp"
#include "photospider/data/resource_set.hpp"
#include "photospider/data/result.hpp"
#include "photospider/data/tensor_span.hpp"
#include "photospider/data/tiles.hpp"

namespace ps::tools {
std::uint32_t u32(Json value);
std::vector<std::uint64_t> shape(Json value);
ElementType dtype(const std::string& name);
std::string dtype_name(ElementType type);
std::shared_ptr<const SchemaTemplate> schema(Json json);
void format(Json root, const std::string& name);
fs::path relative(const std::string& manifest, const std::string& path);
std::uint64_t byte_count(const Region& region, ElementType type);
void endian(std::string* bytes, std::size_t width);
yyjson_mut_val* dimensions(JsonWriter* w,
                           const std::vector<std::uint64_t>& values);
yyjson_mut_val* write_schema(JsonWriter* w, const SchemaTemplate& schema);
ResourceSet read_resources(const std::string& manifest, Files* files,
                           const ResourceBudget& root);
std::map<std::string, TileRequest> read_requests(const std::string& manifest,
                                                 Files* files);
}  // namespace ps::tools
