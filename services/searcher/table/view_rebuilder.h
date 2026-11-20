#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_VIEW_REBUILDER_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_VIEW_REBUILDER_H_

#include <absl/functional/function_ref.h>

#include <cstdint>
#include <memory>
#include <mutex>

#include "common/thread_pool.h"

namespace web3::searcher {

class Snapshot;

// Build on the table's worker. The writer can discard a prepared view on
// rollback or publish it once its underlying data is durable.
class ViewRebuilder {
 public:
  struct Version {
    uint64_t generation;
    std::shared_ptr<const Snapshot> snapshot;
  };

  explicit ViewRebuilder(std::shared_ptr<const Snapshot> initial);
  Version Prepare(absl::FunctionRef<std::shared_ptr<const Snapshot>()> build);
  void Publish(Version next) noexcept;
  // Same logical version. Caller holds the table writer lock and has
  // validated the snapshot from which the compacted view was built.
  void PublishCompaction(std::shared_ptr<const Snapshot> next) noexcept;
  std::shared_ptr<const Snapshot> Read() const;

 private:
  mutable std::mutex mutex_;
  Version published_;
  uint64_t generation_ = 0;  // Accessed only by the worker.
  ThreadPool worker_{1};
};

}  // namespace web3::searcher

#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_VIEW_REBUILDER_H_
