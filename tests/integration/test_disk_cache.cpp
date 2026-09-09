#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "s3_image_workflow/scene.hpp"
#include "support/test_support.hpp"

int main(int argc, char** argv) {
  using namespace ps;  // NOLINT(build/namespaces)
  PS_CHECK(argc == 3);
  const std::string mode = argv[1], directory = argv[2];
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
