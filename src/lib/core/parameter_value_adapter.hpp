#pragma once

#include <yaml-cpp/yaml.h>

#include "photospider/plugin/node_view.hpp"

namespace ps::core {

/**
 * @brief Recursively copies one YAML value into the public owned value tree.
 * @param value YAML value to copy.
 * @return Deep-owned public value without YAML aliases.
 * @throws std::invalid_argument for unsupported mapping keys or normalized-key
 *         collisions.
 * @throws YAML::Exception when a scalar cannot be represented by its inferred
 * or explicit type, including an integer outside signed 64-bit range or a
 * numeric overflow to non-finite double, or uses an unsupported explicit tag.
 * @throws std::bad_alloc unchanged from recursive allocation.
 * @note Plain numeric scalars retain numeric alternatives; quoted numeric text
 *       remains a string.
 */
plugin::ParameterValue parameter_value_from_yaml(const YAML::Node& value);

/**
 * @brief Recursively copies one YAML mapping into a public parameter map.
 * @param value Null/undefined or mapping value to copy.
 * @return Empty map for null/undefined, otherwise an exact deep-owned mapping.
 * @throws std::invalid_argument for non-map input, non-scalar keys, or
 *         normalized-key collisions.
 * @throws YAML::Exception when a key or value scalar cannot be represented by
 * its inferred or explicit type, including numeric overflow, or uses an
 * unsupported explicit tag.
 * @throws std::bad_alloc unchanged from recursive allocation.
 * @note Numeric mapping keys use deterministic decimal string normalization.
 */
plugin::ParameterMap parameter_map_from_yaml(const YAML::Node& value);

/**
 * @brief Converts a public owned value back to independent YAML storage.
 * @param value Public recursive value.
 * @return Newly constructed YAML value.
 * @throws std::invalid_argument if value reports an unknown public parameter
 * kind.
 * @throws std::bad_alloc unchanged from YAML/container allocation.
 * @note The result does not alias any plugin container storage.
 */
YAML::Node parameter_value_to_yaml(const plugin::ParameterValue& value);

}  // namespace ps::core
