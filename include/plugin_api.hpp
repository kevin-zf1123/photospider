// FILE: include/plugin_api.hpp
#pragma once

#include "node.hpp"
#include "ps_types.hpp"

/**
 * @brief The function signature that every Photospider plugin must implement
 * and export.
 *
 * When the main application loads a plugin (a .so or .dll file), it will search
 * for a function with the name "register_photospider_ops". If found, it will be
 * called, allowing the plugin to register its custom operations with the
 * OpRegistry.
 *
 * Use extern "C" to prevent C++ name mangling, which ensures that the
 * application can find the function by its exact name.
 */
#ifdef _WIN32
#define PLUGIN_API __declspec(dllexport)
#else
#define PLUGIN_API
#endif

extern "C" PLUGIN_API void register_photospider_ops();