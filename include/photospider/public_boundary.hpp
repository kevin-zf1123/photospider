#pragma once

/**
 * @file public_boundary.hpp
 * @brief Marks the installable Photospider public header root.
 *
 * This header is intentionally empty at the API level. It gives public-header
 * guardrails a stable root-level `include/photospider/` header to scan and
 * compile alongside the complete installable core, Host, operation-plugin,
 * pure-C policy-plugin, and IPC client contract inventory.
 *
 * @note Public headers under this root must remain self-contained and must not
 *       depend on implementation-only include paths.
 */
