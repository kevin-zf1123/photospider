// FILE: src/cli/ask.cpp
#include "cli/ask.hpp"

#include <iostream>
#include <string>

std::string ask(const std::string& q, const std::string& def) {
  std::cout << q;
  if (!def.empty())
    std::cout << " [" << def << "]";
  std::cout << ": ";
  std::string s;
  std::getline(std::cin, s);
  if (s.empty())
    return def;
  return s;
}
