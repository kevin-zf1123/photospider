// FILE: apps/graph_cli/src/ask_yesno.cpp
#include "graph_cli/ask_yesno.hpp"

#include <iostream>
#include <string>

#include "graph_cli/ask.hpp"

bool ask_yesno(const std::string& q, bool def) {
  std::string d = def ? "Y" : "n";
  while (true) {
    std::string s = ask(q + " [Y/n]", d);
    if (s.empty())
      return def;
    if (s == "Y" || s == "y")
      return true;
    if (s == "N" || s == "n")
      return false;
    std::cout << "Please answer Y or n.\n";
  }
}
