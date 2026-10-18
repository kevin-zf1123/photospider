#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "common/error.hpp"

namespace ps::tools {
namespace fs = std::filesystem;
void publish(const fs::path& from, const fs::path& to, bool exclusive);
struct Files final {
  std::vector<fs::path> inputs, outputs;
  std::string read(const fs::path& path,
                   std::uint64_t limit = 64U * 1024U * 1024U);
  void protect(const fs::path& path);
  void reserve(const fs::path& path);
  void write(const fs::path& path, const std::string& bytes) const;
};
}  // namespace ps::tools
