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

void load_plugins(const std::vector<std::string>& plugin_dir_paths,
                  std::map<std::string, std::string>& op_sources) {
    auto& registry = OpRegistry::instance();

    auto iter_and_load = [&](const fs::path& base_dir, bool recursive) {
        if (!fs::exists(base_dir) || !fs::is_directory(base_dir)) return;
        std::cout << "Scanning for plugins in '" << base_dir.string() << (recursive ? "' (recursive)..." : "'...") << std::endl;

        auto process_path = [&](const fs::path& path) {
            #if defined(_WIN32)
                const std::string extension = ".dll";
            #elif defined(__APPLE__)
                const std::string extension = ".dylib";
            #else
                const std::string extension = ".so";
            #endif
            if (path.extension() != extension) return; // skip non-shared libraries

            std::cout << "  - Attempting to load plugin: " << path.filename().string() << std::endl;
            auto keys_before = registry.get_keys();

            #ifdef _WIN32
            HMODULE handle = LoadLibrary(path.string().c_str());
            if (!handle) { std::cerr << "    Error: Failed to load plugin. Code: " << GetLastError() << std::endl; return; }
            using RegisterFunc = void(*)();
            RegisterFunc register_func = (RegisterFunc)GetProcAddress(handle, "register_photospider_ops");
            if (!register_func) { std::cerr << "    Error: Cannot find 'register_photospider_ops' export in plugin." << std::endl; FreeLibrary(handle); return; }
            #else
            void* handle = dlopen(path.c_str(), RTLD_LAZY);
            if (!handle) { std::cerr << "    Error: Failed to load plugin: " << dlerror() << std::endl; return; }
            void (*register_func)();
            *(void**)(&register_func) = dlsym(handle, "register_photospider_ops");
            const char* dlsym_error = dlerror();
            if (dlsym_error) { std::cerr << "    Error: Cannot find 'register_photospider_ops' export in plugin: " << dlsym_error << std::endl; dlclose(handle); return; }
            #endif

            try {
                register_func();
                auto keys_after = registry.get_keys();
                std::vector<std::string> new_keys;
                std::set_difference(keys_after.begin(), keys_after.end(), keys_before.begin(), keys_before.end(), std::back_inserter(new_keys));
                for (const auto& key : new_keys) { op_sources[key] = fs::absolute(path).string(); }
                std::cout << "    Success: Plugin loaded and " << new_keys.size() << " operation(s) registered." << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "    Error: An exception occurred during plugin registration: " << e.what() << std::endl;
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
}
