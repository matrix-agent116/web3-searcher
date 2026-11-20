#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_SEGMENT_MERGER_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_SEGMENT_MERGER_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

namespace web3::searcher {

class Solidity;

class SegmentMerger {
 public:
  // The worker owns the supplied store handles until it stops.
  explicit SegmentMerger(std::vector<std::shared_ptr<Solidity>> stores);
  SegmentMerger(const SegmentMerger&) = delete;
  SegmentMerger& operator=(const SegmentMerger&) = delete;

  // The caller has durably published the base segment in the table manifest.
  static void RemoveObsoleteSegments(const std::filesystem::path& path,
                                     uint64_t base);

 private:
  static void Run(std::stop_token stop,
                  std::vector<std::shared_ptr<Solidity>> stores);
  static void CompactIfNeeded(Solidity& store, std::stop_token stop);

  std::jthread worker_;
};

}  // namespace web3::searcher

#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_SEGMENT_MERGER_H_
