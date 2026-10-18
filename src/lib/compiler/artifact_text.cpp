#include "compiler/artifact_text.hpp"

#include <algorithm>
#include <array>
#include <cfenv>  // NOLINT(build/c++11)
#include <charconv>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compiler/limits.hpp"
#include "core/callback_environment.hpp"

namespace ps::compiler_internal {
namespace {
struct TextFailure final {
  Status status;
};

/** @brief Give decimal conversions one environment and restore caller state.
 * The default environment also disables traps and nonstandard flush modes.
 */
class NumericEnvironment final {
 public:
  NumericEnvironment() {
    if (!restore_.active() || std::fesetenv(FE_DFL_ENV) != 0 ||
        std::fesetround(FE_TONEAREST) != 0)
      throw TextFailure{
          Status{ErrorCode::OperationFailed,
                 "cannot establish artifact numeric environment"}};
  }

 private:
  core_internal::CallbackEnvironment restore_;
};

Status allocation_failure() {
  return {ErrorCode::ResourceExhausted, "compiler artifact allocation failed"};
}

int hex_digit(unsigned char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

bool hexadecimal(std::string_view value, std::size_t length) {
  if (value.size() != length)
    return false;
  for (unsigned char c : value)
    if (hex_digit(c) < 0)
      return false;
  return true;
}

/** @brief Bounded nonrecursive token reader; errors retain source position. */
class Reader final {
 public:
  explicit Reader(std::string_view text) : text_(text) {
    if (text.size() > kMaximumArtifactBytes)
      fail("text exceeds the 64 MiB artifact limit");
    next();
  }

  ArtifactData parse() {
    ArtifactData data;
    const bool physical = take("psxp");
    if (!physical)
      expect("psir");
    if (number() != 2)
      fail("unsupported compiler artifact version");
    if (physical) {
      data.manifest.kind = ArtifactKind::ExecutionPlan;
    } else {
      expect("stage");
      if (take("semantic"))
        data.manifest.kind = ArtifactKind::SemanticIR;
      else if (take("optimized"))
        data.manifest.kind = ArtifactKind::OptimizedIR;
      else
        fail("expected semantic or optimized IR stage");
    }
    expect("parent");
    data.parent_digest = quoted(16);
    if (!data.parent_digest.empty() && !hexadecimal(data.parent_digest, 16))
      fail("parent must be an empty or sixteen-digit identity");
    if (data.manifest.kind == ArtifactKind::SemanticIR &&
        !data.parent_digest.empty())
      fail("semantic IR has no parent stage");
    if (physical) {
      expect("mode");
      if (take("cpu_exact"))
        data.options.execution_mode = ExecutionMode::CpuExact;
      else if (take("native_gpu"))
        data.options.execution_mode = ExecutionMode::NativeGpu;
      else
        fail("unknown execution mode");
      expect("tile_extent");
      data.options.tile_extent = number();
      const auto extent = data.options.tile_extent;
      if (!extent || (extent & (extent - 1)))
        fail("tile extent must be a positive power of two");
    }
    expect("operations");
    expect("{");
    std::set<std::string> operations;
    while (!take("}")) {
      bound(data.manifest.operations.size(), kMaximumIRNodes, "operations");
      expect("require");
      ArtifactOperation operation;
      operation.key = quoted(1024);
      operation.contract_digest = quoted(16);
      if (operation.key.empty() || !operations.insert(operation.key).second ||
          !hexadecimal(operation.contract_digest, 16))
        fail("duplicate/malformed operation requirement");
      data.manifest.operations.push_back(std::move(operation));
    }
    expect("resources");
    expect("{");
    std::set<ResourceIdentity> resources;
    while (!take("}")) {
      expect("resource");
      if (number() != data.manifest.resources.size())
        fail("resource indices must be consecutive from zero");
      ResourceIdentity identity;
      identity.byte_length = number();
      const auto hash = quoted(64);
      if (!identity.byte_length || !hexadecimal(hash, 64))
        fail("invalid resource length/SHA-256");
      for (std::size_t i = 0; i < identity.sha256.size(); ++i)
        identity.sha256[i] = static_cast<std::uint8_t>(
            hex_digit(hash[2 * i]) * 16 + hex_digit(hash[2 * i + 1]));
      if (!resources.insert(identity).second)
        fail("duplicate resource identity");
      data.manifest.resources.push_back(identity);
    }
    expect("inputs");
    expect("{");
    std::set<std::uint64_t> input_ids;
    std::set<std::string> input_names;
    while (!take("}")) {
      bound(data.declarations.size(), 4096, "input declarations");
      expect("declaration");
      if (number() != data.declarations.size())
        fail("declaration indices must be consecutive from zero");
      WorkflowInputDeclaration input;
      input.id = number();
      input.name = quoted(128);
      input.result_schema = schema();
      if (!input.id || !input_ids.insert(input.id).second ||
          !input_names.insert(input.name).second)
        fail("duplicate/invalid input id or name");
      data.declarations.push_back(std::move(input));
    }
    ir_body(&data);
    if (physical)
      plan_body(&data);
    if (!token_.empty() || token_quoted_)
      fail("unexpected trailing token");
    return data;
  }

 private:
  static bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  }
  static bool punctuation(char c) {
    return c == '{' || c == '}' || c == '[' || c == ']';
  }
  [[noreturn]] void fail(const std::string& message) const {
    std::size_t line = 1, column = 1;
    for (std::size_t i = 0; i < token_offset_ && i < text_.size(); ++i) {
      if (text_[i] == '\n') {
        ++line;
        column = 1;
      } else {
        ++column;
      }
    }
    throw TextFailure{
        Status{ErrorCode::InvalidArgument,
               "compiler artifact " + std::to_string(line) + ":" +
                   std::to_string(column) + ": " + message,
               FailureReason::MalformedEnvelope,
               {FailureOrigin::Schema, FailureScope::Unspecified}}};
  }
  void next() {
    token_.clear();
    token_quoted_ = false;
    for (;;) {
      while (cursor_ < text_.size() && space(text_[cursor_]))
        ++cursor_;
      if (cursor_ == text_.size() || text_[cursor_] != ';')
        break;
      while (cursor_ < text_.size() && text_[cursor_] != '\n')
        ++cursor_;
    }
    token_offset_ = cursor_;
    if (cursor_ == text_.size())
      return;
    char c = text_[cursor_++];
    if (punctuation(c)) {
      token_.push_back(c);
      return;
    }
    if (c == '"') {
      token_quoted_ = true;
      while (cursor_ < text_.size()) {
        c = text_[cursor_++];
        if (c == '"')
          return;
        if (static_cast<unsigned char>(c) < 0x20)
          fail("raw control byte in quoted string");
        if (c == '\\') {
          if (cursor_ == text_.size())
            fail("unfinished string escape");
          c = text_[cursor_++];
          if (c == 'n') {
            c = '\n';
          } else if (c == 'r') {
            c = '\r';
          } else if (c == 't') {
            c = '\t';
          } else if (c == 'x') {
            if (text_.size() - cursor_ < 2)
              fail("unfinished hexadecimal string escape");
            const int a = hex_digit(text_[cursor_]);
            const int b = hex_digit(text_[cursor_ + 1]);
            if (a < 0 || b < 0)
              fail("invalid hexadecimal string escape");
            cursor_ += 2;
            c = static_cast<char>(a * 16 + b);
          } else if (c != '"' && c != '\\') {
            fail("unknown string escape");
          }
        }
        token_.push_back(c);
        if (token_.size() > 8192)
          fail("quoted string exceeds 8192 bytes");
      }
      fail("unterminated quoted string");
    }
    for (;;) {
      const auto byte = static_cast<unsigned char>(c);
      if (byte < 0x21 || byte > 0x7e || c == '"' || c == '\\')
        fail("invalid unquoted token");
      token_.push_back(c);
      if (token_.size() > 128)
        fail("unquoted token exceeds 128 bytes");
      if (cursor_ == text_.size() || space(text_[cursor_]) ||
          punctuation(text_[cursor_]) || text_[cursor_] == ';')
        return;
      c = text_[cursor_++];
    }
  }
  bool take(std::string_view value) {
    if (token_quoted_ || token_ != value)
      return false;
    next();
    return true;
  }
  void expect(std::string_view value) {
    if (!take(value))
      fail("expected " + std::string(value));
  }
  void bound(std::size_t count, std::size_t maximum, const char* kind) const {
    if (count >= maximum)
      fail(std::string("too many ") + kind);
  }
  std::string quoted(std::size_t maximum = 8192) {
    if (!token_quoted_ || token_.size() > maximum)
      fail("expected bounded quoted string");
    auto result = token_;
    next();
    return result;
  }
  std::uint64_t number() {
    std::uint64_t value = 0;
    const auto converted =
        std::from_chars(token_.data(), token_.data() + token_.size(), value);
    if (token_quoted_ || token_.empty() || converted.ec != std::errc{} ||
        converted.ptr != token_.data() + token_.size())
      fail("expected unsigned 64-bit decimal integer");
    next();
    return value;
  }
  std::uint32_t number32() {
    const auto value = number();
    if (value > UINT32_MAX)
      fail("integer exceeds uint32");
    return static_cast<std::uint32_t>(value);
  }
  std::size_t index() {
    const auto value = number();
    if (value > std::numeric_limits<std::size_t>::max())
      fail("index exceeds host size_t");
    return static_cast<std::size_t>(value);
  }
  std::vector<std::uint64_t> dimensions() {
    expect("[");
    std::vector<std::uint64_t> result;
    while (!take("]")) {
      bound(result.size(), 8, "tensor dimensions");
      result.push_back(number());
    }
    return result;
  }
  ElementType dtype() {
    if (take("u8"))
      return ElementType::UInt8;
    if (take("i8"))
      return ElementType::Int8;
    if (take("u16"))
      return ElementType::UInt16;
    if (take("i16"))
      return ElementType::Int16;
    if (take("i64"))
      return ElementType::Int64;
    if (take("f32"))
      return ElementType::Float32;
    if (take("f64"))
      return ElementType::Float64;
    fail("unknown tensor element type");
  }
  std::shared_ptr<const SchemaTemplate> schema() {
    expect("schema");
    auto result = std::make_shared<SchemaTemplate>();
    const auto id = quoted(128);
    result->id.assign(id.data(), id.size());
    result->version = number32();
    expect("{");
    while (take("tensor")) {
      bound(result->tensors.size(), 16, "tensor members");
      ResultTensorSpec tensor;
      const auto key = quoted(128);
      tensor.key.assign(key.data(), key.size());
      tensor.descriptor.element_type = dtype();
      expect("shape");
      tensor.descriptor.shape = dimensions();
      expect("batch");
      const auto batch = dimensions();
      tensor.batch_axes.assign(batch.begin(), batch.end());
      expect("atomic");
      tensor.atomic_trailing_axes = number32();
      expect("layout");
      if (take("spatial")) {
        tensor.layout.spatial = true;
        tensor.layout.height_axis = number32();
        tensor.layout.width_axis = number32();
        if (take("none"))
          tensor.layout.channel_axis.reset();
        else
          tensor.layout.channel_axis = number32();
      } else {
        expect("tensor");
        tensor.layout.spatial = false;
        tensor.layout.height_axis = 0;
        tensor.layout.width_axis = 0;
        tensor.layout.channel_axis.reset();
      }
      result->tensors.push_back(std::move(tensor));
    }
    while (take("publication")) {
      bound(result->publication_scopes.size(), 16, "publication scopes");
      ResultPublicationScope scope;
      if (take("complete_bundle"))
        scope.policy = ResultPublicationPolicy::CompleteBundle;
      else if (take("independent_chunks"))
        scope.policy = ResultPublicationPolicy::IndependentChunks;
      else if (take("stable_prefix"))
        scope.policy = ResultPublicationPolicy::StablePrefix;
      else
        fail("unknown publication policy");
      expect("[");
      while (!take("]")) {
        bound(scope.members.size(), 16, "publication members");
        scope.members.push_back(number32());
      }
      result->publication_scopes.push_back(std::move(scope));
    }
    expect("}");
    const auto status = result->validate();
    if (!status.ok())
      fail("invalid Result schema: " + status.message);
    return result;
  }
  std::vector<ResourceIdentity> used_resources(const ArtifactData& data) {
    expect("uses");
    expect("[");
    std::vector<ResourceIdentity> result;
    std::set<std::size_t> seen;
    while (!take("]")) {
      bound(result.size(), data.manifest.resources.size(), "resource uses");
      const auto slot = index();
      if (slot >= data.manifest.resources.size() || !seen.insert(slot).second)
        fail("duplicate or out-of-range resource use");
      result.push_back(data.manifest.resources[slot]);
    }
    std::sort(result.begin(), result.end());
    return result;
  }
  std::map<std::string, ParameterValue> parameters() {
    expect("parameters");
    expect("{");
    std::map<std::string, ParameterValue> result;
    while (!take("}")) {
      bound(result.size(), 1024, "parameters");
      const auto key = quoted(1024);
      ParameterValue value;
      if (take("i64")) {
        std::int64_t integer = 0;
        const auto converted = std::from_chars(
            token_.data(), token_.data() + token_.size(), integer);
        if (token_quoted_ || token_.empty() || converted.ec != std::errc{} ||
            converted.ptr != token_.data() + token_.size())
          fail("invalid i64 parameter");
        value = integer;
        next();
      } else if (take("f64")) {
        if (token_quoted_ || token_.empty())
          fail("invalid f64 parameter");
        double real = 0;
        std::istringstream stream(token_);
        stream.imbue(std::locale::classic());
        stream >> std::noskipws >> real;
        if (!stream || !stream.eof())
          fail("invalid f64 decimal; use f64_bits for exact IEEE bits");
        value = real;
        next();
      } else if (take("f64_bits")) {
        if (token_quoted_ || !hexadecimal(token_, 16))
          fail("f64_bits requires sixteen hexadecimal digits");
        std::uint64_t bits = 0;
        for (unsigned char c : token_)
          bits = (bits << 4) | static_cast<std::uint64_t>(hex_digit(c));
        double real = 0;
        std::memcpy(&real, &bits, sizeof(real));
        value = real;
        next();
      } else if (take("bool")) {
        if (take("true"))
          value = true;
        else if (take("false"))
          value = false;
        else
          fail("invalid boolean parameter");
      } else if (take("string")) {
        value = quoted();
      } else {
        fail("unknown parameter type");
      }
      if (key.empty() || !result.emplace(key, std::move(value)).second)
        fail("empty or duplicate parameter key");
    }
    return result;
  }
  std::vector<std::uint64_t> ids() {
    expect("[");
    std::vector<std::uint64_t> result;
    while (!take("]")) {
      bound(result.size(), kMaximumPlanSteps, "identifier list");
      result.push_back(number());
    }
    return result;
  }
  IREdge edge() {
    IREdge result;
    result.target = number();
    result.arguments = ids();
    return result;
  }
  TileSet tile_set() {
    expect("[");
    std::vector<TileInterval> intervals;
    while (!take("]")) {
      auto begin = number();
      auto end = number();
      if (begin >= end)
        fail("invalid tile interval");
      intervals.push_back({begin, end});
    }
    auto value = TileSet::from_intervals(ResourceBudget{}, intervals.data(),
                                         intervals.size());
    if (!value.ok())
      throw TextFailure{value.status()};
    return value.take_value();
  }
  TileRequest request() {
    expect("{");
    TileRequest value;
    while (!take("}")) {
      bound(value.members.size(), 16, "tile members");
      expect("member");
      auto member = number32();
      auto selected = tile_set();
      if (!value.members.emplace(member, std::move(selected)).second)
        fail("duplicate tile member");
    }
    return value;
  }
  Footprint samples() {
    expect("samples");
    auto shape = dimensions();
    expect("{");
    std::vector<Region> boxes;
    while (!take("}")) {
      bound(boxes.size(), kMaximumPlanSteps, "sample boxes");
      expect("box");
      auto origin = dimensions();
      auto extent = dimensions();
      if (origin.size() != extent.size())
        fail("box rank mismatch");
      std::vector<RegionDimension> dimensions;
      for (std::size_t i = 0; i < origin.size(); ++i) {
        if (extent[i] > UINT64_MAX - origin[i])
          fail("box overflow");
        dimensions.push_back({origin[i], extent[i]});
      }
      boxes.emplace_back(std::move(dimensions));
    }
    auto result =
        Footprint::from_regions(std::move(shape), boxes, static_set_limits());
    if (!result.ok())
      throw TextFailure{result.status()};
    return result.take_value();
  }
  std::vector<Footprint> sample_members() {
    expect("{");
    std::vector<Footprint> result;
    while (!take("}")) {
      bound(result.size(), 16, "sample members");
      result.push_back(samples());
    }
    return result;
  }
  void ir_body(ArtifactData* data) {
    expect("bindings");
    expect("{");
    while (!take("}")) {
      bound(data->input_bindings.size(), 4096, "IR bindings");
      expect("input");
      auto declaration = number();
      expect("value");
      data->input_bindings.push_back({declaration, number()});
    }
    expect("calls");
    expect("{");
    while (!take("}")) {
      bound(data->nodes.size(), kMaximumIRNodes, "IR calls");
      expect("call");
      IRRecord record;
      record.node.id = number();
      record.node.operation = quoted(1024);
      expect("{");
      record.resources = used_resources(*data);
      expect("arguments");
      record.node.inputs = ids();
      record.node.parameters = parameters();
      expect("results");
      expect("{");
      while (!take("}")) {
        bound(record.node.outputs.size(), 64, "call results");
        expect("value");
        SemanticOutput output;
        output.value = number();
        output.key = quoted(1024);
        output.result_schema = schema();
        record.node.outputs.push_back(std::move(output));
      }
      expect("}");
      data->nodes.push_back(std::move(record));
    }
    expect("returns");
    expect("{");
    while (!take("}")) {
      bound(data->ir_outputs.size(), 4096, "IR returns");
      expect("output");
      auto name = quoted(1024);
      data->ir_outputs.push_back({std::move(name), schema()});
    }
    expect("entry");
    data->entry = number();
    expect("blocks");
    expect("{");
    while (!take("}")) {
      bound(data->blocks.size(), 65536, "IR blocks");
      expect("block");
      IRBlock block;
      block.id = number();
      expect("{");
      expect("parameters");
      expect("{");
      while (!take("}")) {
        bound(block.parameters.size(), 4096, "block parameters");
        expect("value");
        auto value = number();
        block.parameters.push_back({value, schema()});
      }
      expect("calls");
      block.calls = ids();
      if (take("jump")) {
        block.terminator = IRJump{edge()};
      } else if (take("branch")) {
        IRConditionalJump branch;
        branch.condition = number();
        expect("true");
        branch.if_true = edge();
        expect("false");
        branch.if_false = edge();
        block.terminator = std::move(branch);
      } else {
        expect("return");
        block.terminator = IRReturn{ids()};
      }
      expect("}");
      data->blocks.push_back(std::move(block));
    }
  }
  void plan_body(ArtifactData* data) {
    expect("placements");
    expect("{");
    while (!take("}")) {
      bound(data->steps.size(), kMaximumPlanSteps, "plan placements");
      expect("step");
      if (index() != data->steps.size())
        fail("nonconsecutive placement index");
      PlanRecord record;
      expect("call");
      record.step.node_id = number();
      expect("output");
      record.step.output_index = number32();
      expect("backend");
      if (take("cpu"))
        record.step.backend = Backend::Cpu;
      else if (take("gpu"))
        record.step.backend = Backend::Gpu;
      else
        fail("invalid placement backend");
      data->steps.push_back(std::move(record));
    }
    expect("outputs");
    expect("{");
    while (!take("}")) {
      expect("output");
      auto name = quoted(1024);
      expect("return");
      auto returned = index();
      if (!data->plan_outputs.emplace(name, returned).second)
        fail("duplicate plan output");
      expect("request");
      data->options.output_requests.emplace(name, request());
    }
    expect("code");
    expect("{");
    std::size_t total = 0;
    while (!take("}")) {
      expect("block");
      auto block = number();
      expect("{");
      std::vector<PlanTileInstruction> instructions;
      while (!take("}")) {
        bound(total++, kMaximumPlanSteps, "tile instructions");
        expect("unit");
        if (index() != instructions.size())
          fail("nonconsecutive instruction index");
        PlanTileInstruction instruction;
        expect("step");
        instruction.step_index = index();
        expect("stage");
        instruction.shared_stage = take("none") ? UINT32_MAX : number32();
        expect("member");
        instruction.tensor_slot = number32();
        expect("tile");
        instruction.tile_unit = number();
        expect("cpu_fallback");
        const auto fallback = number32();
        if (fallback > 1)
          fail("invalid CPU fallback flag");
        instruction.cpu_fallback = fallback != 0;
        expect("{");
        expect("dependencies");
        for (auto item : ids()) {
          if (item >= instructions.size())
            fail("dependency must precede instruction");
          instruction.dependencies.push_back(static_cast<std::size_t>(item));
        }
        expect("publication");
        instruction.publication = request();
        expect("published_samples");
        instruction.publication_samples = sample_members();
        expect("tensor_output");
        if (!take("none"))
          instruction.tensor_outputs = samples();
        expect("reads");
        expect("{");
        while (!take("}")) {
          expect("input");
          PlanTensorRead read;
          read.input = number32();
          expect("member");
          read.tensor_slot = number32();
          expect("descriptor");
          if (take("true"))
            read.descriptor_only = true;
          else if (take("false"))
            read.descriptor_only = false;
          else
            fail("invalid descriptor flag");
          expect("tiles");
          read.tiles = tile_set();
          read.samples = samples();
          instruction.inputs.push_back(std::move(read));
        }
        expect("shared");
        expect("{");
        while (!take("}")) {
          expect("stage");
          PlanSharedRead read;
          read.stage = number32();
          read.tiles = request();
          read.samples = sample_members();
          instruction.shared_inputs.push_back(std::move(read));
        }
        expect("}");
        instructions.push_back(std::move(instruction));
      }
      if (!data->code.emplace(block, std::move(instructions)).second)
        fail("duplicate code block");
    }
  }

  std::string_view text_;
  std::size_t cursor_ = 0, token_offset_ = 0;
  std::string token_;
  bool token_quoted_ = false;
};

/** @brief Incrementally bound output storage; never allocate an unbounded dump.
 */
class Writer final {
 public:
  Writer& text(std::string_view text) {
    if (text.size() > kMaximumArtifactBytes - value_.size())
      throw TextFailure{Status{ErrorCode::ResourceExhausted,
                               "compiler artifact exceeds 64 MiB"}};
    value_.append(text.data(), text.size());
    return *this;
  }
  Writer& number(std::uint64_t number) { return text(std::to_string(number)); }
  Writer& quoted(std::string_view value) {
    text("\"");
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned char c : value) {
      if (c == '"' || c == '\\') {
        text("\\");
        const char character = static_cast<char>(c);
        text(std::string_view(&character, 1));
      } else if (c == '\n') {
        text("\\n");
      } else if (c == '\r') {
        text("\\r");
      } else if (c == '\t') {
        text("\\t");
      } else if (c < 0x20 || c == 0x7f) {
        const char escaped[] = {'\\', 'x', digits[c >> 4], digits[c & 15]};
        text(std::string_view(escaped, 4));
      } else {
        const char character = static_cast<char>(c);
        text(std::string_view(&character, 1));
      }
    }
    return text("\"");
  }
  std::string finish() { return std::move(value_); }

 private:
  std::string value_;
};

const char* dtype_name(ElementType type) {
  switch (type) {
    case ElementType::UInt8:
      return "u8";
    case ElementType::Int8:
      return "i8";
    case ElementType::UInt16:
      return "u16";
    case ElementType::Int16:
      return "i16";
    case ElementType::Int64:
      return "i64";
    case ElementType::Float32:
      return "f32";
    case ElementType::Float64:
      return "f64";
  }
  throw TextFailure{Status{ErrorCode::InvalidArgument, "invalid tensor dtype"}};
}

template <class Values>
void write_numbers(Writer* writer, const Values& values) {
  writer->text("[");
  for (auto value : values)
    writer->text(" ").number(value);
  writer->text(" ]");
}

void write_schema(Writer* writer, const SchemaTemplate& schema,
                  const std::string& indent) {
  auto& w = *writer;
  w.text("schema ")
      .quoted(schema.id)
      .text(" ")
      .number(schema.version)
      .text(" {\n");
  for (const auto& tensor : schema.tensors) {
    w.text(indent)
        .text("  tensor ")
        .quoted(tensor.key)
        .text(" ")
        .text(dtype_name(tensor.descriptor.element_type))
        .text(" shape ");
    write_numbers(&w, tensor.descriptor.shape);
    w.text(" batch ");
    write_numbers(&w, tensor.batch_axes);
    w.text(" atomic ").number(tensor.atomic_trailing_axes).text(" layout ");
    if (tensor.layout.spatial) {
      w.text("spatial ")
          .number(tensor.layout.height_axis)
          .text(" ")
          .number(tensor.layout.width_axis)
          .text(" ");
      if (tensor.layout.channel_axis)
        w.number(*tensor.layout.channel_axis);
      else
        w.text("none");
    } else {
      w.text("tensor");
    }
    w.text("\n");
  }
  for (const auto& scope : schema.publication_scopes) {
    w.text(indent).text("  publication ");
    if (scope.policy == ResultPublicationPolicy::CompleteBundle)
      w.text("complete_bundle ");
    else if (scope.policy == ResultPublicationPolicy::StablePrefix)
      w.text("stable_prefix ");
    else
      w.text("independent_chunks ");
    write_numbers(&w, scope.members);
    w.text("\n");
  }
  w.text(indent).text("}");
}

void write_parameters(Writer* writer,
                      const std::map<std::string, ParameterValue>& parameters) {
  auto& w = *writer;
  w.text("    parameters {\n");
  for (const auto& parameter : parameters) {
    w.text("      ").quoted(parameter.first).text(" ");
    const auto& value = parameter.second;
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
      w.text("i64 ").text(std::to_string(*integer));
    } else if (const auto* real = std::get_if<double>(&value)) {
      std::ostringstream stream;
      stream.imbue(std::locale::classic());
      // Decimal conversion of subnormals can report underflow on some
      // libraries.
      std::uint64_t bits = 0;
      std::memcpy(&bits, real, sizeof(bits));
      const auto exponent = (bits >> 52) & 0x7ffU;
      const auto fraction = bits & ((std::uint64_t{1} << 52) - 1);
      if (exponent == 0x7ffU || (!exponent && fraction)) {
        stream << std::hex << std::setw(16) << std::setfill('0') << bits;
        w.text("f64_bits ").text(stream.str());
      } else {
        stream << std::setprecision(std::numeric_limits<double>::max_digits10)
               << *real;
        w.text("f64 ").text(stream.str());
      }
    } else if (const auto* boolean = std::get_if<bool>(&value)) {
      w.text(*boolean ? "bool true" : "bool false");
    } else {
      w.text("string ").quoted(std::get<std::string>(value));
    }
    w.text("\n");
  }
  w.text("    }\n");
}

void write_resources(Writer* writer, const std::vector<ResourceIdentity>& used,
                     const std::map<ResourceIdentity, std::size_t>& slots) {
  writer->text("    uses [");
  for (const auto& resource : used)
    writer->text(" ").number(slots.at(resource));
  writer->text(" ]\n");
}

std::string hash_string(const ResourceIdentity& resource) {
  constexpr char digits[] = "0123456789abcdef";
  std::string hash;
  hash.reserve(64);
  for (auto byte : resource.sha256) {
    hash.push_back(digits[byte >> 4]);
    hash.push_back(digits[byte & 15]);
  }
  return hash;
}

void write_request(Writer* w, const TileRequest& request) {
  w->text("{");
  for (const auto& member : request.members) {
    w->text(" member ").number(member.first).text(" [");
    for (const auto interval : member.second.intervals())
      w->text(" ").number(interval.begin).text(" ").number(interval.end);
    w->text(" ]");
  }
  w->text(" }");
}
void write_samples(Writer* w, const Footprint& samples) {
  w->text("samples ");
  write_numbers(w, samples.shape());
  w->text(" {");
  for (const auto& box : samples.boxes()) {
    w->text(" box ");
    std::vector<std::uint64_t> offsets, extents;
    for (const auto& dimension : box.dimensions()) {
      offsets.push_back(dimension.offset);
      extents.push_back(dimension.extent);
    }
    write_numbers(w, offsets);
    w->text(" ");
    write_numbers(w, extents);
  }
  w->text(" }");
}
void write_sample_members(Writer* w, const std::vector<Footprint>& members) {
  w->text("{ ");
  for (const auto& samples : members) {
    write_samples(w, samples);
    w->text(" ");
  }
  w->text("}");
}
void write_edge(Writer* w, const IREdge& edge) {
  w->number(edge.target).text(" ");
  write_numbers(w, edge.arguments);
}

std::string write_data(const ArtifactData& data) {
  Writer w;
  const bool physical = data.manifest.kind == ArtifactKind::ExecutionPlan;
  if (physical) {
    w.text("psxp 2\n");
  } else {
    w.text("psir 2\nstage ")
        .text(data.manifest.kind == ArtifactKind::SemanticIR ? "semantic\n"
                                                             : "optimized\n");
  }
  w.text("parent ").quoted(data.parent_digest).text("\n");
  if (physical) {
    w.text("mode ").text(data.options.execution_mode == ExecutionMode::CpuExact
                             ? "cpu_exact\n"
                             : "native_gpu\n");
    w.text("tile_extent ").number(data.options.tile_extent).text("\n");
  }
  w.text("operations {\n");
  for (const auto& operation : data.manifest.operations)
    w.text("  require ")
        .quoted(operation.key)
        .text(" ")
        .quoted(operation.contract_digest)
        .text("\n");
  w.text("}\nresources {\n");
  std::map<ResourceIdentity, std::size_t> resource_slots;
  for (const auto& resource : data.manifest.resources) {
    const auto index = resource_slots.size();
    resource_slots.emplace(resource, index);
    w.text("  resource ")
        .number(index)
        .text(" ")
        .number(resource.byte_length)
        .text(" ")
        .quoted(hash_string(resource))
        .text("\n");
  }
  w.text("}\ninputs {\n");
  for (std::size_t i = 0; i < data.declarations.size(); ++i) {
    const auto& input = data.declarations[i];
    w.text("  declaration ")
        .number(i)
        .text(" ")
        .number(input.id)
        .text(" ")
        .quoted(input.name)
        .text(" ");
    write_schema(&w, *input.result_schema, "  ");
    w.text("\n");
  }
  w.text("}\n");
  w.text("bindings {\n");
  for (const auto& binding : data.input_bindings)
    w.text("  input ")
        .number(binding.declaration_id)
        .text(" value ")
        .number(binding.value)
        .text("\n");
  w.text("}\ncalls {\n");
  for (const auto& record : data.nodes) {
    const auto& node = record.node;
    w.text("  call ")
        .number(node.id)
        .text(" ")
        .quoted(node.operation)
        .text(" {\n");
    write_resources(&w, record.resources, resource_slots);
    w.text("    arguments ");
    write_numbers(&w, node.inputs);
    w.text("\n");
    write_parameters(&w, node.parameters);
    w.text("    results {\n");
    for (const auto& output : node.outputs) {
      w.text("      value ")
          .number(output.value)
          .text(" ")
          .quoted(output.key)
          .text(" ");
      write_schema(&w, *output.result_schema, "      ");
      w.text("\n");
    }
    w.text("    }\n  }\n");
  }
  w.text("}\nreturns {\n");
  for (const auto& output : data.ir_outputs) {
    w.text("  output ").quoted(output.name).text(" ");
    write_schema(&w, *output.result_schema, "  ");
    w.text("\n");
  }
  w.text("}\nentry ").number(data.entry).text("\nblocks {\n");
  for (const auto& block : data.blocks) {
    w.text("  block ").number(block.id).text(" {\n    parameters {\n");
    for (const auto& parameter : block.parameters) {
      w.text("      value ").number(parameter.value).text(" ");
      write_schema(&w, *parameter.result_schema, "      ");
      w.text("\n");
    }
    w.text("    }\n    calls ");
    write_numbers(&w, block.calls);
    w.text("\n    ");
    if (const auto* jump = std::get_if<IRJump>(&block.terminator)) {
      w.text("jump ");
      write_edge(&w, jump->edge);
    } else if (const auto* branch =
                   std::get_if<IRConditionalJump>(&block.terminator)) {
      w.text("branch ").number(branch->condition).text(" true ");
      write_edge(&w, branch->if_true);
      w.text(" false ");
      write_edge(&w, branch->if_false);
    } else {
      w.text("return ");
      write_numbers(&w, std::get<IRReturn>(block.terminator).values);
    }
    w.text("\n  }\n");
  }
  w.text("}\n");
  if (physical) {
    w.text("placements {\n");
    for (std::size_t i = 0; i < data.steps.size(); ++i) {
      const auto& step = data.steps[i].step;
      w.text("  step ")
          .number(i)
          .text(" call ")
          .number(step.node_id)
          .text(" output ")
          .number(step.output_index)
          .text(" backend ")
          .text(step.backend == Backend::Cpu ? "cpu\n" : "gpu\n");
    }
    w.text("}\noutputs {\n");
    for (const auto& output : data.plan_outputs) {
      w.text("  output ")
          .quoted(output.first)
          .text(" return ")
          .number(output.second)
          .text(" request ");
      write_request(&w, data.options.output_requests.at(output.first));
      w.text("\n");
    }
    w.text("}\ncode {\n");
    for (const auto& entry : data.code) {
      w.text("  block ").number(entry.first).text(" {\n");
      for (std::size_t i = 0; i < entry.second.size(); ++i) {
        const auto& instruction = entry.second[i];
        w.text("    unit ")
            .number(i)
            .text(" step ")
            .number(instruction.step_index)
            .text(" stage ");
        if (instruction.shared_stage == UINT32_MAX)
          w.text("none");
        else
          w.number(instruction.shared_stage);
        w.text(" member ")
            .number(instruction.tensor_slot)
            .text(" tile ")
            .number(instruction.tile_unit)
            .text(" cpu_fallback ")
            .number(instruction.cpu_fallback)
            .text(" {\n      dependencies ");
        write_numbers(&w, instruction.dependencies);
        w.text("\n      publication ");
        write_request(&w, instruction.publication);
        w.text("\n      published_samples ");
        write_sample_members(&w, instruction.publication_samples);
        w.text("\n      tensor_output ");
        if (instruction.tensor_outputs)
          write_samples(&w, *instruction.tensor_outputs);
        else
          w.text("none");
        w.text("\n      reads {\n");
        for (const auto& read : instruction.inputs) {
          w.text("        input ")
              .number(read.input)
              .text(" member ")
              .number(read.tensor_slot)
              .text(" descriptor ")
              .text(read.descriptor_only ? "true" : "false")
              .text(" tiles [");
          for (auto interval : read.tiles.intervals())
            w.text(" ").number(interval.begin).text(" ").number(interval.end);
          w.text(" ] ");
          write_samples(&w, read.samples);
          w.text("\n");
        }
        w.text("      }\n      shared {\n");
        for (const auto& read : instruction.shared_inputs) {
          w.text("        stage ").number(read.stage).text(" ");
          write_request(&w, read.tiles);
          w.text(" ");
          write_sample_members(&w, read.samples);
          w.text("\n");
        }
        w.text("      }\n    }\n");
      }
      w.text("  }\n");
    }
    w.text("}\n");
  }
  return w.finish();
}

Status io_failure(const std::string& message) {
  return {ErrorCode::OperationFailed,
          message,
          FailureReason::ShortIo,
          {FailureOrigin::Io, FailureScope::Unspecified}};
}
bool valid_path(const std::string& path) {
  return !path.empty() && path.size() <= 4096 &&
         path.find('\0') == std::string::npos;
}
}  // namespace

Result<ArtifactData> decode_artifact(std::string_view text) {
  try {
    NumericEnvironment environment;
    return Result<ArtifactData>(Reader(text).parse());
  } catch (const TextFailure& failure) {
    return Result<ArtifactData>(failure.status);
  } catch (const std::bad_alloc&) {
    return Result<ArtifactData>(allocation_failure());
  } catch (const std::length_error&) {
    return Result<ArtifactData>(allocation_failure());
  }
}

Result<std::string> encode_artifact(const ArtifactData& data) {
  try {
    NumericEnvironment environment;
    return Result<std::string>(write_data(data));
  } catch (const TextFailure& failure) {
    return Result<std::string>(failure.status);
  } catch (const std::bad_alloc&) {
    return Result<std::string>(allocation_failure());
  } catch (const std::length_error&) {
    return Result<std::string>(allocation_failure());
  }
}

Result<std::string> read_artifact_file(const std::string& path) {
  if (!valid_path(path))
    return Result<std::string>(
        Status{ErrorCode::InvalidArgument, "invalid compiler artifact path"});
  try {
    std::ifstream file(path, std::ios::binary);
    if (!file)
      return Result<std::string>(
          Status{ErrorCode::NotFound, "cannot open compiler artifact"});
    std::string text;
    std::array<char, 16384> buffer{};
    while (file) {
      file.read(buffer.data(), buffer.size());
      const auto count = static_cast<std::size_t>(file.gcount());
      if (count > kMaximumArtifactBytes - text.size())
        return Result<std::string>(Status{ErrorCode::InvalidArgument,
                                          "compiler artifact exceeds 64 MiB"});
      text.append(buffer.data(), count);
    }
    if (file.bad())
      return Result<std::string>(io_failure("cannot read compiler artifact"));
    return Result<std::string>(std::move(text));
  } catch (const std::bad_alloc&) {
    return Result<std::string>(allocation_failure());
  }
}

Status write_artifact_file(const std::string& path,
                           const Result<std::string>& text) {
  if (!text.ok())
    return text.status();
  if (!valid_path(path))
    return {ErrorCode::InvalidArgument, "invalid compiler artifact path"};
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file)
    return io_failure("cannot open compiler artifact for writing");
  file.write(text.value().data(),
             static_cast<std::streamsize>(text.value().size()));
  file.close();
  if (!file)
    return io_failure("cannot write compiler artifact");
  return Status::success();
}
}  // namespace ps::compiler_internal
