#include "kernel/scheduler/scheduler_plugin_api.hpp"

#include <atomic>
#include <future>
#include <string>

namespace {

std::atomic<int> g_active_count{0};
std::atomic<int> g_destroy_count{0};

class DestroyCountScheduler final : public ps::IScheduler {
 public:
  DestroyCountScheduler() { g_active_count.fetch_add(1); }
  ~DestroyCountScheduler() override { g_active_count.fetch_sub(1); }

  void attach(ps::GraphRuntime*) override {}
  void detach() override {}
  void start() override { running_ = true; }
  void shutdown() override { running_ = false; }

  std::future<ps::NodeOutput> schedule(const ps::ComputeOptions&) override {
    std::promise<ps::NodeOutput> promise;
    promise.set_value(ps::NodeOutput{});
    return promise.get_future();
  }

  std::string name() const override { return "destroy_count_test"; }
  std::string get_stats() const override { return "destroy-count-test"; }
  bool is_running() const override { return running_; }

 private:
  bool running_ = false;
};

}  // namespace

extern "C" {

int ps_scheduler_plugin_get_count() {
  return 1;
}

const char* ps_scheduler_plugin_get_name(int index) {
  return index == 0 ? "destroy_count_test" : nullptr;
}

const char* ps_scheduler_plugin_get_description(int index) {
  return index == 0 ? "Destroy-count scheduler lifecycle test" : nullptr;
}

ps::IScheduler* ps_scheduler_plugin_create(const char* type_name,
                                           unsigned int) {
  if (!type_name || std::string(type_name) != "destroy_count_test") {
    return nullptr;
  }
  return new DestroyCountScheduler();
}

void ps_scheduler_plugin_destroy(ps::IScheduler* scheduler) {
  g_destroy_count.fetch_add(1);
  delete scheduler;
}

const char* ps_scheduler_plugin_get_version() {
  return "test";
}

int ps_test_scheduler_active_count() {
  return g_active_count.load();
}

int ps_test_scheduler_destroy_count() {
  return g_destroy_count.load();
}

void ps_test_scheduler_reset_counts() {
  g_active_count.store(0);
  g_destroy_count.store(0);
}

}
