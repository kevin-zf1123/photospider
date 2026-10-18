#include "common/environment.hpp"

#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "common/json.hpp"
#if defined(PHOTOSPIDER_CLI_BUILTINS)
#include "photospider/ops/registry.hpp"
#endif

namespace ps::tools {
std::shared_ptr<OperationRegistry> load_operations(
    const std::vector<std::string>& libraries, Files* files) {
#if defined(PHOTOSPIDER_CLI_BUILTINS)
  auto result = make_default_operation_registry(false);
#else
  auto result = std::make_shared<OperationRegistry>();
#endif
  std::set<fs::path> loaded;
  for (const auto& library : libraries) {
    files->protect(library);
    if (!loaded.insert(fs::weakly_canonical(fs::absolute(library))).second)
      throw ToolError("duplicate operation library", 2);
    check(result->load_plugin(library));
  }
  check(result->freeze());
  return result;
}
void list_operations(const OperationRegistry& registry) {
  JsonWriter w;
  auto* list = w.array();
  for (const auto& name : registry.keys()) {
    auto traits = take(registry.find_traits(name));
    auto* item = w.object();
    w.push(list, item);
    w.put(item, "name", w.str(name));
    w.put(item, "traits_version",
          w.integer(static_cast<std::uint64_t>(traits.version)));
    w.put(item, "cpu", w.boolean(traits.supports_cpu));
    w.put(item, "gpu", w.boolean(traits.supports_gpu));
  }
  std::cout << w.finish(list, true);
}
}  // namespace ps::tools
