// Implementation of plugin loading
#include "plugin_loader.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <vector>
#include <map>

#include "ps_types.hpp"

#ifdef _WIN32
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

using namespace ps; // for OpRegistry and fs

namespace ps {

PluginLoadResult load_plugins(const std::vector<std::string>& plugin_dir_paths,
                              std::map<std::string, std::string>& op_sources) {
    auto& registry = OpRegistry::instance();
    PluginLoadResult result;

    auto iter_and_load = [&](const fs::path& base_dir, bool recursive) {
        if (!fs::exists(base_dir) || !fs::is_directory(base_dir)) return;
        // Kernel layer: avoid terminal output; frontends can report scanning if needed.

        auto process_path = [&](const fs::path& path) {
            #if defined(_WIN32)
                const std::string extension = ".dll";
            #elif defined(__APPLE__)
                const std::string extension = ".dylib";
            #else
                const std::string extension = ".so";
            #endif
            if (path.extension() != extension) return; // skip non-shared libraries

            // silent in kernel
            auto keys_before = registry.get_keys();

            #ifdef _WIN32
            HMODULE handle = LoadLibrary(path.string().c_str());
            if (!handle) { result.errors.push_back({fs::absolute(path).string(), GraphErrc::Io, "LoadLibrary failed"}); return; }
            using RegisterFunc = void(*)();
            RegisterFunc register_func = (RegisterFunc)GetProcAddress(handle, "register_photospider_ops");
            if (!register_func) { result.errors.push_back({fs::absolute(path).string(), GraphErrc::InvalidParameter, "Missing register_photospider_ops"}); FreeLibrary(handle); return; }
            #else
            void* handle = dlopen(path.c_str(), RTLD_LAZY);
            if (!handle) { const char* e = dlerror(); result.errors.push_back({fs::absolute(path).string(), GraphErrc::Io, e?e:"dlopen failed"}); return; }
            void (*register_func)();
            *(void**)(&register_func) = dlsym(handle, "register_photospider_ops");
            const char* dlsym_error = dlerror();
            if (dlsym_error) { result.errors.push_back({fs::absolute(path).string(), GraphErrc::InvalidParameter, dlsym_error}); dlclose(handle); return; }
            #endif

            try {
                register_func();
                auto keys_after = registry.get_keys();
                std::vector<std::string> new_keys;
                std::set_difference(keys_after.begin(), keys_after.end(), keys_before.begin(), keys_before.end(), std::back_inserter(new_keys));
                for (const auto& key : new_keys) { op_sources[key] = fs::absolute(path).string(); }
                // silent in kernel
            } catch (const std::exception& e) {
                result.errors.push_back({fs::absolute(path).string(), GraphErrc::Unknown, e.what()});
                #ifdef _WIN32
                FreeLibrary(handle);
                #else
                dlclose(handle);
                #endif
            }
        };

        if (recursive) {
            for (const auto& entry : fs::recursive_directory_iterator(base_dir)) {
                if (entry.is_regular_file()) process_path(entry.path());
            }
        } else {
            for (const auto& entry : fs::directory_iterator(base_dir)) {
                if (entry.is_regular_file()) process_path(entry.path());
            }
        }
    };

    for (const auto& raw_path : plugin_dir_paths) {
        if (raw_path.empty()) continue;
        // Interpret simple wildcard suffixes:
        //   path/**  => recursive
        //   path/*   => shallow (explicit)
        //   path     => shallow
        bool recursive = false;
        std::string path_str = raw_path;
        if (path_str.size() >= 3 && path_str.substr(path_str.size() - 3) == "/**") {
            recursive = true;
            path_str = path_str.substr(0, path_str.size() - 3);
        } else if (path_str.size() >= 2 && path_str.substr(path_str.size() - 2) == "/*") {
            recursive = false;
            path_str = path_str.substr(0, path_str.size() - 2);
        }
        iter_and_load(path_str, recursive);
    }
    return result;
}

} // namespace ps
