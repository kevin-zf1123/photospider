#include <cstdint>
#include <future>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include "execution/dependency_dirty.hpp"
#include "photospider/data/dependency.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
Footprint point(std::uint64_t p) {
  return Footprint::from_regions({2}, {Region({{p, 1}})}).take_value();
}
int late_delta() {
  execution_internal::DirtyDeltaQueue queue;
  PS_CHECK(queue.receive(1, point(0)).ok());
  PS_CHECK(queue.receive(1, point(0)).ok());
  PS_CHECK(queue.take().value()->changed == point(0));
  PS_CHECK(!queue.take().value());
  PS_CHECK(queue.receive(1, point(1)).ok());
  auto next = queue.take();
  PS_CHECK(next.ok() && next.value()->changed == point(1));
  PS_CHECK(queue.accumulated(1).value() == Footprint::all({2}).value());
  PS_CHECK(!queue.accumulated(2).ok());
  auto a = std::async(std::launch::async,
                      [&] { return queue.receive(2, point(0)); });
  auto b = std::async(std::launch::async,
                      [&] { return queue.receive(2, point(1)); });
  PS_CHECK(a.get().ok() && b.get().ok());
  PS_CHECK(queue.take().value()->changed == Footprint::all({2}).value());
  PS_CHECK(!queue.take().value());
  return 0;
}
int failures_and_races() {
  execution_internal::DirtyDeltaQueue mismatch;
  PS_CHECK(mismatch.receive(1, point(0)).ok());
  const auto failure = mismatch.receive(1, Footprint::all({3}).take_value());
  PS_CHECK(failure.code == ErrorCode::InvalidArgument);
  PS_CHECK(mismatch.take().status().code == failure.code);
  PS_CHECK(mismatch.receive(2, point(0)).code == failure.code);
  PS_CHECK(mismatch.fail(Status::failure(ErrorCode::OperationFailed, "later"))
               .code == failure.code);
  FootprintLimits limits;
  limits.maximum_boxes = 1;
  execution_internal::DirtyDeltaQueue bounded(limits);
  PS_CHECK(bounded.receive(1, point(0)).ok());
  PS_CHECK(bounded.receive(2, point(0)).code == ErrorCode::ResourceExhausted);
  PS_CHECK(bounded.take().status().code == ErrorCode::ResourceExhausted);
  for (int i = 0; i < 100; ++i) {
    execution_internal::DirtyDeltaQueue queue;
    PS_CHECK(queue.receive(1, point(0)).ok());
    auto consumer =
        std::async(std::launch::async, [&] { return queue.take(); });
    auto producer = std::async(std::launch::async,
                               [&] { return queue.receive(1, point(1)); });
    auto first = consumer.get();
    PS_CHECK(producer.get().ok() && first.ok() && first.value());
    auto delivered = first.value()->changed;
    auto second = queue.take();
    PS_CHECK(second.ok());
    if (second.value()) {
      PS_CHECK(delivered.intersect(second.value()->changed).value().empty());
      delivered = delivered.unite(second.value()->changed).take_value();
    }
    PS_CHECK(delivered == Footprint::all({2}).value());
    PS_CHECK(!queue.take().value());
  }
  return 0;
}
int random_graphs() {
  std::mt19937 random(20260911);
  for (unsigned trial = 0; trial < 300; ++trial) {
    // Literal edges are a separate atomic reachability oracle.
    std::set<std::pair<unsigned, unsigned>> edges;
    for (unsigned consumer = 1; consumer < 7; ++consumer)
      for (unsigned source = 0; source < consumer; ++source)
        for (unsigned out = 0; out < 2; ++out)
          for (unsigned in = 0; in < 2; ++in)
            if (random() % 5 == 0)
              edges.emplace(source * 2 + in, consumer * 2 + out);
    struct Subscription {
      unsigned source, consumer;
      DependencyCertificate certificate;
    };
    std::vector<Subscription> subscriptions;
    for (unsigned consumer = 1; consumer < 7; ++consumer)
      for (unsigned source = 0; source < consumer; ++source) {
        std::vector<AtomCertificate> rows;
        for (unsigned out = 0; out < 2; ++out) {
          auto inputs = Footprint::none({2}).take_value();
          for (unsigned in = 0; in < 2; ++in)
            if (edges.count({source * 2 + in, consumer * 2 + out}))
              inputs = inputs.unite(point(in)).take_value();
          rows.push_back({{out}, {{0, 1, inputs, {}}}});
        }
        subscriptions.push_back(
            {source, consumer,
             DependencyCertificate::create("fixed generation",
                                           Footprint::all({2}).take_value(),
                                           {{2}}, rows)
                 .take_value()});
      }
    execution_internal::DirtyDeltaQueue queue;
    std::set<unsigned> expected;
    for (unsigned wave = 0; wave < 2; ++wave) {
      const auto seed = static_cast<unsigned>(random() % 4);
      expected.insert(seed);
      bool changed;
      do {
        changed = false;
        for (const auto& edge : edges)
          if (expected.count(edge.first))
            changed |= expected.insert(edge.second).second;
      } while (changed);
      PS_CHECK(queue.receive(seed / 2, point(seed % 2)).ok());
      for (;;) {
        auto item = queue.take();
        PS_CHECK(item.ok());
        if (!item.value())
          break;
        for (const auto& subscription : subscriptions)
          if (subscription.source == item.value()->record) {
            auto dirty = subscription.certificate.transpose(
                {0, 1, item.value()->changed, {}});
            PS_CHECK(dirty.ok());
            if (!dirty.value().empty())
              PS_CHECK(
                  queue.receive(subscription.consumer, dirty.value()).ok());
          }
      }
      for (unsigned record = 0; record < 7; ++record) {
        const auto actual = queue.accumulated(record);
        for (unsigned atom = 0; atom < 2; ++atom)
          PS_CHECK((actual.ok() && actual.value().contains({atom})) ==
                   (expected.count(record * 2 + atom) != 0));
      }
    }
  }
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(late_delta() == 0);
  PS_CHECK(random_graphs() == 0);
  PS_CHECK(failures_and_races() == 0);
  return 0;
}
