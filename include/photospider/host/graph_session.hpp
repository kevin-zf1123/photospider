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
 *       the backend to load `<root_dir>/<session>/content.yaml`. Every
 *       nonempty relative filesystem field is interpreted against the Host
 *       caller process working directory. The IPC Host resolves that text in
 *       its client process before transport so daemon working-directory state
 *       cannot change the public behavior.
 */
struct GraphLoadRequest {
  /** @brief Frontend-visible graph/session name. */
  GraphSessionId session;

  /** @brief Root directory, absolute or caller-working-directory relative. */
  std::string root_dir;

  /** @brief Optional absolute or caller-relative YAML source to copy/load. */
  std::string yaml_path;

  /** @brief Optional absolute or caller-relative config source path. */
  std::string config_path;

  /** @brief Optional absolute or caller-relative cache-root directory. */
  std::string cache_root_dir;
};

}  // namespace ps
