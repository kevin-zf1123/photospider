#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace ps {

class GraphEventService {
 public:
  struct ComputeEvent {
    int id;
    std::string name;
    std::string source;
    double elapsed_ms;
  };

  void push(int id, const std::string& name, const std::string& source,
            double ms);
  std::vector<ComputeEvent> drain();

 private:
  std::mutex mutex_;
  std::vector<ComputeEvent> buffer_;
};

}  // namespace ps
