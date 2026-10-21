#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "execution/block_memory.hpp"
#include "execution/cpu_workers.hpp"
#include "execution/task_schedule.hpp"
#include "support/physical_program_fixture.hpp"
#include "support/status_check.hpp"

namespace {
using namespace ps::physical;            // NOLINT(build/namespaces)
using namespace ps::execution_internal;  // NOLINT(build/namespaces)

struct Fixture {
  Fixture(const Program& p, BlockMemory storage)
      : program(p), memory(std::move(storage)) {
    for (unsigned i = 0; i < tiles.size(); ++i)
      tiles[i] = memory.tile(i).data();
  }
  const Program& program;
  BlockMemory memory;
  std::array<std::uint8_t*, 5> tiles;
  std::array<std::shared_ptr<const ps::CpuStorage>, 5> completed_tiles;
  std::mutex mutex;
  std::condition_variable changed;
  std::array<Id, 5> completions = {};
  std::size_t completed = 0;
  bool release_old = false, old_started = false;
  struct Work {
    Fixture* fixture = nullptr;
    Submission submission;
  };
  std::array<Work, 5> work;

  static void invoke(void* context) noexcept {
    auto& item = *static_cast<Work*>(context);
    auto& self = *item.fixture;
    const auto ti = item.submission.task;
    if (ti == 0) {
      std::unique_lock<std::mutex> lock(self.mutex);
      self.old_started = true;
      self.changed.notify_all();
      self.changed.wait(lock, [&] { return self.release_old; });
    }
    const auto& task = self.program.tasks[ti];
    std::uint8_t value = 1;
    for (const auto& read : task.reads)
      value += self.tiles[read.tile][0];
    for (auto tile : task.writes)
      for (unsigned byte = 0; byte < 4; ++byte)
        self.tiles[tile][byte] = value;
  }
  static void complete(void* context) noexcept {
    auto& item = *static_cast<Work*>(context);
    auto& self = *item.fixture;
    {
      std::lock_guard<std::mutex> lock(self.mutex);
      self.completions[self.completed++] = item.submission.task;
    }
    self.changed.notify_all();
  }
  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      release_old = true;
    }
    changed.notify_all();
  }
};
struct ReleaseOnExit {
  Fixture& fixture;
  ~ReleaseOnExit() { fixture.release(); }
};

struct CounterWork {
  std::mutex mutex;
  std::condition_variable changed;
  bool started = false, release = false;
  std::atomic<unsigned> invoked{0}, completed{0};
  static void invoke(void* context) noexcept {
    auto& self = *static_cast<CounterWork*>(context);
    ++self.invoked;
    std::unique_lock<std::mutex> lock(self.mutex);
    self.started = true;
    self.changed.notify_all();
    self.changed.wait(lock, [&] { return self.release; });
  }
  static void complete(void* context) noexcept {
    ++static_cast<CounterWork*>(context)->completed;
  }
};
}  // namespace

int main() {
  using namespace ps::physical;            // NOLINT(build/namespaces)
  using namespace ps::execution_internal;  // NOLINT(build/namespaces)
  auto p = ps::test::physical_fixture::diamond();
  PS_REQUIRE_OK(finalize_program(&p));
  ps::ResourceBudget budget;
  auto memory = BlockMemory::allocate(p, 0, 2, budget);
  PS_REQUIRE_OK(memory);
  Fixture fixture(p, memory.take_value());
  CpuWorkers pool(2, 2);
  ReleaseOnExit release_on_exit{fixture};
  TaskSchedule schedule(p, 0, 1, 1);
  std::vector<Id> retired;
  retired.reserve(5);
  std::size_t consumed = 0;
  while (!schedule.drained()) {
    while (schedule.inflight() < 2) {
      auto submission = schedule.issue_next(Backend::Cpu);
      if (!submission)
        break;
      auto& work = fixture.work[submission->task];
      work = {&fixture, *submission};
      PS_CHECK(pool.submit({&work, Fixture::invoke, Fixture::complete}) ==
               CpuSubmission::Accepted);
    }
    Id finished = none;
    {
      std::unique_lock<std::mutex> lock(fixture.mutex);
      const auto observed = fixture.changed.wait_for(
          lock, std::chrono::seconds(5),
          [&] { return fixture.old_started && fixture.completed > consumed; });
      PS_CHECK(observed);
      finished = fixture.completions[consumed++];
    }
    fixture.completed_tiles[finished] = fixture.memory.publish(finished);
    PS_REQUIRE_OK(schedule.complete(fixture.work[finished].submission.key, {}));
    if (finished == 3) {
      // D consumed B on a real second worker while the oldest call was blocked.
      PS_CHECK(schedule.state(0) == TaskState::Submitted);
      PS_CHECK(fixture.tiles[3][0] == 2);
      PS_CHECK(schedule.retirement_head() == 0);
      fixture.release();
    }
    while (auto record = schedule.retire_next())
      retired.push_back(record->call);
  }
  pool.drain();
  PS_REQUIRE_OK(schedule.settled_status());
  PS_CHECK(pool.peak_active() == 2);
  PS_CHECK(retired == std::vector<Id>({0, 1, 2, 3, 4}));
  PS_CHECK(fixture.completions == (std::array<Id, 5>{2, 3, 0, 1, 4}));
  for (unsigned byte = 0; byte < 4; ++byte)
    PS_CHECK(fixture.tiles[4][byte] == 5);
  for (unsigned i = 0; i < 5; ++i)
    for (unsigned j = 0; j < i; ++j)
      PS_CHECK(fixture.tiles[i] != fixture.tiles[j]);

  // Accepted queue entries drain on destruction. Busy rejection owns nothing
  // and produces no callback. The producer keeps all three work descriptors.
  CounterWork counter;
  auto workers = std::make_unique<CpuWorkers>(1, 1);
  CpuWork work{&counter, CounterWork::invoke, CounterWork::complete};
  PS_CHECK(workers->submit(work) == CpuSubmission::Accepted);
  bool started;
  {
    std::unique_lock<std::mutex> lock(counter.mutex);
    started = counter.changed.wait_for(lock, std::chrono::seconds(5),
                                       [&] { return counter.started; });
    if (!started)
      counter.release = true;
  }
  counter.changed.notify_all();
  if (!started) {
    workers.reset();
    PS_CHECK(started);
  }
  auto queued = workers->submit(work);
  auto rejected = workers->submit(work);
  auto invalid = workers->submit({});
  {
    std::lock_guard<std::mutex> lock(counter.mutex);
    counter.release = true;
  }
  counter.changed.notify_all();
  workers.reset();
  PS_CHECK(queued == CpuSubmission::Accepted);
  PS_CHECK(rejected == CpuSubmission::Busy);
  PS_CHECK(invalid == CpuSubmission::Invalid);
  PS_CHECK(counter.invoked == 2 && counter.completed == 2);
  return 0;
}
