// FILE: src/cli/command/help_utils.cpp
#include <iostream>
#include <fstream>
#include <filesystem>
#include "cli/command/help_utils.hpp"

namespace fs = std::filesystem;

void print_help_from_file(const std::string& filename) {
    try {
        fs::path path = fs::path("src") / "cli" / "command" / "help" / filename;
        std::ifstream in(path);
        if (!in) {
            std::cout << "(Help not available: " << path.string() << ")\n";
            return;
        }
        std::string line;
        while (std::getline(in, line)) {
            std::cout << line << '\n';
        }
    } catch (const std::exception& e) {
        std::cout << "(Error reading help file: " << e.what() << ")\n";
    }
}

