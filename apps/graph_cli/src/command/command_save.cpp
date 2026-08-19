// FILE: apps/graph_cli/src/command/command_save.cpp
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "graph_cli/command/commands.hpp"
#include "graph_cli/command/help_utils.hpp"
#include "providers/configured_image_artifact_codec.hpp"

namespace {

/**
 * @brief Parses the explicit finite-domain handling token.
 * @param token Exact `reject` or `clamp` spelling.
 * @return Matching closed conversion policy.
 * @throws std::invalid_argument for every other spelling.
 */
ps::OutOfDomainPolicy parse_domain_policy(const std::string& token) {
  if (token == "reject")
    return ps::OutOfDomainPolicy::Reject;
  if (token == "clamp")
    return ps::OutOfDomainPolicy::Clamp;
  throw std::invalid_argument("domain policy must be reject or clamp");
}

/**
 * @brief Parses the explicit integral rounding token.
 * @param token Exact maintained rounding spelling.
 * @return Matching closed rounding policy.
 * @throws std::invalid_argument for every other spelling.
 */
ps::SampleRoundingMode parse_rounding(const std::string& token) {
  if (token == "nearest-even")
    return ps::SampleRoundingMode::NearestEven;
  if (token == "toward-zero")
    return ps::SampleRoundingMode::TowardZero;
  if (token == "floor")
    return ps::SampleRoundingMode::Floor;
  if (token == "ceil")
    return ps::SampleRoundingMode::Ceil;
  throw std::invalid_argument(
      "rounding must be nearest-even, toward-zero, floor, or ceil");
}

/**
 * @brief Parses the explicit exceptional-value handling token.
 * @param token Exact `reject` or `preserve` spelling.
 * @return Matching closed exceptional-value policy.
 * @throws std::invalid_argument for every other spelling.
 */
ps::NonFinitePolicy parse_non_finite_policy(const std::string& token) {
  if (token == "reject")
    return ps::NonFinitePolicy::Reject;
  if (token == "preserve")
    return ps::NonFinitePolicy::Preserve;
  throw std::invalid_argument("non-finite policy must be reject or preserve");
}

/**
 * @brief Parses the explicit numeric-loss handling token.
 * @param token Exact `reject` or `allow` spelling.
 * @return Matching closed precision-loss policy.
 * @throws std::invalid_argument for every other spelling.
 */
ps::PrecisionLossPolicy parse_precision_policy(const std::string& token) {
  if (token == "reject")
    return ps::PrecisionLossPolicy::Reject;
  if (token == "allow")
    return ps::PrecisionLossPolicy::Allow;
  throw std::invalid_argument("precision-loss policy must be reject or allow");
}

/**
 * @brief Parses one explicit destination sample-encoding kind.
 * @param token Exact `value`, `normalized`, or `code` spelling.
 * @return Matching closed sample-encoding kind.
 * @throws std::invalid_argument for every other spelling.
 */
ps::SampleEncodingKind parse_sample_encoding(const std::string& token) {
  if (token == "value")
    return ps::SampleEncodingKind::Value;
  if (token == "normalized")
    return ps::SampleEncodingKind::Normalized;
  if (token == "code")
    return ps::SampleEncodingKind::CodeValue;
  throw std::invalid_argument(
      "destination encoding must be value, normalized, or code");
}

/**
 * @brief Parses one explicit destination sample-domain kind.
 * @param token Exact `normalized`, `legal`, or `code` spelling.
 * @return Matching closed sample-domain kind.
 * @throws std::invalid_argument for every other spelling.
 */
ps::SampleDomainKind parse_sample_domain(const std::string& token) {
  if (token == "normalized")
    return ps::SampleDomainKind::Normalized;
  if (token == "legal")
    return ps::SampleDomainKind::Legal;
  if (token == "code")
    return ps::SampleDomainKind::CodeValue;
  throw std::invalid_argument(
      "destination domain must be normalized, legal, or code");
}

/**
 * @brief Parses one finite destination-domain endpoint without a partial read.
 * @param token Complete decimal token.
 * @param label Stable endpoint name used by failure diagnostics.
 * @return Exact finite binary64 value accepted by `std::stod`.
 * @throws std::invalid_argument when the token is partial or non-finite.
 * @throws std::out_of_range when the token exceeds binary64 range.
 */
double parse_finite_endpoint(const std::string& token,
                             const std::string& label) {
  std::size_t consumed = 0U;
  const double value = std::stod(token, &consumed);
  if (consumed != token.size() || !std::isfinite(value)) {
    throw std::invalid_argument(label + " must be one finite decimal value");
  }
  return value;
}

/**
 * @brief Builds one explicit Value-to-file code conversion request.
 * @param value Ready ordinary image with one default sample endpoint.
 * @param storage Exact `uint8` or `uint16` destination selection.
 * @param destination_encoding Explicit destination sample encoding.
 * @param destination_domain Explicit destination sample-domain kind.
 * @param destination_minimum Finite inclusive destination lower endpoint.
 * @param destination_maximum Finite inclusive destination upper endpoint.
 * @param domain_policy Explicit finite out-of-domain policy.
 * @param rounding Explicit integral rounding policy.
 * @param non_finite Explicit NaN/infinity policy.
 * @param precision Explicit narrowing/quantization-loss policy.
 * @return Complete encode request without extension or storage inference.
 * @throws std::invalid_argument for missing/per-channel sample metadata or an
 * unsupported destination token.
 * @throws std::bad_alloc when metadata copying allocates.
 */
ps::ImageArtifactEncodeRequest make_encode_request(
    const ps::Value& value, const std::string& storage,
    ps::SampleEncodingKind destination_encoding,
    ps::SampleDomainKind destination_domain, double destination_minimum,
    double destination_maximum, ps::OutOfDomainPolicy domain_policy,
    ps::SampleRoundingMode rounding, ps::NonFinitePolicy non_finite,
    ps::PrecisionLossPolicy precision) {
  if (!value.image_facet().has_value() ||
      !value.image_facet()->sample_domain.has_value() ||
      !value.image_facet()->sample_domain->per_channel.empty()) {
    throw std::invalid_argument(
        "selected image needs one explicit default sample domain");
  }
  const std::uint32_t bits = storage == "uint8"    ? 8U
                             : storage == "uint16" ? 16U
                                                   : 0U;
  if (bits == 0U) {
    throw std::invalid_argument("destination storage must be uint8 or uint16");
  }
  const ps::SampleDomainFacet& samples = *value.image_facet()->sample_domain;
  ps::SampleConversion conversion;
  conversion.source =
      ps::SampleEndpoint{samples.encoding, samples.default_domain};
  conversion.destination = ps::SampleEndpoint{
      ps::SampleEncoding{1U, destination_encoding},
      ps::SampleDomain{destination_domain, destination_minimum,
                       destination_maximum}};
  conversion.destination_element_semantics =
      ps::ElementSemantics::UnsignedInteger;
  conversion.destination_storage_encoding = ps::StorageEncoding{bits};
  conversion.out_of_domain = domain_policy;
  conversion.rounding = rounding;
  conversion.non_finite = non_finite;
  conversion.precision_loss = precision;
  return ps::ImageArtifactEncodeRequest{conversion};
}

/** @brief Stable CLI usage for the explicit image-save contract. */
constexpr char kSaveUsage[] =
    "Usage: save <id> <output> <file> <uint8|uint16> "              // NOLINT
    "<value|normalized|code> <normalized|legal|code> <min> <max> "  // NOLINT
    "<reject|clamp> <nearest-even|toward-zero|floor|ceil> "         // NOLINT
    "<reject|preserve> <reject|allow>";                             // NOLINT

}  // namespace

/** @copydoc handle_save */
bool handle_save(std::istringstream& iss, ps::Host& svc,
                 std::string& current_graph, bool& /*modified*/,
                 CliConfig& config) {
  if (current_graph.empty()) {
    std::cout << "No current graph. Use load/switch.\n";
    return true;
  }
  int node_id = -1;
  std::string output_name;
  std::string path;
  std::string storage;
  std::string destination_encoding_token;
  std::string destination_domain_token;
  std::string destination_minimum_token;
  std::string destination_maximum_token;
  std::string domain_token;
  std::string rounding_token;
  std::string non_finite_token;
  std::string precision_token;
  if (!(iss >> node_id >> output_name >> path >> storage >>
        destination_encoding_token >> destination_domain_token >>
        destination_minimum_token >> destination_maximum_token >>
        domain_token >> rounding_token >> non_finite_token >>
        precision_token) ||
      node_id < 0) {
    std::cout << kSaveUsage << "\n";
    return true;
  }
  std::string trailing;
  if (iss >> trailing) {
    std::cout << kSaveUsage << "\n";
    return true;
  }

  try {
    ps::HostComputeRequest request;
    request.session = ps::GraphSessionId{current_graph};
    request.node = ps::NodeId{node_id};
    request.cache.precision = config.cache_precision;
    request.cache.force_recache = false;
    request.cache.disable_disk_cache = false;
    request.telemetry.enable_timing = false;
    request.execution.parallel = false;
    const ps::Result<ps::NamedValueResult> computed =
        svc.compute_and_get_values(request);
    if (!computed.status.ok) {
      std::cout << "Failed to compute node " << node_id << ".\n";
      if (!computed.status.message.empty()) {
        std::cout << "Reason: " << computed.status.message << "\n";
      }
      return true;
    }
    const ps::Value* value = computed.value.find(output_name);
    if (value == nullptr || !value->image_facet().has_value()) {
      std::cout << "Named output '" << output_name
                << "' is absent or is not an ordinary image.\n";
      return true;
    }
    const ps::ImageArtifactEncodeRequest encode_request = make_encode_request(
        *value, storage, parse_sample_encoding(destination_encoding_token),
        parse_sample_domain(destination_domain_token),
        parse_finite_endpoint(destination_minimum_token, "destination minimum"),
        parse_finite_endpoint(destination_maximum_token, "destination maximum"),
        parse_domain_policy(domain_token), parse_rounding(rounding_token),
        parse_non_finite_policy(non_finite_token),
        parse_precision_policy(precision_token));
    const auto codec = ps::providers::make_configured_image_artifact_codec();
    codec->encode(path, *value, encode_request);
    std::cout << "Saved named output '" << output_name << "' to " << path
              << "\n";
  } catch (const std::exception& error) {
    std::cout << "Failed to save image: " << error.what() << "\n";
  }
  return true;
}

/** @copydoc print_help_save */
void print_help_save(const CliConfig& /*config*/) {
  print_help_from_file("help_save.txt");
}
