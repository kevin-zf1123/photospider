#include "photospider/plugin/component_operation.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ps {
namespace {
using Op = ComponentOperation;
using Poll = Result<ResultProgramPoll>;
using Record = std::array<std::int64_t, 4>;
Status invalid(const char* message) {
  return {ErrorCode::TypeMismatch,
          message,
          FailureReason::InvalidAssociation,
          {FailureOrigin::Schema, FailureScope::Group}};
}
Status capacity(const char* message) {
  return {ErrorCode::ResourceExhausted,
          message,
          FailureReason::CapacityLimit,
          {FailureOrigin::Resource, FailureScope::Group}};
}
Status profile(const ComponentsSpec& spec) {
  constexpr auto maximum = (static_cast<std::uint64_t>(INT64_MAX) - 4095) / 32;
  if (!spec.height || !spec.width || spec.height > maximum / spec.width ||
      spec.maximum_count > INT64_MAX || spec.ids != ComponentIdScheme::MinPixel)
    return invalid("components4 requires bounded HW min-pixel identity");
  return Status::success();
}
Result<SchemaTemplate> derived_schema(const ComponentsSpec& spec, bool filter) {
  auto checked = profile(spec);
  if (!checked.ok())
    return Result<SchemaTemplate>(checked);
  auto made = components_schema(spec);
  if (!made.ok())
    return made;
  auto schema = made.take_value();
  schema.id = filter ? "photospider.component_filter"
                     : "photospider.component_area_index";
  schema.metadata[0].key =
      filter ? "component_filter_basis_v1" : "component_area_basis_v1";
  if (filter)
    schema.fields = {{"mask",
                      ElementType::UInt8,
                      {ResultExtentKind::Fixed, spec.height * spec.width},
                      {}}};
  else
    schema.fields = {
        {"rows", ElementType::Int64, {ResultExtentKind::RuntimeCount}, {2}}};
  checked = schema.validate(true);
  return checked.ok() ? Result<SchemaTemplate>(std::move(schema))
                      : Result<SchemaTemplate>(checked);
}
Status inputs(Op op, const ComponentsSpec& spec,
              const std::vector<OperationMetadata>& metadata,
              const std::map<std::string, ParameterValue>& parameters) {
  if (op == Op::Filter) {
    const auto found = parameters.find("minimum_area");
    if (found == parameters.end() ||
        !std::holds_alternative<std::int64_t>(found->second) ||
        std::get<std::int64_t>(found->second) <= 0)
      return {ErrorCode::InvalidArgument,
              "minimum_area must be positive Int64",
              FailureReason::InvalidDomain,
              {FailureOrigin::Schema, FailureScope::Group}};
  }
  if (metadata.size() != (op == Op::Filter ? 2U : 1U))
    return invalid("components4 input count mismatch");
  for (unsigned i = 0; i < metadata.size(); ++i) {
    const auto& input = metadata[i];
    if (op == Op::Labels) {
      if (input.result_schema ||
          input.descriptor.element_type != ElementType::UInt8 ||
          input.descriptor.shape !=
              std::vector<std::uint64_t>{spec.height, spec.width} ||
          !input.facets.empty())
        return invalid("components4 labels requires facet-free UInt8 HW");
    } else {
      auto expected = i ? component_area_schema(spec) : components_schema(spec);
      if (!expected.ok() || !input.result_schema ||
          !input.result_schema->same_schema(expected.value()))
        return invalid("components4 count/basis/schema mismatch");
    }
  }
  return Status::success();
}
struct State {
  enum Stage {
    Start,
    Inputs,
    Created,
    Extended,
    Source,
    SourceReady,
    Edges,
    EdgeReady,
    Neighbour,
    Find,
    Found,
    UnionWritten,
    Emit,
    EmitReady,
    EmitWritten,
    Area,
    AreaReady,
    AreaWritten,
    Verify,
    VerifyReady,
    Filter,
    LabelReady,
    Search,
    IndexReady,
    FilterWritten,
    Finish
  } stage = Start;
  Op op;
  ComponentsSpec spec;
  std::uint64_t n, position = 0, batch = 0, count = 0;
  std::uint64_t root_id = 0, find_id = 0, label_fill = 0, table_fill = 0,
                table_rows = 0;
  std::uint64_t label_rows = 0, index_first = 0, index_rows = 0, low = 0,
                high = 0;
  unsigned neighbour = 0, hops = 0;
  bool emit_find = false;
  std::int64_t threshold = 1, current_label = 0;
  Record root_record{}, found_record{};
  TemporaryStorage tree;
  MutableBuffer labels, table;
  std::array<ResultRef, 2> input;
  std::array<ResultDescriptor, 2> descriptors;
  std::shared_ptr<const CpuStorage> label_page, index_page;
  ResultBuilder builder;
  State(Op operation, ComponentsSpec specification)
      : op(operation), spec(specification), n(spec.height * spec.width) {}
  std::uint64_t window(const ResultProgramPhase& p) const {
    return std::min<std::uint64_t>(p.query.page_bytes, 1024);
  }
  Result<ResultRelation> relation(const ResultProgramPhase& p,
                                  std::uint64_t outputs) {
    std::array<ResultRelation, 4> parts;
    unsigned used = 0;
    for (unsigned i = 0; i < p.query.inputs.size(); ++i) {
      const auto samples = op == Op::Labels ? n
                           : i              ? descriptors[i].rows(0) * 2
                                            : n + descriptors[i].rows(1) * 3;
      auto made = ResultRelation::cartesian(
          p.resources, outputs, {i, input[i].valid() ? 7U : 15U, 0, samples},
          DependencyGuarantee::Conservative);
      if (!made.ok())
        return made;
      parts[used++] = made.take_value();
      if (input[i].valid()) {
        made = ResultRelation::cartesian(p.resources, outputs, {i, 8, 0, 1},
                                         DependencyGuarantee::Conservative);
        if (!made.ok())
          return made;
        parts[used++] = made.take_value();
      }
    }
    auto lease = p.resources.reserve(ResourceCapacity::host(
        used * sizeof(ResultRelation), used * sizeof(ResultRelation)));
    if (!lease.ok())
      return Result<ResultRelation>(lease.status());
    return ResultRelation::unite(
        p.resources,
        std::vector<ResultRelation>(parts.begin(), parts.begin() + used));
  }
  Status bad_index(const char* message) const {
    auto status = invalid(message);
    status.detail.scope = FailureScope::Association;
    status.detail.association = input[1].object_id();
    return status;
  }
  Status begin(const ResultProgramPhase& p) {
    auto lease = p.resources.reserve(ResourceCapacity::host(16, 16));
    if (!lease.ok())
      return lease.status();
    std::vector<std::uint64_t> association;
    association.reserve(2);
    for (const auto& value : input)
      if (value.valid())
        association.push_back(value.object_id());
    auto made = ResultBuilder::start(p.resources, *p.query.output.result_schema,
                                     p.query.semantic_key, {n, n * 32},
                                     std::move(association));
    if (!made.ok())
      return made.status();
    builder = made.take_value();
    auto support = relation(p, 1);
    return support.ok() ? builder.bind_descriptor_relation(support.take_value())
                        : support.status();
  }
  Poll finish(const ResultProgramPhase& p) {
    for (unsigned f = 0; f < builder.reference().schema().fields.size(); ++f) {
      const auto rows = (op == Op::Area || f == 1) ? count : n;
      auto support = relation(p, rows);
      if (!support.ok())
        return Poll(support.status());
      auto status = builder.publish(f, rows, support.take_value(),
                                    {true, true, true, true});
      if (!status.ok())
        return Poll(status);
    }
    auto result = builder.seal();
    return result.ok() ? Poll(ResultPublication{result.take_value(), true})
                       : Poll(result.status());
  }
  Result<ResultWriteTemporary> write_record(const ResultProgramPhase& p,
                                            std::uint64_t id, Record record) {
    auto memory = p.allocator.allocate(32);
    if (!memory.ok())
      return Result<ResultWriteTemporary>(memory.status());
    auto buffer = memory.take_value();
    std::memcpy(buffer.data(), record.data(), 32);
    return Result<ResultWriteTemporary>(
        ResultWriteTemporary{tree, (id - 1) * 32, std::move(buffer).freeze()});
  }
  Record record(const ResultProgramPhase& p) {
    Record value;
    std::memcpy(
        value.data(),
        std::get<std::shared_ptr<const CpuStorage>>(p.io.at(0))->bytes().data(),
        32);
    return value;
  }
  Poll emit(const ResultProgramPhase& p) {
    if (!labels.size()) {
      label_rows = std::min(n - position, window(p) / 8);
      auto memory = p.allocator.allocate(label_rows * 8);
      if (!memory.ok())
        return Poll(memory.status());
      labels = memory.take_value();
      label_fill = 0;
    }
    const std::int64_t label = find_id ? found_record[2] + 1 : 0;
    std::memcpy(labels.data() + label_fill * 8, &label, 8);
    ++label_fill;
    if (label && found_record[2] == static_cast<std::int64_t>(position)) {
      if (count == spec.maximum_count)
        return Poll(capacity("component count limit"));
      if (!table.size()) {
        table_rows = std::min(spec.maximum_count - count, window(p) / 24);
        auto memory = p.allocator.allocate(table_rows * 24);
        if (!memory.ok())
          return Poll(memory.status());
        table = memory.take_value();
        table_fill = 0;
      }
      const std::array<std::int64_t, 3> row{label, found_record[3],
                                            found_record[2]};
      std::memcpy(table.data() + table_fill * 24, row.data(), 24);
      ++table_fill;
      ++count;
    }
    ++position;
    ResultProgramNeed need;
    if (label_fill == label_rows) {
      auto write =
          builder.prepare_append(0, label_fill, std::move(labels).freeze());
      if (!write.ok())
        return Poll(write.status());
      need.io.push_back(write.take_value());
      label_fill = 0;
    }
    if (table_fill && (table_fill == table_rows || position == n)) {
      if (table_fill != table_rows) {
        auto memory = p.allocator.allocate(table_fill * 24);
        if (!memory.ok())
          return Poll(memory.status());
        auto smaller = memory.take_value();
        std::memcpy(smaller.data(), table.data(), table_fill * 24);
        table = std::move(smaller);
      }
      auto write =
          builder.prepare_append(1, table_fill, std::move(table).freeze());
      if (!write.ok())
        return Poll(write.status());
      need.io.push_back(write.take_value());
      table_fill = 0;
    }
    stage = EmitWritten;
    if (!need.io.empty())
      return Poll(std::move(need));
    // No publication or I/O is required for a partly filled output page.
    stage = Emit;
    return poll(p);
  }
  Poll poll(const ResultProgramPhase& p) {
    if (window(p) < (op == Op::Labels ? 32U : 24U))
      return Poll(capacity("components4 record exceeds page window"));
    for (;;)
      switch (stage) {
        case Start:
          stage = Inputs;
          if (op != Op::Labels) {
            ResultProgramNeed need;
            for (unsigned i = 0; i < p.query.inputs.size(); ++i)
              need.results.push_back({i, 0, true, 0});
            return Poll(std::move(need));
          }
          break;
        case Inputs: {
          for (const auto& item : p.results) {
            input[item.first] = item.second;
            auto d = item.second.descriptor();
            if (!d.ok())
              return Poll(d.status());
            descriptors[item.first] = d.take_value();
          }
          if (op != Op::Labels)
            count = descriptors[0].rows(1);
          if (op == Op::Filter) {
            threshold =
                std::get<std::int64_t>(p.query.parameters.at("minimum_area"));
            if (input[1].association().size() != 1 ||
                input[1].association()[0] != input[0].object_id() ||
                descriptors[1].rows(0) != count)
              return Poll(bad_index(
                  "area index belongs to a different component association"));
          }
          auto status = begin(p);
          if (!status.ok())
            return Poll(status);
          if (op != Op::Labels) {
            stage = op == Op::Area ? Area : Verify;
            break;
          }
          status = p.consume_work(n * 4);
          if (!status.ok())
            return Poll(status);
          stage = Created;
          return Poll(ResultProgramNeed{{}, {}, {ResultCreateTemporary{}}});
        }
        case Created:
          tree = std::get<TemporaryStorage>(p.io.at(0));
          stage = Extended;
          return Poll(
              ResultProgramNeed{{}, {}, {ResultExtendTemporary{tree, n * 32}}});
        case Extended:
          stage = Source;
          break;
        case Source: {
          if (position == n) {
            position = 0;
            stage = Edges;
            break;
          }
          batch = std::min({n - position, spec.width - position % spec.width,
                            window(p) / 32});
          auto requested = Footprint::from_regions(
              p.query.inputs[0].descriptor.shape,
              {Region({{position / spec.width, 1},
                       {position % spec.width, batch}})});
          if (!requested.ok())
            return Poll(requested.status());
          stage = SourceReady;
          return Poll(ResultProgramNeed{{{0, requested.take_value()}}, {}, {}});
        }
        case SourceReady: {
          auto memory = p.allocator.allocate(batch * 32);
          if (!memory.ok())
            return Poll(memory.status());
          auto buffer = memory.take_value();
          auto fuel = p.consume_work(batch * 4);
          if (!fuel.ok())
            return Poll(fuel);
          for (std::uint64_t i = 0; i < batch; ++i) {
            std::uint8_t mask = 0;
            auto status =
                p.read(0, {position / spec.width, position % spec.width + i},
                       &mask, 1);
            if (!status.ok())
              return Poll(status);
            if (mask) {
              const auto index = static_cast<std::int64_t>(position + i);
              const Record value{index + 1, 0, index, 1};
              std::memcpy(buffer.data() + i * 32, value.data(), 32);
            }
          }
          const auto offset = position * 32;
          position += batch;
          stage = Source;
          return Poll(ResultProgramNeed{
              {},
              {},
              {ResultWriteTemporary{tree, offset,
                                    std::move(buffer).freeze()}}});
        }
        case Edges:
          if (position == n) {
            position = 0;
            stage = Emit;
            break;
          }
          stage = EdgeReady;
          return Poll(ResultProgramNeed{
              {},
              {},
              {ResultReadTemporary{tree, position * 32, 32}}});
        case EdgeReady: {
          root_record = record(p);
          auto fuel = p.consume_work(1);
          if (!fuel.ok())
            return Poll(fuel);
          if (!root_record[0]) {
            ++position;
            stage = Edges;
            break;
          }
          if (root_record[0] != static_cast<std::int64_t>(position + 1))
            return Poll(invalid(
                "union traversal encountered an already processed vertex"));
          root_id = position + 1;
          neighbour = 0;
          stage = Neighbour;
          break;
        }
        case Neighbour: {
          if (neighbour == 2) {
            ++position;
            stage = Edges;
            break;
          }
          const auto side = neighbour++;
          if ((side == 0 && position % spec.width == 0) ||
              (side == 1 && position < spec.width))
            break;
          find_id = position - (side ? spec.width : 1) + 1;
          hops = 0;
          emit_find = false;
          stage = Find;
          break;
        }
        case Find:
          if (!find_id || find_id > n || ++hops > 64)
            return Poll(invalid("invalid bounded union parent path"));
          stage = Found;
          return Poll(ResultProgramNeed{
              {},
              {},
              {ResultReadTemporary{tree, (find_id - 1) * 32, 32}}});
        case Found: {
          found_record = record(p);
          auto fuel = p.consume_work(1);
          if (!fuel.ok())
            return Poll(fuel);
          if (!found_record[0]) {
            find_id = 0;
          } else {
            if (found_record[0] < 0 ||
                static_cast<std::uint64_t>(found_record[0]) > n ||
                found_record[1] < 0 || found_record[1] > 63 ||
                found_record[2] < 0 ||
                static_cast<std::uint64_t>(found_record[2]) >= n ||
                found_record[3] <= 0)
              return Poll(invalid("invalid private union record"));
            if (static_cast<std::uint64_t>(found_record[0]) != find_id) {
              find_id = static_cast<std::uint64_t>(found_record[0]);
              stage = Find;
              break;
            }
          }
          if (emit_find) {
            stage = EmitReady;
            break;
          }
          if (!find_id || find_id == root_id) {
            stage = Neighbour;
            break;
          }
          if (root_record[1] < found_record[1] ||
              (root_record[1] == found_record[1] && root_id > find_id)) {
            std::swap(root_record, found_record);
            std::swap(root_id, find_id);
          }
          auto union_work = p.consume_work(8);
          if (!union_work.ok())
            return Poll(union_work);
          if (root_record[3] > INT64_MAX - found_record[3])
            return Poll(capacity("component area overflow"));
          if (root_record[1] == found_record[1])
            ++root_record[1];
          root_record[2] = std::min(root_record[2], found_record[2]);
          root_record[3] += found_record[3];
          found_record[0] = static_cast<std::int64_t>(root_id);
          {
            auto winner = write_record(p, root_id, root_record),
                 loser = write_record(p, find_id, found_record);
            if (!winner.ok() || !loser.ok())
              return Poll(!winner.ok() ? winner.status() : loser.status());
            stage = UnionWritten;
            return Poll(
                ResultProgramNeed{{},
                                  {},
                                  {winner.take_value(), loser.take_value()}});
          }
        }
        case UnionWritten:
          stage = Neighbour;
          break;
        case Emit:
          if (position == n) {
            tree = {};
            stage = Finish;
            break;
          }
          find_id = position + 1;
          hops = 0;
          emit_find = true;
          stage = Find;
          break;
        case EmitReady:
          return emit(p);
        case EmitWritten:
          stage = Emit;
          break;
        case Area: {
          if (position == count) {
            stage = Finish;
            break;
          }
          batch = std::min(count - position, window(p) / 24);
          if (!batch)
            return Poll(capacity("component table record exceeds page window"));
          auto read = input[0].prepare_read(descriptors[0], 1, position, batch);
          if (!read.ok())
            return Poll(read.status());
          stage = AreaReady;
          return Poll(ResultProgramNeed{{}, {}, {read.take_value()}});
        }
        case AreaReady: {
          auto memory = p.allocator.allocate(batch * 16);
          if (!memory.ok())
            return Poll(memory.status());
          auto buffer = memory.take_value();
          auto fuel = p.consume_work(batch);
          if (!fuel.ok())
            return Poll(fuel);
          const auto& page =
              std::get<std::shared_ptr<const CpuStorage>>(p.io.at(0));
          for (std::uint64_t i = 0; i < batch; ++i)
            std::memcpy(buffer.data() + i * 16, page->bytes().data() + i * 24,
                        16);
          auto write =
              builder.prepare_append(0, batch, std::move(buffer).freeze());
          if (!write.ok())
            return Poll(write.status());
          position += batch;
          stage = AreaWritten;
          return Poll(ResultProgramNeed{{}, {}, {write.take_value()}});
        }
        case AreaWritten:
          stage = Area;
          break;
        case Verify: {
          if (position == count) {
            position = 0;
            stage = Filter;
            break;
          }
          batch = std::min(count - position, window(p) / 24);
          if (!batch)
            return Poll(capacity("component table record exceeds page window"));
          auto original =
                   input[0].prepare_read(descriptors[0], 1, position, batch),
               index =
                   input[1].prepare_read(descriptors[1], 0, position, batch);
          if (!original.ok() || !index.ok())
            return Poll(!original.ok() ? original.status() : index.status());
          stage = VerifyReady;
          return Poll(
              ResultProgramNeed{{},
                                {},
                                {original.take_value(), index.take_value()}});
        }
        case VerifyReady: {
          const auto& original =
              std::get<std::shared_ptr<const CpuStorage>>(p.io.at(0));
          const auto& index =
              std::get<std::shared_ptr<const CpuStorage>>(p.io.at(1));
          auto fuel = p.consume_work(batch);
          if (!fuel.ok())
            return Poll(fuel);
          for (std::uint64_t i = 0; i < batch; ++i)
            if (std::memcmp(original->bytes().data() + i * 24,
                            index->bytes().data() + i * 16, 16) != 0)
              return Poll(bad_index(
                  "area index differs from associated component table"));
          position += batch;
          stage = Verify;
          break;
        }
        case Filter: {
          if (position == n) {
            label_page.reset();
            index_page.reset();
            stage = Finish;
            break;
          }
          batch = std::min(n - position, window(p) / 8);
          auto read = input[0].prepare_read(descriptors[0], 0, position, batch);
          if (!read.ok())
            return Poll(read.status());
          stage = LabelReady;
          return Poll(ResultProgramNeed{{}, {}, {read.take_value()}});
        }
        case LabelReady: {
          label_page = std::get<std::shared_ptr<const CpuStorage>>(p.io.at(0));
          auto memory = p.allocator.allocate(batch);
          if (!memory.ok())
            return Poll(memory.status());
          labels = memory.take_value();
          label_fill = 0;
          low = 0;
          high = count;
          current_label = -1;
          stage = Search;
          break;
        }
        case Search: {
          if (label_fill == batch) {
            auto write =
                builder.prepare_append(0, batch, std::move(labels).freeze());
            if (!write.ok())
              return Poll(write.status());
            label_page.reset();
            position += batch;
            stage = FilterWritten;
            return Poll(ResultProgramNeed{{}, {}, {write.take_value()}});
          }
          if (current_label < 0) {
            std::memcpy(&current_label,
                        label_page->bytes().data() + label_fill * 8, 8);
            if (current_label < 0)
              return Poll(invalid("negative label in area filter"));
            low = 0;
            high = count;
          }
          if (!current_label) {
            auto fuel = p.consume_work(1);
            if (!fuel.ok())
              return Poll(fuel);
            labels.data()[label_fill++] = 0;
            current_label = -1;
            break;
          }
          const auto middle = low + (high - low) / 2;
          if (middle == count)
            return Poll(bad_index("nonzero label has no associated area"));
          if (!index_page || middle < index_first ||
              middle - index_first >= index_rows) {
            const auto page_rows = window(p) / 16;
            index_first = (middle / page_rows) * page_rows;
            index_rows = std::min(count - index_first, page_rows);
            auto read = input[1].prepare_read(descriptors[1], 0, index_first,
                                              index_rows);
            if (!read.ok())
              return Poll(read.status());
            index_page.reset();
            stage = IndexReady;
            return Poll(ResultProgramNeed{{}, {}, {read.take_value()}});
          }
          std::array<std::int64_t, 2> property;
          std::memcpy(property.data(),
                      index_page->bytes().data() + (middle - index_first) * 16,
                      16);
          auto fuel = p.consume_work(1);
          if (!fuel.ok())
            return Poll(fuel);
          if (low < high) {
            if (property[0] < current_label)
              low = middle + 1;
            else
              high = middle;
            break;
          }
          if (property[0] != current_label || property[1] <= 0)
            return Poll(bad_index("nonzero label has no associated area"));
          labels.data()[label_fill++] = property[1] >= threshold ? 1 : 0;
          current_label = -1;
          break;
        }
        case IndexReady:
          index_page = std::get<std::shared_ptr<const CpuStorage>>(p.io.at(0));
          stage = Search;
          break;
        case FilterWritten:
          stage = Filter;
          break;
        case Finish:
          return finish(p);
      }
  }
};
}  // namespace
Result<SchemaTemplate> component_area_schema(const ComponentsSpec& spec) {
  return derived_schema(spec, false);
}
Result<SchemaTemplate> component_filter_schema(const ComponentsSpec& spec) {
  return derived_schema(spec, true);
}
Result<OperationDefinition> make_component_operation(
    Op operation, const ComponentsSpec& spec) {
  auto checked = profile(spec);
  if (!checked.ok())
    return Result<OperationDefinition>(checked);
  const auto number = static_cast<unsigned>(operation);
  if (!number || number > 3)
    return Result<OperationDefinition>(
        invalid("unknown components4 operation"));
  OperationDefinition definition;
  const char* names[] = {"", "components4.labels", "components4.area",
                         "components4.filter"};
  definition.key = names[number];
  auto& traits = definition.traits;
  traits.input_count = operation == Op::Filter ? 2 : 1;
  traits.input_schema.resize(traits.input_count);
  traits.workspace_bytes = 4096;
  auto& out = traits.outputs[0];
  out.region_rule = OperationRegionRule::Dependency;
  out.dependency_version = 2;
  out.continuation_bytes = sizeof(State);
  out.maximum_dependency_stages = 1000000;
  auto schema = operation == Op::Labels ? components_schema(spec)
                : operation == Op::Area ? component_area_schema(spec)
                                        : component_filter_schema(spec);
  if (!schema.ok())
    return Result<OperationDefinition>(schema.status());
  out.result_schema = schema.take_value();
  out.output_schema.kind = OperationPortKind::Result;
  out.output_schema.result_schema_id = std::string(out.result_schema->id);
  out.output_schema.result_schema_version = 1;
  if (operation != Op::Labels) {
    for (unsigned i = 0; i < traits.input_count; ++i) {
      traits.input_schema[i].kind = OperationPortKind::Result;
      traits.input_schema[i].result_schema_id =
          i ? "photospider.component_area_index" : "photospider.components";
      traits.input_schema[i].result_schema_version = 1;
    }
  }
  if (operation == Op::Filter)
    traits.parameter_schema.push_back(
        {"minimum_area", OperationParameterType::Int64, true, false, 0, 0});
  definition.validate_dependency = [operation, spec](const auto& metadata,
                                                     const auto& parameters) {
    return inputs(operation, spec, metadata, parameters);
  };
  definition.start_result = [operation, spec](
                                const ResultProgramQuery& query,
                                const BufferAllocator& allocator) {
    auto status = inputs(operation, spec, query.inputs, query.parameters);
    if (!status.ok())
      return Result<ResultContinuation>(status);
    return ResultContinuation::make<State>(allocator, operation, spec);
  };
  return Result<OperationDefinition>(std::move(definition));
}
}  // namespace ps
