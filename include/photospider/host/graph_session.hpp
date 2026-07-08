#pragma once

#include <string>

#include "photospider/core/inspection_types.hpp"

/**
 * @file graph_session.hpp
 * @brief Public graph-session request values for Host implementations.
 *
 * These values describe graph lifecycle requests without exposing backend
 * runtime, model, filesystem service objects, or scheduler state. They are
 * copied into an embedded or IPC host adapter before any backend mutation
 * occurs.
 */

namespace ps {

/**
 * @brief Request for loading a graph session through a Host.
 *
 * @throws Nothing for value operations except string allocation on mutation.
 * @note `session.value` is the frontend-visible graph name. `root_dir` and
 *       `yaml_path` preserve current load semantics: an empty YAML path asks
 *       the backend to load `<root_dir>/<session>/content.yaml`.
 */
struct GraphLoadRequest {
  /** @brief Frontend-visible graph/session name. */
  GraphSessionId session;

  /** @brief Root directory that owns the graph session folder. */
  std::string root_dir;

  /** @brief Optional YAML source path to copy and load. */
  std::string yaml_path;

  /** @brief Optional config path copied into the graph session folder. */
  std::string config_path;

  /** @brief Optional cache-root directory used by graph cache services. */
  std::string cache_root_dir;
};

}  // namespace ps
