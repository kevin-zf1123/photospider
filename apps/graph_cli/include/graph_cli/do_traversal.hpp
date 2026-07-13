#pragma once

#include <string>

#include "photospider/host/host.hpp"

/**
 * @brief Prints Host traversal branches and optional cache-state annotations.
 *
 * The function requests one copied traversal-details value, prints each
 * ending-node branch in evaluation order, and adds memory or disk cache labels
 * selected by the caller. A recoverable Host failure and an empty branch set
 * share the existing no-ending-nodes diagnostic.
 *
 * @param host Borrowed public Host used for the traversal inspection call.
 * @param graph_name Opaque graph session name to inspect.
 * @param show_mem Whether to print observed high-precision memory-cache state.
 * @param show_disk Whether to print observed disk-cache state.
 * @return Nothing.
 * @throws std::bad_alloc if the Host result, status labels, or output
 *         formatting exhausts memory.
 * @note `host` must remain alive for the call. The function retains no Host
 *       value and provides no thread synchronization; invoke it according to
 *       the Host implementation's thread/lifecycle contract.
 */
void do_traversal(ps::Host& host, const std::string& graph_name, bool show_mem,
                  bool show_disk);
