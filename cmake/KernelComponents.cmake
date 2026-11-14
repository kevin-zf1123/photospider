# Independently linkable runtime components. Only the physical provider owns GPU dependencies.
set(ps_data_sources
  src/lib/core/cancellation.cpp
  src/lib/core/resources.cpp
  src/lib/core/utf8_validation.cpp
  src/lib/data/device_storage.cpp
  src/lib/data/device_tensor_span.cpp
  src/lib/data/footprint.cpp
  src/lib/data/fragment_atlas.cpp
  src/lib/data/immutable_resource.cpp
  src/lib/data/input_snapshot.cpp
  src/lib/data/region.cpp
  src/lib/data/region_runs.cpp
  src/lib/data/resource_set.cpp
  src/lib/data/resource_storage.cpp
  src/lib/data/result.cpp
  src/lib/data/result_builder.cpp
  src/lib/data/result_capture.cpp
  src/lib/data/result_device.cpp
  src/lib/data/result_schema.cpp
  src/lib/data/result_support.cpp
  src/lib/data/result_tensor_publication.cpp
  src/lib/data/result_tensor_store.cpp
  src/lib/data/result_tensor_window.cpp
  src/lib/data/storage.cpp
  src/lib/data/tensor.cpp
  src/lib/data/tensor_geometry.cpp
  src/lib/data/tensor_layout.cpp
  src/lib/data/tensor_span.cpp
  src/lib/data/tensor_validation.cpp
  src/lib/data/tiles.cpp)
set(ps_model_sources
  sdk/model/graph_context.cpp
  sdk/model/physical_program.cpp)
set(ps_operation_runtime_sources
  sdk/operation/tile_mapping.cpp
  sdk/operation/fixed_invocation.cpp
  sdk/operation/operation_contract.cpp
  sdk/operation/operation_extensions.cpp
  sdk/operation/operation_module.cpp
  sdk/operation/operation_program.cpp
  sdk/operation/operation_registry.cpp
  sdk/operation/port_validation.cpp)
set(ps_mapper_geometry_sources plugins/mappers/geometry/geometry.cpp plugins/mappers/geometry/register.cpp)
set(ps_program_sources
  sdk/program/artifact.cpp
  sdk/program/artifact_text.cpp
  sdk/program/identity.cpp
  sdk/program/ir_validation.cpp
  sdk/program/plan.cpp
  sdk/program/plan_linking.cpp
  sdk/program/plan_selection.cpp
  sdk/program/plan_validation.cpp)
set(ps_frontend_dag_sources
  plugins/frontends/dag/dag.cpp)
set(ps_optimizer_deduplicate_sources
  plugins/optimizers/deduplicate/deduplicate.cpp)
set(ps_planner_static_tiles_sources
  plugins/planners/static_tiles/static_tiles.cpp)
set(ps_metal_host_sources host/metal_completion.mm)
set(ps_plugin_host_sources host/provider_library.cpp sdk/execution/thread_retirement.cpp)
set(ps_compiler_default_sources host/default_compiler.cpp)
set(ps_compiler_sources
  host/planner_module.cpp
  plugins/compiler/default/compiler.cpp
  plugins/compiler/default/frontend.cpp
  plugins/compiler/default/optimizer.cpp
  plugins/compiler/default/planner.cpp
  host/frontend_registry.cpp
  host/optimizer_registry.cpp
  host/planner_registry.cpp)
set(ps_execution_default_sources host/physical_context.cpp)
set(ps_execution_sources sdk/execution/execution.cpp sdk/execution/data_services.cpp sdk/execution/preflight.cpp sdk/execution/plan_services.cpp)
set(ps_executor_physical_sources
  plugins/executors/physical/provider.cpp
  plugins/executors/physical/block_memory.cpp
  plugins/executors/physical/cpu_workers.cpp
  plugins/executors/physical/execution.cpp
  plugins/executors/physical/native_copy.cpp
  plugins/executors/physical/native_gpu.cpp
  plugins/executors/physical/task_schedule.cpp
  plugins/executors/physical/tile_cache.cpp)
if(PHOTOSPIDER_ENABLE_METAL AND APPLE)
  list(APPEND ps_executor_physical_sources plugins/executors/physical/native_gpu_metal.mm)
  set_source_files_properties(plugins/executors/physical/native_gpu_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()
if(PHOTOSPIDER_ENABLE_VULKAN)
  list(APPEND ps_executor_physical_sources plugins/executors/physical/native_gpu_vulkan.cpp)
endif()

function(photospider_configure_gpu target)
  if(PHOTOSPIDER_ENABLE_METAL AND APPLE)
    target_compile_definitions(${target} PRIVATE PHOTOSPIDER_HAS_METAL)
    target_link_libraries(${target} PRIVATE "-framework Metal" "-framework Foundation")
    set_target_properties(${target} PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON)
  endif()
  if(PHOTOSPIDER_ENABLE_VULKAN)
    target_compile_definitions(${target} PRIVATE PHOTOSPIDER_HAS_VULKAN)
    target_link_libraries(${target} PRIVATE Vulkan::Vulkan)
  endif()

endfunction()

function(ps_component name)
  set(target photospider_${name})
  # Module hosts and default providers must share process-local data and TLS,
  # including when the embedding application's convenience libraries are static.
  if(name MATCHES "^(data|model|operation_runtime|program|plugin_host|executor_physical|metal_host)$")
    add_library(${target} SHARED ${ps_${name}_sources})
  else()
    add_library(${target} ${ps_${name}_sources})
  endif()
  add_library(Photospider::${name} ALIAS ${target})
  photospider_configure_kernel_target(${target})
  set_target_properties(${target} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE EXPORT_NAME ${name}
    CXX_VISIBILITY_PRESET default VISIBILITY_INLINES_HIDDEN OFF
    VERSION ${PROJECT_VERSION} SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
  # Windows exports component implementations from their individual import
  # libraries. C module entry points carry their own explicit dllexport.
  target_compile_definitions(${target} PUBLIC PHOTOSPIDER_COMPONENT_BUILD)
  foreach(dependency IN LISTS ARGN)
    target_link_libraries(${target} PUBLIC Photospider::${dependency})
  endforeach()
  set(PHOTOSPIDER_COMPONENT_TARGETS ${PHOTOSPIDER_COMPONENT_TARGETS} ${target} PARENT_SCOPE)
  set(PHOTOSPIDER_KERNEL_SOURCES ${PHOTOSPIDER_KERNEL_SOURCES} ${ps_${name}_sources} PARENT_SCOPE)
endfunction()

function(ps_module target source dependency)
  add_library(${target} MODULE ${source} ${ARGN})
  target_link_libraries(${target} PRIVATE ${dependency})
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_features(${target} PRIVATE cxx_std_17)
  target_compile_definitions(${target} PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
  install(TARGETS ${target} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/photospider
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/photospider)
endfunction()

macro(photospider_make_components)
  ps_component(data)
  target_include_directories(photospider_data PRIVATE src/lib)
  ps_component(model data)
  ps_component(plugin_host data)
  ps_component(operation_runtime model plugin_host)
  ps_component(program operation_runtime)
  if(PHOTOSPIDER_BUILD_COMPILER)
    ps_component(frontend_dag program)
    ps_component(optimizer_deduplicate program)
    ps_component(planner_static_tiles program)
    ps_component(mapper_geometry operation_runtime)
    ps_component(compiler plugin_host program)
    ps_component(compiler_default compiler frontend_dag optimizer_deduplicate planner_static_tiles)
    ps_module(photospider_planner_module plugins/planners/static_tiles/module.cpp Photospider::planner_static_tiles)
    ps_module(photospider_mapper_geometry_module plugins/mappers/geometry/module.cpp
      Photospider::operation_sdk plugins/mappers/geometry/geometry.cpp)
  endif()
  if(PHOTOSPIDER_BUILD_EXECUTOR)
    if(PHOTOSPIDER_ENABLE_METAL AND APPLE)
      ps_component(metal_host data)
      target_link_libraries(photospider_metal_host PRIVATE "-framework Metal" "-framework Foundation")
      set_source_files_properties(host/metal_completion.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
      ps_component(executor_physical program plugin_host metal_host)
    else()
      ps_component(executor_physical program plugin_host)
    endif()
    photospider_configure_gpu(photospider_executor_physical)
    ps_component(execution program plugin_host)
    ps_component(execution_default execution executor_physical)
    ps_module(photospider_executor_module plugins/executors/physical/module.cpp Photospider::executor_physical)
  endif()
  if(PHOTOSPIDER_BUILD_COMPILER AND PHOTOSPIDER_BUILD_EXECUTOR)
    add_library(photospider INTERFACE)
    add_library(Photospider::kernel ALIAS photospider)
    set_target_properties(photospider PROPERTIES EXPORT_NAME kernel)
    target_link_libraries(photospider INTERFACE Photospider::compiler_default Photospider::execution_default)
    list(APPEND PHOTOSPIDER_COMPONENT_TARGETS photospider)
  endif()
endmacro()
