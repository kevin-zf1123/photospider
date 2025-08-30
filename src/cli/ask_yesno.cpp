// FILE: src/cli/ask_yesno.cpp
#include <iostream>
#include "cli/ask.hpp"
#include "cli/ask_yesno.hpp"

bool ask_yesno(const std::string& q, bool def) {
    std::string d = def ? "Y" : "n";
    while (true) {
        std::string s = ask(q + " [Y/n]", d);
        if (s.empty()) return def;
        if (s == "Y" || s == "y") return true;
        if (s == "N" || s == "n") return false;
        std::cout << "Please answer Y or n.\n";
    }
}

