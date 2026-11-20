#ifndef WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_SEGMENT_MERGER_H_
#define WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_SEGMENT_MERGER_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

namespace web3::indexer {

class Storage;

class SegmentMerger {
 public:
  // The worker owns the supplied storage handles until it stops.
  explicit SegmentMerger(std::vector<std::shared_ptr<Storage>> storages);
  SegmentMerger(const SegmentMerger&) = delete;
  SegmentMerger& operator=(const SegmentMerger&) = delete;

  // The caller has durably published the base segment in the table manifest.
  static void RemoveObsoleteSegments(const std::filesystem::path& path,
                                     uint64_t base);

 private:
  static void Run(std::stop_token stop,
                  std::vector<std::shared_ptr<Storage>> storages);
  static void CompactIfNeeded(Storage& storage, std::stop_token stop);

  std::jthread worker_;
};

}  // namespace web3::indexer

#endif  // WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_SEGMENT_MERGER_H_
