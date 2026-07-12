#pragma once

#include "photospider/ipc/client.hpp"

namespace ps::ipc::internal {

/**
 * @brief Private access to the Client's shutdown-only read interruption.
 *
 * @throws Nothing.
 * @note The helper never exposes or transfers a Unix descriptor. It is used
 *       only by IPC Host destruction to latch stop before publication or wake
 *       a polling RPC before the worker performs normal Client RAII close and
 *       joins.
 */
class ClientInterruptAccess {
 public:
  /**
   * @brief Latches stop and shuts down an active socket without closing it.
   *
   * @param client Client whose blocking read/write should be interrupted.
   * @throws Nothing.
   * @note A null pointer is ignored. A Client with no published descriptor
   *       retains the latch for its pending connect. The owning worker remains
   *       responsible for finishing and closing a descriptor exactly once.
   */
  static void interrupt(Client* client) noexcept {
    if (client != nullptr) {
      client->interrupt();
    }
  }
};

}  // namespace ps::ipc::internal
