#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../../examples/numeric_workflow/result_fixture.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
Value values(const std::vector<double>& numbers) {
  std::vector<std::uint8_t> bytes(numbers.size() * 8);
  std::memcpy(bytes.data(), numbers.data(), bytes.size());
  return Value::create({ElementType::Float64, {numbers.size()}},
                       Region::whole({numbers.size()}), {0, {8}}, bytes)
      .take_value();
}
WorkflowDocument document(std::uint64_t n, std::int64_t block) {
  WorkflowDocument doc;
  WorkflowInputDeclaration input;
  input.id = 1;
  input.name = "x";
  SchemaTemplate schema;
  schema.id = "test.scan.input";
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ElementType::Float64, {n}};
  schema.tensors.push_back(std::move(tensor));
  input.result_schema = std::make_shared<SchemaTemplate>(std::move(schema));
  doc.inputs = {input};
  doc.nodes = {{1,
                "numeric.ordered_scan",
                {WorkflowInputReference{1}},
                {{"block_size", block}}}};
  doc.outputs = {{"y", 1, "value"}};
  return doc;
}
Footprint point(std::uint64_t at, std::uint64_t n) {
  return Footprint::from_regions({n}, {Region({{at, 1}})}).take_value();
}
ExecutionContextConfig config(std::uint64_t cache = 512) {
  ExecutionContextConfig result;
  result.cpu_workers = 1;
  result.result_cache_bytes = cache;
  result.managed_resources = ResourceLimits{};
  return result;
}
ExecutionBindings bindings_for(const ResourceBudget& root,
                               const std::vector<double>& numbers,
                               const WorkflowDocument& doc) {
  return {
      {{"x", numeric_result_fixture::source(
                 root, values(numbers), doc.inputs[0].result_schema.get())}}};
}
Status read(const ResultRef& result, std::uint64_t index, double* out) {
  return numeric_result_fixture::read(result, {index}, out, sizeof(*out));
}
struct ScanHooks {
  std::function<Status(const ResultProgramNeed&)> need;
  unsigned invalid = 0, starts = 0, publishes = 0, restores = 0;
  bool probe_checkpoints = false;
  std::uint64_t sequence = 0;
  ResultRef state, foreign;
  std::optional<ResultCheckpoint> latest, previous;
};
struct ScanProgram {
  ResultContinuation inner;
  std::shared_ptr<ScanHooks> hooks;
  ScanProgram(ResultContinuation program, std::shared_ptr<ScanHooks> observed)
      : inner(std::move(program)), hooks(std::move(observed)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto forwarded = phase;
    forwarded.checkpoint_before = [&](std::uint32_t kind,
                                      std::uint64_t before) {
      auto found =
          phase.checkpoint_before(hooks->invalid == 1 ? 0 : kind, before);
      if (found.ok() && found.value()) {
        const auto& checkpoint = *found.value();
        if (checkpoint.phase() != kind || checkpoint.sequence() > before)
          return Result<std::optional<ResultCheckpoint>>(
              Status{ErrorCode::OperationFailed, "future checkpoint"});
        ++hooks->restores;
      }
      return found;
    };
    forwarded.checkpoint_publish = [&](std::uint32_t kind,
                                       std::uint64_t sequence,
                                       const ResultRef& state) {
      auto status =
          phase.checkpoint_publish(hooks->invalid == 2 ? 0 : kind, sequence,
                                   hooks->invalid == 3   ? ResultRef{}
                                   : hooks->invalid == 4 ? hooks->foreign
                                                         : state);
      if (status.ok()) {
        ++hooks->publishes;
        hooks->sequence = sequence;
        hooks->state = state;
        if (!hooks->probe_checkpoints)
          return status;
        auto found = phase.checkpoint_before(kind, sequence);
        if (!found.ok())
          return found.status();
        hooks->latest = found.take_value();
        if (sequence) {
          auto previous = phase.checkpoint_before(kind, sequence - 1);
          if (!previous.ok())
            return previous.status();
          hooks->previous = previous.take_value();
        }
      }
      return status;
    };
    auto result = inner.poll(forwarded);
    if (result.ok() && hooks->need) {
      if (const auto* need = std::get_if<ResultProgramNeed>(&result.value())) {
        auto status = hooks->need(*need);
        if (!status.ok())
          return Result<ResultProgramPoll>(status);
      }
    }
    return result;
  }
};
struct ScanPreparation {
  std::shared_ptr<const PreparedOperation> inner;
};
std::shared_ptr<OperationRegistry> observed_registry(
    const std::shared_ptr<ScanHooks>& hooks) {
  auto base = make_default_operation_registry();
  auto registry = make_default_operation_registry(false);
  OperationDefinition operation;
  operation.key = "test.scan";
  operation.traits = base->find_traits("numeric.ordered_scan").take_value();
  operation.traits.outputs[0].continuation_bytes += sizeof(ScanProgram);
  operation.prepare_static = [base](const auto& inputs,
                                    const auto& parameters) {
    auto inner =
        base->prepare_operation("numeric.ordered_scan", inputs, parameters);
    if (!inner.ok())
      return Result<OperationPreparation>(inner.status());
    OperationPreparation prepared;
    prepared.outputs.resize(1);
    prepared.outputs[0].metadata.result_schema =
        std::make_shared<SchemaTemplate>(
            *inner.value()->traits().outputs[0].result_schema);
    prepared.state =
        std::make_shared<ScanPreparation>(ScanPreparation{inner.take_value()});
    return Result<OperationPreparation>(std::move(prepared));
  };
  operation.start_result = [base, hooks](const auto& query,
                                         const auto& allocator) {
    ++hooks->starts;
    auto forwarded = query;
    forwarded.prepared =
        static_cast<const ScanPreparation*>(query.prepared->state())->inner;
    auto inner =
        base->start_result("numeric.ordered_scan", forwarded, allocator);
    if (!inner.ok())
      return inner;
    return ResultContinuation::make<ScanProgram>(allocator, inner.take_value(),
                                                 hooks);
  };
  numeric_result_fixture::require(
      registry->register_operation(std::move(operation)).ok(), "register scan");
  numeric_result_fixture::require(registry->freeze().ok(), "freeze scan");
  return registry;
}
int checkpoint_guards() {
  auto hooks = std::make_shared<ScanHooks>();
  hooks->probe_checkpoints = true;
  auto registry = observed_registry(hooks);
  auto doc = document(2, 1);
  doc.nodes[0].operation = "test.scan";
  GraphContext graph(doc);
  const auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, config());
  auto root = context.resource_budget().take_value();
  auto demand =
      context.open_demand(plan, bindings_for(root, {1, 2}, doc)).take_value();
  auto completed = demand.request({{"y", point(1, 2)}});
  double second = 0;
  PS_CHECK(completed.ok() &&
           read(completed.value().results.at("y"), 1, &second).ok() &&
           second == 3);
  PS_CHECK(hooks->publishes == 2 && hooks->sequence == 1 &&
           read(hooks->state, 0, &second).ok() && second == 3);
  PS_CHECK(hooks->latest && hooks->latest->phase() == 1 &&
           hooks->latest->sequence() == 1 &&
           read(hooks->latest->state(), 0, &second).ok() && second == 3);
  PS_CHECK(hooks->previous && hooks->previous->sequence() == 0 &&
           read(hooks->previous->state(), 0, &second).ok() && second == 1);
  auto first = demand.request({{"y", point(0, 2)}});
  PS_CHECK(first.ok() && read(first.value().results.at("y"), 0, &second).ok() &&
           second == 1);
  const auto before_edit = hooks->restores;
  PS_CHECK(demand.replace_bindings(bindings_for(root, {4, 2}, doc)).ok());
  auto edited = demand.request({{"y", point(1, 2)}});
  PS_CHECK(edited.ok() && hooks->restores == before_edit &&
           read(edited.value().results.at("y"), 1, &second).ok() &&
           second == 6);
  // Different static block sizes have separate scopes over the same input.
  auto other_doc = doc;
  other_doc.nodes.push_back({9,
                             "test.scan",
                             {WorkflowInputReference{1}},
                             {{"block_size", INT64_C(2)}}});
  other_doc.outputs.push_back({"other", 9, "value"});
  GraphContext other_graph(other_doc);
  auto other_plan = Compiler(registry).compile(other_graph).take_value().plan;
  const auto starts_before = hooks->starts;
  const auto publishes_before = hooks->publishes;
  auto other =
      context.execute(other_plan, bindings_for(root, {8, 2}, other_doc));
  PS_CHECK(other.ok() && hooks->restores == before_edit &&
           read(other.value().results.at("y"), 1, &second).ok() &&
           second == 10);
  PS_CHECK(read(other.value().results.at("other"), 1, &second).ok() &&
           second == 10);
  PS_CHECK(hooks->starts == starts_before + 2 &&
           hooks->publishes == publishes_before + 4 &&
           other.value().results.at("y").object_id() !=
               other.value().results.at("other").object_id());
  for (unsigned bad = 1; bad <= 4; ++bad) {
    auto invalid = std::make_shared<ScanHooks>();
    invalid->invalid = bad;
    if (bad == 4)
      invalid->foreign =
          numeric_result_fixture::source(ResourceBudget{}, values({3}));
    auto bad_registry = observed_registry(invalid);
    GraphContext bad_graph(doc);
    auto bad_plan = Compiler(bad_registry).compile(bad_graph).take_value().plan;
    ExecutionContext bad_context(bad_registry, config());
    auto failed = bad_context.execute(
        bad_plan,
        bindings_for(bad_context.resource_budget().take_value(), {1, 2}, doc));
    PS_CHECK(failed.status().code == ErrorCode::InvalidArgument);
  }
  ExecutionOptions bounded;
  bounded.maximum_dependency_cache_work = 0;
  const auto published_before = hooks->publishes;
  auto uncached =
      context.execute(plan, bindings_for(root, {4, 2}, doc), {}, bounded);
  PS_CHECK(uncached.ok() &&
           read(uncached.value().results.at("y"), 1, &second).ok() &&
           second == 6);
  PS_CHECK(hooks->publishes == published_before + 2 &&
           hooks->restores == before_edit && !hooks->latest &&
           !hooks->previous);

  const auto owned = numeric_result_fixture::source(root, values({3}));
  PS_CHECK(owned.owned_by(root) && !owned.owned_by(ResourceBudget{}));
  return 0;
}
int exact_reads(bool ancestor) {
  constexpr std::uint64_t n = 256;
  std::uint64_t samples = 0, next = 0;
  auto hooks = std::make_shared<ScanHooks>();
  hooks->need = [&](const ResultProgramNeed& need) {
    for (const auto& tensor : need.tensors) {
      if (tensor.input != 0 || tensor.slot != 0 || tensor.roles != 5)
        return Status{ErrorCode::OperationFailed, "scan Need type"};
      for (const auto& box : tensor.samples.boxes()) {
        if (box.dimensions()[0].offset != next)
          return Status{ErrorCode::OperationFailed, "scan reread prefix"};
        next += box.dimensions()[0].extent;
        samples += box.dimensions()[0].extent;
      }
    }
    return Status::success();
  };
  auto registry = observed_registry(hooks);
  auto doc = document(n, 16);
  doc.nodes[0].operation = "test.scan";
  if (ancestor) {
    doc.nodes[0].inputs = {WorkflowNodeOutput{2, "value"}};
    doc.nodes.push_back({2, "core.identity", {WorkflowInputReference{1}}, {}});
  }
  GraphContext graph(doc);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  std::vector<double> numbers(n);
  for (std::uint64_t i = 0; i < n; ++i)
    numbers[i] = static_cast<double>(i + 1);
  ExecutionContext context(registry, config(0));
  ExecutionOptions options;
  if (ancestor)
    options.maximum_dependency_work = 32U * 1024U * 1024U;
  auto result = context.execute(
      plan, bindings_for(context.resource_budget().take_value(), numbers, doc),
      {}, options);
  if (!result.ok())
    std::cerr << result.status().message << '\n';
  PS_CHECK(result.ok() && samples == n);
  const auto& output = result.value().results.at("y");
  for (std::uint64_t i = 0; i < n; ++i) {
    double value = 0;
    PS_CHECK(read(output, i, &value).ok() &&
             value == static_cast<double>((i + 1) * (i + 2) / 2));
  }
  for (auto i : {0U, 13U, 255U}) {
    auto restricted =
        result.value().dependencies.restrict({{"y", point(i, n)}});
    PS_CHECK(restricted.ok());
    auto support = restricted.value().source_support();
    const auto prefix =
        Footprint::from_regions({n}, {Region({{0, i + 1}})}).take_value();
    PS_CHECK(support.ok() && support.value().at("x") == prefix);
  }
  auto dirty = result.value().dependencies.potential_dirty(
      "x", point(13, n), 7, {}, ResultSupportTarget::Tensor, 0);
  PS_CHECK(
      dirty.ok() &&
      dirty.value().at("y") ==
          Footprint::from_regions({n}, {Region({{13, n - 13}})}).take_value());
  return 0;
}
int edited_prefixes() {
  auto registry = make_default_operation_registry();
  auto doc = document(4, 2);
  GraphContext graph(doc);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, config(256));
  auto root = context.resource_budget().take_value();
  auto bindings = bindings_for(root, {0, 1, 0x1p54, 4}, doc);
  auto demand = context.open_demand(plan, bindings).take_value();
  DemandQuery query{
      {"y", Footprint::from_regions({4}, {Region({{1, 3}})}).take_value()}};
  auto initial = demand.request(query);
  PS_CHECK(initial.ok());
  auto frozen = demand.freeze().take_value();
  bindings = bindings_for(root, {1, 1, 0x1p54, 4}, doc);
  auto edit = demand.replace_bindings(bindings);
  PS_CHECK(edit.ok() && edit.value().potential_dirty.at("y") == query.at("y"));
  auto changed = demand.request(query);
  auto old = context.execute_fragments(frozen, query);
  PS_CHECK(changed.ok() && old.ok());

  for (unsigned i = 1; i < 4; ++i) {
    double before = 0, after = 0, pinned = 0;
    PS_CHECK(read(initial.value().results.at("y"), i, &before).ok());
    PS_CHECK(read(changed.value().results.at("y"), i, &after).ok());
    PS_CHECK(read(old.value().results.at("y"), i, &pinned).ok());
    const double expected = i == 1 ? 1 : i == 2 ? 0x1p54 : 0x1p54 + 4;
    PS_CHECK(before == expected && pinned == expected);
    // Incoming 0/1 changes the current block's first output, although its
    // final outgoing accumulator reconverges at 2^54 by nearest-even rounding.
    PS_CHECK(after == (i == 1 ? 2 : expected));
  }
  return 0;
}
int block_reconvergence() {
  auto registry = make_default_operation_registry();
  auto doc = document(6, 1);
  GraphContext graph(doc);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, config());
  auto root = context.resource_budget().take_value();
  std::vector<double> numbers{0, 1, 0x1p54, 4, 5, 6};
  auto demand =
      context.open_demand(plan, bindings_for(root, numbers, doc)).take_value();
  DemandQuery query{{"y", point(5, 6)}};
  auto initial = demand.request(query);
  PS_CHECK(initial.ok());
  numbers[0] = 1;
  PS_CHECK(demand.replace_bindings(bindings_for(root, numbers, doc)).ok());
  auto changed = demand.request(query);
  PS_CHECK(changed.ok());
  volatile double expected = 0;
  for (const auto number : numbers)
    expected = expected + number;
  double actual = 0;
  PS_CHECK(read(changed.value().results.at("y"), 5, &actual).ok() &&
           actual == expected);
  auto evidence = changed.value().dependencies.source_support();
  PS_CHECK(evidence.ok() &&
           evidence.value().at("x") == Footprint::all({6}).value());
  auto dirty = changed.value().dependencies.potential_dirty(
      "x", point(0, 6), 7, {}, ResultSupportTarget::Tensor, 0);
  PS_CHECK(dirty.ok() && dirty.value().at("y") == point(5, 6));
  context.clear_result_cache();
  auto retained = demand.request(query);
  PS_CHECK(retained.ok() &&
           retained.value().diagnostics.shared_computations == 1);
  PS_CHECK(read(retained.value().results.at("y"), 5, &actual).ok() &&
           actual == expected);
  initial = Result<DemandResult>(DemandResult{});
  changed = Result<DemandResult>(DemandResult{});
  retained = Result<DemandResult>(DemandResult{});
  context.clear_result_cache();
  auto cleared = demand.request(query);
  PS_CHECK(cleared.ok());
  PS_CHECK(read(cleared.value().results.at("y"), 5, &actual).ok() &&
           actual == expected);
  cleared = Result<DemandResult>(DemandResult{});
  context.clear_result_cache();
  ExecutionOptions disabled;
  disabled.maximum_dependency_cache_work = 0;
  auto plain = demand.request(query, {}, disabled);
  PS_CHECK(plain.ok() && plain.value().diagnostics.cache_hits == 0);
  PS_CHECK(read(plain.value().results.at("y"), 5, &actual).ok() &&
           actual == expected);
  return 0;
}
int errors_and_order() {
  auto registry = make_default_operation_registry();
  const std::vector<double> input{1e16, 1, -1e16, 4, 1, 0x1p54, 2, 3};
  for (auto block : {1, 2, 3, 64}) {
    auto doc = document(input.size(), block);
    GraphContext graph(doc);
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    ExecutionContext context(registry, config());
    auto root = context.resource_budget().take_value();
    auto demand =
        context.open_demand(plan, bindings_for(root, input, doc)).take_value();
    for (unsigned warm = 0; warm < 2; ++warm) {
      auto result =
          demand.request({{"y", Footprint::all({input.size()}).take_value()}});
      if (!result.ok())
        std::cerr << result.status().message << '\n';
      PS_CHECK(result.ok());
      volatile double carry = 0;
      for (std::uint64_t i = 0; i < input.size(); ++i) {
        carry = carry + input[i];
        double value = 0, expected = carry;
        PS_CHECK(read(result.value().results.at("y"), i, &value).ok());
        PS_CHECK(std::memcmp(&value, &expected, 8) == 0);
      }
    }
  }
  auto doc = document(2, 64);
  GraphContext graph(doc);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContext context(registry, config(64));
  auto root = context.resource_budget().take_value();
  auto demand =
      context
          .open_demand(
              plan,
              bindings_for(root, {1, std::numeric_limits<double>::infinity()},
                           doc))
          .take_value();
  for (unsigned warm = 0; warm < 2; ++warm) {
    auto first = demand.request({{"y", point(0, 2)}});
    double output = 0;
    PS_CHECK(first.ok() &&
             read(first.value().results.at("y"), 0, &output).ok() &&
             output == 1);
    auto both = demand.request({{"y", Footprint::all({2}).take_value()}});
    PS_CHECK(both.status().code == ErrorCode::OperationFailed &&
             both.status().message == "nonfinite scan input 1");
    auto second = demand.request({{"y", point(1, 2)}});
    PS_CHECK(second.status().code == ErrorCode::OperationFailed &&
             second.status().message == "nonfinite scan input 1");
  }
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(checkpoint_guards() == 0);
  PS_CHECK(exact_reads(false) == 0);
  PS_CHECK(exact_reads(true) == 0);
  PS_CHECK(errors_and_order() == 0);
  PS_CHECK(edited_prefixes() == 0);
  PS_CHECK(block_reconvergence() == 0);
  return 0;
}
