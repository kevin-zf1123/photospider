#include <array>
#include <atomic>
#include <cfenv>  // NOLINT(build/c++11)
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

#define STAGE_CHECK(expression)                                        \
  do {                                                                 \
    if (!::ps::test::check(static_cast<bool>(expression), #expression, \
                           __FILE__, __LINE__))                        \
      throw std::runtime_error("CPU stage assertion failed");          \
  } while (false)

namespace {
using namespace ps;  // NOLINT(build/namespaces)
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
using Exercise = std::function<void(const PlanarOperationInvocation&)>;
std::shared_ptr<OperationRegistry> registry(const Exercise& exercise) {
  auto result = std::make_shared<OperationRegistry>();
  OperationDefinition op;
  op.key = "test.cpu_stages";
  op.traits.planar_storage_capable = true;
  op.traits.cpu_staged_tiles = true;
  op.traits.input_count = 1;
  op.traits.input_schema.resize(1);
  op.traits.workspace_bytes = 4096;
  auto& out = op.traits.outputs[0];
  out.key = "values";
  out.output_element_type = ElementType::Float32;
  out.shape_rule = OperationShapeRule::PreserveFirstInput;
  out.region_rule = OperationRegionRule::Whole;
  out.planar_layout = PlanarImageLayout{};
  op.planar_callback = [exercise](const PlanarOperationInvocation& call) {
    exercise(call);
    auto row = call.output.row_run({0, 0, 0});
    if (!row.ok())
      return row.status();
    const float value = 1;
    std::memcpy(row.value().data, &value, sizeof(value));
    // Ignoring a service failure here still must prevent publication.
    return Status::success();
  };
  STAGE_CHECK(result->register_operation(std::move(op)).ok());
  STAGE_CHECK(result->freeze().ok());
  return result;
}
WorkflowDocument document() {
  WorkflowDocument result;
  const ValueDescriptor descriptor{ElementType::Float32, {1, 1, 1}};
  result.inputs = {{1,
                    "image",
                    descriptor,
                    Region::whole(descriptor.shape),
                    {},
                    {},
                    PlanarImageLayout{}}};
  result.nodes = {{1, "test.cpu_stages", {WorkflowInputReference{1}}, {}}};
  result.outputs = {{"result", 1, "values"}};
  return result;
}
ExecutionBindings bindings() {
  auto image = take(PlanarImage::create({ElementType::Float32, {1, 1, 1}}, {}));
  const float value = .25f;
  STAGE_CHECK(image
                  .publish(Region::whole({1, 1, 1}),
                           reinterpret_cast<const uint8_t*>(&value), 4)
                  .ok());
  return {{{"image", std::make_shared<const PlanarImage>(std::move(image))}}};
}
struct Geometry {
  std::thread::id caller;
  std::array<std::atomic<unsigned>, 17 * 5 * 3> visits{};
  std::atomic<unsigned> entered{0};
  std::array<std::atomic<bool>, 64> slots{};
  unsigned grant = 1;
  unsigned total = 0;
};
int visit(void* raw, const ps_cpu_tile_v1* tile) {
  auto& work = *static_cast<Geometry*>(raw);
  if (std::this_thread::get_id() == work.caller || tile->slot >= work.grant ||
      fegetround() != FE_TONEAREST ||
      tile->index != tile->begin[0] / 4 +
                         5 * (tile->begin[1] / 2 + 3 * (tile->begin[2] / 2)))
    return 6;
  if (work.slots[tile->slot].exchange(true))
    return 6;
  for (auto z = tile->begin[2]; z < tile->end[2]; ++z)
    for (auto y = tile->begin[1]; y < tile->end[1]; ++y)
      for (auto x = tile->begin[0]; x < tile->end[0]; ++x) {
        if (x >= 17 || y >= 5 || z >= 3 ||
            work.visits[x + 17 * (y + 5 * z)].fetch_add(1) != 0)
          return 6;
      }
  work.slots[tile->slot].store(false);
  fesetround(FE_DOWNWARD);
  return 0;
}
int merge(void* raw, const ps_cpu_tile_v1*) {
  auto& work = *static_cast<Geometry*>(raw);
  if (std::this_thread::get_id() == work.caller || fegetround() != FE_TONEAREST)
    return 6;
  for (const auto& count : work.visits)
    work.total += count.load();
  return work.total == 255 ? 0 : 1;
}
int occupy(void* raw, const ps_cpu_tile_v1* tile) {
  auto& work = *static_cast<Geometry*>(raw);
  if (tile->slot >= work.grant || work.slots[tile->slot].exchange(true))
    return 6;
  ++work.entered;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (work.entered.load() < work.grant) {
    if (std::chrono::steady_clock::now() > deadline)
      return 1;
    std::this_thread::yield();
  }
  work.slots[tile->slot].store(false);
  return 0;
}
void geometry() {
  for (unsigned workers : {1, 4, 64}) {
    for (unsigned limit : {0, 1}) {
      auto operations = registry([&](const PlanarOperationInvocation& call) {
        STAGE_CHECK(call.cpu_tiles && !call.cpu_parallel && !call.gpu);
        const auto& s = *call.cpu_tiles;
        Geometry work;
        work.caller = std::this_thread::get_id();
        work.grant = limit ? limit : workers;
        STAGE_CHECK(s.maximum_workers == work.grant);
        const ps_cpu_tile_stage_v1 stage{sizeof(stage),
                                         {17, 5, 3},
                                         {4, 2, 2},
                                         0};
        STAGE_CHECK(s.run(s.context, &stage, visit, &work) == 0);
        const ps_cpu_tile_stage_v1 one{sizeof(one), {1, 1, 1}, {1, 1, 1}, 1};
        STAGE_CHECK(s.run(s.context, &one, merge, &work) == 0);
        STAGE_CHECK(work.total == 255);
        const ps_cpu_tile_stage_v1 full{sizeof(full),
                                        {work.grant, 1, 1},
                                        {1, 1, 1},
                                        0};
        STAGE_CHECK(s.run(s.context, &full, occupy, &work) == 0);
      });
      ExecutionContextConfig config;
      config.cpu_workers = workers;
      config.managed_resources = ResourceLimits{};
      ExecutionContext execution(operations, config);
      GraphContext graph(document());
      auto plan = take(Compiler(operations).compile(graph));
      ExecutionOptions options;
      options.maximum_parallelism = limit;
      auto result = execution.execute(plan.plan, bindings(), {}, options);
      STAGE_CHECK(result.ok());
      STAGE_CHECK(result.value().diagnostics.cpu_stage_count == 3);
      STAGE_CHECK(result.value().diagnostics.cpu_tile_callback_count ==
                  31 + (limit ? limit : workers));
      STAGE_CHECK(result.value().diagnostics.peak_active_tasks ==
                  (limit ? limit : workers));
    }
  }
}
void failures() {
  for (unsigned test = 0; test < 9; ++test) {
    auto operations = registry([&](const PlanarOperationInvocation& call) {
      const auto& s = *call.cpu_tiles;
      ps_cpu_tile_stage_v1 stage{sizeof(stage), {8, 1, 1}, {1, 1, 1}, 0};
      if (test == 0)
        stage.tile[1] = 0;
      if (test == 1) {
        stage.extent[0] = UINT64_MAX;
        stage.extent[1] = 2;
      }
      if (test == 2)
        stage.workers = s.maximum_workers + 1;
      const auto body = [](void* raw, const ps_cpu_tile_v1*) -> int {
        auto& call =
            *static_cast<std::pair<unsigned, const ps_cpu_tiles_service_v1*>*>(
                raw);
        if (call.first == 3)
          return 4;
        if (call.first == 4)
          throw std::bad_alloc();
        if (call.first == 5)
          throw std::runtime_error("stage failure");
        if (call.first == 6) {
          ps_cpu_tile_stage_v1 nested{sizeof(nested), {1, 1, 1}, {1, 1, 1}, 0};
          (void)call.second->run(
              call.second->context, &nested,
              [](void*, const ps_cpu_tile_v1*) { return 0; }, nullptr);
        }
        if (call.first == 7)
          return 3;
        if (call.first == 8)
          return 5;
        return 0;
      };
      std::pair<unsigned, const ps_cpu_tiles_service_v1*> work{test, &s};
      const auto code = s.run(s.context, &stage, body, &work);
      STAGE_CHECK(code != 0);
      unsigned later = 0;
      const ps_cpu_tile_stage_v1 next{sizeof(next), {1, 1, 1}, {1, 1, 1}, 0};
      const auto repeated = s.run(
          s.context, &next,
          [](void* raw, const ps_cpu_tile_v1*) {
            ++*static_cast<unsigned*>(raw);
            return 0;
          },
          &later);
      STAGE_CHECK(repeated == code && later == 0);
    });
    ExecutionContextConfig config;
    config.cpu_workers = 2;
    config.managed_resources = ResourceLimits{};
    ExecutionContext execution(operations, config);
    GraphContext graph(document());
    auto plan = take(Compiler(operations).compile(graph));
    const auto result = execution.execute(plan.plan, bindings());
    STAGE_CHECK(!result.ok());
    const auto expected = test == 1 || test == 3 || test == 4
                              ? ErrorCode::ResourceExhausted
                          : test == 5 ? ErrorCode::OperationFailed
                          : test == 7 ? ErrorCode::BackendUnavailable
                          : test == 8 ? ErrorCode::TypeMismatch
                                      : ErrorCode::InvalidArgument;
    STAGE_CHECK(result.status().code == expected);
    STAGE_CHECK(execution.resource_budget()
                    .value()
                    .statistics()
                    .live[ResourceKind::Payload] == 0);
    STAGE_CHECK(execution.resource_budget()
                    .value()
                    .statistics()
                    .live[ResourceKind::Queue] == 0);
  }
}
template <class Predicate>
bool wait_until(Predicate ready) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!ready()) {
    if (std::chrono::steady_clock::now() > deadline)
      return false;
    std::this_thread::yield();
  }
  return true;
}
void pending(bool managed, bool cancel) {
  std::atomic<unsigned> attempts{0};
  std::atomic<bool> entered{false}, release{false};
  struct Work {
    bool wait;
    std::atomic<bool>& entered;
    std::atomic<bool>& release;
  };
  auto operations = registry([&](const PlanarOperationInvocation& call) {
    Work work{attempts.fetch_add(1) == 0, entered, release};
    const ps_cpu_tile_stage_v1 stage{sizeof(stage), {1, 1, 1}, {1, 1, 1}, 1};
    (void)call.cpu_tiles->run(
        call.cpu_tiles->context, &stage,
        [](void* raw, const ps_cpu_tile_v1*) {
          auto& work = *static_cast<Work*>(raw);
          if (work.wait) {
            work.entered.store(true);
            if (!wait_until([&] { return work.release.load(); }))
              return 1;
          }
          return 0;
        },
        &work);
  });
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_queued_tasks = cancel ? 2 : 1;
  if (managed)
    config.managed_resources = ResourceLimits{};
  ExecutionContext execution(operations, config);
  GraphContext graph(document());
  auto plan = take(Compiler(operations).compile(graph));
  const auto input = bindings();
  CancellationSource stop;
  std::future<Result<ExecutionResult>> first, second;
  struct Drain {
    std::atomic<bool>& release;
    CancellationSource& stop;
    ~Drain() {
      release.store(true);
      stop.cancel();
    }
  } drain{release, stop};
  first = std::async(std::launch::async,
                     [&] { return execution.execute(plan.plan, input); });
  STAGE_CHECK(wait_until([&] { return entered.load(); }));
  if (cancel) {
    second = std::async(std::launch::async, [&] {
      return execution.execute(plan.plan, input, stop.token());
    });
    // One active stage and one unclaimed stage hold their queue leases.
    STAGE_CHECK(wait_until([&] {
      return attempts.load() == 2 && execution.resource_budget()
                                             .value()
                                             .statistics()
                                             .live[ResourceKind::Queue] == 2;
    }));
    stop.cancel();
    STAGE_CHECK(second.wait_for(std::chrono::seconds(2)) ==
                std::future_status::ready);
    STAGE_CHECK(second.get().status().code == ErrorCode::Cancelled);
  } else {
    auto rejected = execution.execute(plan.plan, input);
    STAGE_CHECK(rejected.status().code == ErrorCode::ResourceExhausted);
  }
  release.store(true);
  STAGE_CHECK(first.get().ok());
  STAGE_CHECK(execution.execute(plan.plan, input).ok());
  if (managed) {
    const auto stats = execution.resource_budget().value().statistics();
    STAGE_CHECK(stats.live[ResourceKind::Queue] == 0);
    STAGE_CHECK(stats.live[ResourceKind::Payload] == 0);
  }
}
void stop_active(bool stale) {
  std::atomic<unsigned> entered{0};
  std::atomic<bool> release{false}, returned{false};
  struct Work {
    std::atomic<unsigned>& entered;
    std::atomic<bool>& release;
  };
  auto operations = registry([&](const PlanarOperationInvocation& call) {
    Work work{entered, release};
    const ps_cpu_tile_stage_v1 stage{sizeof(stage), {16, 1, 1}, {1, 1, 1}, 1};
    (void)call.cpu_tiles->run(
        call.cpu_tiles->context, &stage,
        [](void* raw, const ps_cpu_tile_v1*) {
          auto& work = *static_cast<Work*>(raw);
          ++work.entered;
          return wait_until([&] { return work.release.load(); }) ? 0 : 1;
        },
        &work);
    returned.store(true);
  });
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ResourceLimits{};
  ExecutionContext execution(operations, config);
  GraphContext graph(document());
  auto plan = take(Compiler(operations).compile(graph));
  const auto input = bindings();
  CancellationSource stop;
  std::future<Result<ExecutionResult>> result;
  struct Drain {
    std::atomic<bool>& release;
    ~Drain() { release.store(true); }
  } drain{release};
  result = std::async(std::launch::async, [&] {
    return execution.execute(plan.plan, input, stop.token());
  });
  STAGE_CHECK(wait_until([&] { return entered.load() == 1; }));
  if (stale)
    graph.replace(document());
  else
    stop.cancel();
  STAGE_CHECK(result.wait_for(std::chrono::milliseconds(20)) ==
              std::future_status::timeout);
  STAGE_CHECK(!returned.load());
  STAGE_CHECK(execution.resource_budget()
                  .value()
                  .statistics()
                  .live[ResourceKind::Queue] == 1);
  release.store(true);
  STAGE_CHECK(result.get().status().code ==
              (stale ? ErrorCode::Stale : ErrorCode::Cancelled));
  STAGE_CHECK(entered.load() == 1);
  STAGE_CHECK(returned.load());
  const auto stats = execution.resource_budget().value().statistics();
  STAGE_CHECK(stats.live[ResourceKind::Queue] == 0);
  STAGE_CHECK(stats.live[ResourceKind::Payload] == 0);
}
}  // namespace
int main() {
  geometry();
  failures();
  pending(false, false);
  pending(true, false);
  pending(true, true);
  stop_active(false);
  stop_active(true);
  return 0;
}
