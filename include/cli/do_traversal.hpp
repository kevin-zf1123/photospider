// FILE: include/cli/do_traversal.hpp
#pragma once

#include <string>

#include "photospider/host/host.hpp"

void do_traversal(ps::Host& host, const std::string& graph_name, bool show_mem,
                  bool show_disk);
