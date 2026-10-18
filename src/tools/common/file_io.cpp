#include "common/file_io.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <system_error>

#include "common/process.hpp"

#if defined(__APPLE__)
#include <stdio.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#if !defined(_WIN32)
#include <poll.h>
#include <unistd.h>
#endif

namespace ps::tools {
namespace {
fs::path normalized(const fs::path& path) {
  return fs::weakly_canonical(fs::absolute(path));
}
bool same(const fs::path& a, const fs::path& b) {
  std::error_code error;
  return normalized(a) == normalized(b) ||
         (fs::equivalent(a, b, error) && !error);
}
}  // namespace
void publish(const fs::path& from, const fs::path& to, bool exclusive) {
#if defined(__APPLE__)
  if (exclusive) {
    if (renamex_np(from.c_str(), to.c_str(), RENAME_EXCL))
      throw fs::filesystem_error(
          "publish", from, to, std::error_code(errno, std::generic_category()));
    return;
  }
#elif defined(__linux__)
  if (exclusive) {
    if (syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 1))
      throw fs::filesystem_error(
          "publish", from, to, std::error_code(errno, std::generic_category()));
    return;
  }
#elif defined(_WIN32)
  if (!MoveFileExW(from.c_str(), to.c_str(),
                   exclusive ? 0 : MOVEFILE_REPLACE_EXISTING))
    throw fs::filesystem_error(
        "publish", from, to,
        std::error_code(GetLastError(), std::system_category()));
  return;
#else
  if (exclusive)
    throw ToolError(
        "atomic exclusive directory publication is unsupported on this "
        "platform",
        4);
#endif
  fs::rename(from, to);
}
void Files::protect(const fs::path& path) {
  if (path == "-")
    return;
  for (const auto& output : outputs)
    if (output != "-" && same(path, output))
      throw ToolError("output aliases input: " + path.string(), 2);
  inputs.push_back(normalized(path));
}
void Files::reserve(const fs::path& path) {
  for (const auto& output : outputs)
    if ((path == "-" && output == "-") ||
        (path != "-" && output != "-" && same(path, output)))
      throw ToolError("duplicate output path: " + path.string(), 2);
  if (path != "-")
    for (const auto& input : inputs)
      if (same(input, path))
        throw ToolError("output aliases input: " + path.string(), 2);
  outputs.push_back(path);
}
std::string Files::read(const fs::path& path, std::uint64_t limit) {
  protect(path);
  interrupted();
#if !defined(_WIN32)
  if (path == "-") {
    std::string result;
    char buffer[65536];
    for (;;) {
      interrupted();
      pollfd descriptor{STDIN_FILENO, POLLIN, 0};
      const int ready = poll(&descriptor, 1, 100);
      if (ready < 0 && errno == EINTR)
        continue;
      if (ready < 0)
        throw ToolError("stdin polling failed", 4, "PS-IO-READ");
      if (!ready)
        continue;
      const auto count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
      if (count < 0 && errno == EINTR)
        continue;
      interrupted();
      if (count < 0)
        throw ToolError("stdin read failed", 4, "PS-IO-READ");
      if (!count)
        return result;
      if (static_cast<std::uint64_t>(count) > limit - result.size())
        throw ToolError("input size limit exceeded: -");
      result.append(buffer, static_cast<std::size_t>(count));
    }
  }
#endif
  std::ifstream file;
  std::istream* stream = &std::cin;
  if (path != "-") {
    file.open(path, std::ios::binary);
    interrupted();
    if (!file)
      throw ToolError("cannot read " + path.string(), 4, "PS-IO-READ");
    stream = &file;
  }
  std::string result;
  char buffer[65536];
  while (stream->read(buffer, sizeof(buffer)) || stream->gcount()) {
    interrupted();
    auto count = static_cast<std::size_t>(stream->gcount());
    if (result.size() > limit || count > limit - result.size())
      throw ToolError("input size limit exceeded: " + path.string());
    result.append(buffer, count);
  }
  interrupted();
  if (stream->bad())
    throw ToolError("read failed: " + path.string(), 4, "PS-IO-READ");
  return result;
}
void Files::write(const fs::path& path, const std::string& bytes) const {
  interrupted();
  if (path == "-") {
    std::cout.write(bytes.data(), bytes.size());
    std::cout.flush();
    interrupted();
    if (!std::cout)
      throw ToolError("stdout write failed", 4, "PS-IO-WRITE");
    return;
  }
  for (const auto& input : inputs)
    if (same(input, path))
      throw ToolError("output aliases input: " + path.string(), 2);
  static std::atomic<std::uint64_t> sequence{0};
  auto parent = fs::absolute(path).parent_path();
  fs::path staging;
  for (int i = 0; i < 100; ++i) {
    auto id = std::chrono::steady_clock::now().time_since_epoch().count();
    staging = parent /
              (".psc-" + std::to_string(id) + "-" + std::to_string(sequence++));
    if (fs::create_directory(staging))
      break;
    staging.clear();
  }
  if (staging.empty())
    throw ToolError("cannot create output staging directory", 4);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup{staging};
  fs::permissions(staging, fs::perms::owner_all);
  auto temporary = staging / "artifact";
  std::ofstream stream(temporary, std::ios::binary);
  if (!stream)
    throw ToolError("cannot stage output: " + path.string(), 4, "PS-IO-WRITE");
  stream.write(bytes.data(), bytes.size());
  stream.close();
  if (!stream)
    throw ToolError("output write failed: " + path.string(), 4, "PS-IO-WRITE");
  interrupted();
  publish(temporary, path, false);
}
}  // namespace ps::tools
