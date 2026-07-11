#pragma once

#include <cstddef>
#include <string>

namespace ps {

/**
 * @brief Retains one sticky input prefix while the REPL navigates matches.
 *
 * The state captures the prefix and cursor position at the start of a history
 * or completion cycle. Callers reset it after edits or cursor movement so a
 * later cycle observes the new input.
 *
 * @note The state is owned by one REPL invocation and is not thread-safe.
 */
struct InputMatchState {
  /** @brief Whether a navigation cycle currently owns the captured prefix. */
  bool active = false;
  /** @brief Input prefix captured when the current cycle began. */
  std::string original_prefix;
  /** @brief Cursor position captured when the current cycle began. */
  size_t original_cursor_pos = 0;

  /**
   * @brief Starts a navigation cycle from one prefix and cursor position.
   *
   * @param prefix Prefix to retain until the cycle is reset.
   * @param cursor_pos Cursor position associated with `prefix`.
   * @return Nothing.
   * @throws std::bad_alloc If copying `prefix` cannot allocate storage.
   * @note The caller must invoke Reset() after any edit that invalidates the
   * captured prefix or cursor position.
   */
  void Begin(const std::string& prefix, size_t cursor_pos) {
    active = true;
    original_prefix = prefix;
    original_cursor_pos = cursor_pos;
  }

  /**
   * @brief Ends the current navigation cycle and clears captured input.
   *
   * @return Nothing.
   * @throws Nothing.
   * @note Existing string capacity may remain owned for the next cycle.
   */
  void Reset() {
    active = false;
    original_prefix.clear();
    original_cursor_pos = 0;
  }
};

}  // namespace ps
