#pragma once

/**
 * @file photospider.hpp
 * @brief Convenience include for the complete public embedded-kernel facade.
 *
 * @note Individual headers remain supported for compile-time isolation.
 */

#include "photospider/benchmark/raw_benchmark.hpp"
#include "photospider/compiler/compiler.hpp"
#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/status.hpp"
#include "photospider/data/dependency.hpp"
#include "photospider/data/footprint.hpp"
#include "photospider/data/fragment_atlas.hpp"
#include "photospider/data/input_snapshot.hpp"
#include "photospider/data/region.hpp"
#include "photospider/data/storage.hpp"
#include "photospider/data/value.hpp"
#include "photospider/data/value_fragments.hpp"
#include "photospider/execution/cancellation.hpp"
#include "photospider/execution/execution.hpp"
#include "photospider/plugin/data_definition_registry.hpp"
#include "photospider/plugin/operation_plugin.hpp"
#include "photospider/plugin/operation_registry.hpp"
