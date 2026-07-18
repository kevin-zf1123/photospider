#pragma once

/**
 * @file graph_document_test_dependencies.hpp
 * @brief Supplies configured graph-document dependencies to backend tests.
 */

#include <memory>

#include "adapters/yaml/yaml_graph_document_adapter.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_io_service.hpp"  // NOLINT(build/include_subdir)

namespace ps::testing {

/**
 * @brief Creates one configured YAML graph-document adapter owner.
 *
 * @return Mutable concrete owner convertible to both immutable contracts.
 * @throws std::bad_alloc if shared ownership allocation fails.
 * @note The returned instance owns no process-global format selection and may
 *       be isolated per test.
 */
inline std::shared_ptr<adapters::yaml::YamlGraphDocumentAdapter>
make_yaml_graph_document_adapter() {
  return std::make_shared<adapters::yaml::YamlGraphDocumentAdapter>();
}

/**
 * @brief Creates format-neutral graph IO backed by one YAML adapter instance.
 *
 * @return Service retaining the same adapter control block as reader/writer.
 * @throws std::bad_alloc if adapter ownership allocation fails.
 * @note This helper preserves configured product behavior in tests that focus
 *       on graph/model transactions rather than dependency substitution.
 */
inline GraphIOService make_yaml_graph_io_service() {
  auto adapter = make_yaml_graph_document_adapter();
  return GraphIOService(adapter, adapter);
}

}  // namespace ps::testing
