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
    for (const auto& plugin_dir_path : plugin_dir_paths) {
        if (!fs::exists(plugin_dir_path) || !fs::is_directory(plugin_dir_path)) continue;
        std::cout << "Scanning for plugins in '" << plugin_dir_path << "'..." << std::endl;

        for (const auto& entry : fs::directory_iterator(plugin_dir_path)) {
            const auto& path = entry.path();
            #if defined(_WIN32)
                const std::string extension = ".dll";
            #elif defined(__APPLE__)
                const std::string extension = ".dylib";
            #else
                const std::string extension = ".so";
            #endif
            if (path.extension() != extension) continue;

            std::cout << "  - Attempting to load plugin: " << path.filename().string() << std::endl;
            auto keys_before = registry.get_keys();

            #ifdef _WIN32
            HMODULE handle = LoadLibrary(path.string().c_str());
            if (!handle) { std::cerr << "    Error: Failed to load plugin. Code: " << GetLastError() << std::endl; continue; }
            using RegisterFunc = void(*)();
            RegisterFunc register_func = (RegisterFunc)GetProcAddress(handle, "register_photospider_ops");
            if (!register_func) { std::cerr << "    Error: Cannot find 'register_photospider_ops' export in plugin." << std::endl; FreeLibrary(handle); continue; }
            #else
            void* handle = dlopen(path.c_str(), RTLD_LAZY);
            if (!handle) { std::cerr << "    Error: Failed to load plugin: " << dlerror() << std::endl; continue; }
            void (*register_func)();
            *(void**)(&register_func) = dlsym(handle, "register_photospider_ops");
            const char* dlsym_error = dlerror();
            if (dlsym_error) { std::cerr << "    Error: Cannot find 'register_photospider_ops' export in plugin: " << dlsym_error << std::endl; dlclose(handle); continue; }
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
        }
    }
}

