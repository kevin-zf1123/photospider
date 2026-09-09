#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "execution/disk_cache.hpp"
#include "s3_image_workflow/scene.hpp"
#include "support/test_support.hpp"

namespace {
int internal_regressions(const std::string& directory) {
  using namespace ps;  // NOLINT(build/namespaces)
  using execution_internal::DiskCache;
  std::filesystem::remove_all(directory);
  auto budget = std::make_shared<execution_internal::MemoryBudget>(12);
  DiskCacheConfig config{directory, 512, 1, 4};
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false, release = false;
  std::atomic<bool> gate{true}, fail_index{false};
  DiskCache cache(config, "test-implementation", budget, 4,
                  {[&] {
                     if (!gate)
                       return;
                     std::unique_lock<std::mutex> lock(mutex);
                     entered = true;
                     cv.notify_all();
                     cv.wait(lock, [&] { return release; });
                   },
                   [&] {
                     if (fail_index)
                       throw std::bad_alloc();
                   }});
  auto reservation = budget->reserve(4).take_value();
  auto writer =
      MutableValue::allocate({ElementType::Float32, {1, 1}},
                             Region::whole({1, 1}), reservation->allocator())
          .take_value();
  float half = .5F;
  std::memcpy(writer.data(), &half, 4);
  auto value =
      std::move(writer).publish({{"photospider.mask", 1, {}}}).take_value();
  reservation->seal();
  cache.put("active", value, OperationPortKind::Float32Mask);
  value = {};
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return entered; });
  }
  // The blocked writer has sole ownership of these four accounted bytes.
  PS_CHECK(budget->available() == 8);
  cache.drop_pending();
  auto full_working_set = budget->reserve(12);
  PS_CHECK(full_working_set.ok());
  full_working_set.value()->seal();
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  cv.notify_all();
  cache.flush();
  PS_CHECK(cache.statistics().entries == 0);
  gate = false;
  auto unaccounted =
      MutableValue::allocate({ElementType::Float32, {1, 1}},
                             Region::whole({1, 1}), BufferAllocator{})
          .take_value();
  std::memcpy(unaccounted.data(), &half, 4);
  value = std::move(unaccounted)
              .publish({{"photospider.mask", 1, {}}})
              .take_value();
  fail_index = true;
  cache.put("allocation-failure", value, OperationPortKind::Float32Mask);
  cache.flush();
  PS_CHECK(cache.statistics().write_failures == 1 &&
           cache.statistics().entries == 0);
  for (const auto& file : std::filesystem::directory_iterator(directory))
    PS_CHECK(file.path().extension() != ".pscache" &&
             file.path().extension() != ".tmp");
  fail_index = false;
  const auto sentinel = directory + "-sentinel";
  {
    std::ofstream file(sentinel);
    file << "unchanged";
  }
  const auto link =
      std::filesystem::path(directory) / (cache.disk_key("symlink") + ".tmp");
  std::error_code ec;
  std::filesystem::create_symlink(std::filesystem::absolute(sentinel), link,
                                  ec);
#if !defined(_WIN32)
  PS_CHECK(!ec);
#endif
  if (!ec) {
    cache.put("symlink", value, OperationPortKind::Float32Mask);
    cache.flush();
    std::ifstream file(sentinel);
    std::string text;
    file >> text;
    PS_CHECK(text == "unchanged" && cache.statistics().entries == 0);
    std::filesystem::remove(link);
  }
  std::filesystem::remove(sentinel);
  for (const auto& key : {"first", "second"}) {
    cache.put(key, value, OperationPortKind::Float32Mask);
    cache.flush();
    std::uint64_t actual_bytes = 0, actual_entries = 0;
    for (const auto& file : std::filesystem::directory_iterator(directory))
      if (file.path().extension() == ".pscache") {
        actual_bytes += file.file_size();
        ++actual_entries;
      }
    PS_CHECK(actual_entries == 1 &&
             actual_entries == cache.statistics().entries);
    PS_CHECK(actual_bytes == cache.statistics().retained_bytes &&
             actual_bytes <= 512);
  }
  cache.clear();
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  using namespace ps;  // NOLINT(build/namespaces)
  PS_CHECK(argc == 3);
  const std::string mode = argv[1], directory = argv[2];
  if (mode == "internal") {
    return internal_regressions(directory);
  }
  auto registry = make_default_operation_registry();
  PS_CHECK(registry->persistent_cache_identity().size() == 64);
  PS_CHECK(std::make_shared<OperationRegistry>()
               ->persistent_cache_identity()
               .empty());
  const bool damaged = mode == "corrupt" || mode == "truncate" ||
                       mode == "checksum" || mode == "version";
  if (damaged) {
    for (const auto& file : std::filesystem::directory_iterator(directory)) {
      if (file.path().extension() != ".pscache")
        continue;
      if (mode == "truncate") {
        std::filesystem::resize_file(file.path(), file.file_size() - 1);
        continue;
      }
      std::fstream output(file.path(),
                          std::ios::in | std::ios::out | std::ios::binary);
      if (mode == "version")
        output.seekp(8);
      if (mode == "checksum")
        output.seekp(-1, std::ios::end);
      output.put('!');
    }
  }
  ExecutionContextConfig config{2, false, 16, 1024 * 1024, 256 * 1024};
  config.disk_cache =
      DiskCacheConfig{directory, mode == "quota" ? 1024U : 1024U * 1024U,
                      mode == "quota" ? 2U : 4096U, 512};
  if (mode == "pressure")
    config.result_cache_bytes = 1;
  ExecutionContext execution(registry, config);
  if (mode == "pressure")
    execution.clear_disk_cache();
  bool exclusive = false;
  try {
    ExecutionContext conflicting(registry, config);
  } catch (const std::invalid_argument&) {
    exclusive = true;
  }
  PS_CHECK(exclusive);
  if (mode == "write_failure") {
    std::vector<std::filesystem::path> temporary;
    for (const auto& file : std::filesystem::directory_iterator(directory))
      if (file.path().extension() == ".pscache")
        temporary.push_back(file.path().parent_path() /
                            (file.path().stem().string() + ".tmp"));
    execution.clear_disk_cache();
    for (const auto& path : temporary)
      std::filesystem::create_directory(path);
  }
  s3::Scene scene(registry);
  auto frozen = scene.freeze(execution, 2);
  auto result = execution.execute(frozen);
  PS_CHECK(result.ok());
  s3::Frame frame(s3::Scene::height, s3::Scene::width);
  frame.blit(result.value().values.at("result"));
  frame.check(scene.oracle(2));
  execution.flush_disk_cache();
  auto stats = execution.disk_cache_statistics();
  PS_CHECK(stats.retained_bytes <= config.disk_cache->maximum_bytes &&
           stats.entries <= config.disk_cache->maximum_entries &&
           stats.queued_writes == 0);
  if (mode == "write_failure")
    PS_CHECK(stats.write_failures > 0);
  if (mode == "write")
    PS_CHECK(stats.entries > 0);
  if (mode == "read")
    PS_CHECK(stats.hits > 0 &&
             result.value().diagnostics.operation_timings.empty());
  if (damaged)
    PS_CHECK(stats.invalid_entries > 0 &&
             !result.value().diagnostics.operation_timings.empty());
  if (mode == "pressure")
    PS_CHECK(stats.dropped_writes > 0 && stats.entries == 0);
  if (mode == "delete") {
    execution.clear_result_cache();
    execution.clear_disk_cache();
    PS_CHECK(execution.disk_cache_statistics().entries == 0);
    auto rebuilt = execution.execute(frozen);
    PS_CHECK(rebuilt.ok());
    s3::Frame actual(s3::Scene::height, s3::Scene::width);
    actual.blit(rebuilt.value().values.at("result"));
    actual.check(scene.oracle(2));
    execution.flush_disk_cache();
  }
  std::cout << "S3Disk.DiscardAndRebuild mode=" << mode
            << " hits=" << stats.hits << " invalid=" << stats.invalid_entries
            << " disk_bytes=" << stats.retained_bytes << " oracle=passed\n";
  return 0;
}
