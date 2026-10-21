#pragma once

/**
 * @file photospider.hpp
 * @brief Convenience include for the complete public embedded-kernel facade.
 *
 * @note Individual headers remain supported for compile-time isolation.
 */

#include "photospider/compiler/artifact.hpp"
#include "photospider/compiler/compiler.hpp"
#include "photospider/compiler/frontend.hpp"
#include "photospider/compiler/ir.hpp"
#include "photospider/compiler/plan.hpp"
#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/cancellation.hpp"
#include "photospider/core/resources.hpp"
#include "photospider/core/status.hpp"
#include "photospider/data/footprint.hpp"
#include "photospider/data/fragment_atlas.hpp"
#include "photospider/data/input_snapshot.hpp"
#include "photospider/data/region.hpp"
#include "photospider/data/region_runs.hpp"
#include "photospider/data/resource_set.hpp"
#include "photospider/data/result.hpp"
#include "photospider/data/storage.hpp"
#include "photospider/data/tensor.hpp"
#include "photospider/data/tensor_layout.hpp"
#include "photospider/data/tensor_span.hpp"
#include "photospider/execution/execution.hpp"
#include "photospider/plugin/native_kernel_api.h"
#include "photospider/plugin/operation_module_api.h"
#include "photospider/plugin/operation_registry.hpp"
