#pragma once

/**
 * @file public_boundary.hpp
 * @brief Marks the installable Photospider public header root.
 *
 * This header is intentionally empty at the API level. It gives phase-0
 * public-header guardrails a real `include/photospider/` header to scan and
 * compile before phase-1 introduces stable value contracts.
 *
 * @note Public headers under this root must remain self-contained and must not
 *       depend on implementation-only include paths.
 */
