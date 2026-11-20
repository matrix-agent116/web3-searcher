#include "searcher/table/view_rebuilder.h"

#include <utility>

namespace web3::searcher {

ViewRebuilder::ViewRebuilder(std::shared_ptr<const Snapshot> initial)
    : published_{0, std::move(initial)} {}

ViewRebuilder::Version ViewRebuilder::Prepare(
    absl::FunctionRef<std::shared_ptr<const Snapshot>()> build) {
  // Waiting here keeps the submitting request (including its JSON parser)
  // alive until the worker returns its result or exception.
  return worker_
      .Commit([this, build] { return Version{++generation_, build()}; })
      .get();
}

void ViewRebuilder::Publish(Version next) noexcept {
  std::lock_guard lock(mutex_);
  if (next.generation > published_.generation) {
    std::swap(published_, next);
  }
}

std::shared_ptr<const Snapshot> ViewRebuilder::Read() const {
  std::lock_guard lock(mutex_);
  return published_.snapshot;
}

void ViewRebuilder::PublishCompaction(
    std::shared_ptr<const Snapshot> next) noexcept {
  std::lock_guard lock(mutex_);
  published_.snapshot.swap(next);
}

}  // namespace web3::searcher
