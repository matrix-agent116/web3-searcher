#ifndef WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_MEMORY_SEGMENT_MANAGER_H_
#define WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_MEMORY_SEGMENT_MANAGER_H_

#include <condition_variable>
#include <functional>
#include <iresearch/store/memory_directory.hpp>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "indexer/storage/memory_segment.h"

namespace web3::indexer {

// Owned by one Storage. Foreground operations and merge publication share
// that table's write lock. The IResearch merge runs outside the lock.
class MemorySegmentManager {
 public:
  using PublishMerge = std::function<void(std::span<const MemorySegment>,
                                          const irs::SegmentReader&)>;

  MemorySegmentManager(duckdb::DatabaseInstance& database,
                       const TableSchema& schema);
  MemorySegment Build(size_t ordinal, const MemorySegment::Batch& update);
  void Append(MemorySegment segment);
  void UndoAppend();
  void Reset(std::vector<MemorySegment> segments = {});
  uint64_t Docs() const { return docs_; }
  std::string Flush(irs::Directory& target, uint64_t& last_id) const;

  void StartMerger(std::mutex& write_mutex, PublishMerge publish);
  void StopMerger();

 private:
  std::span<const MemorySegment> MergeCandidates() const;
  irs::DirectoryReader Readers(std::span<const MemorySegment> segments) const;
  void Run(std::stop_token stop, std::mutex& write_mutex,
           const PublishMerge& publish);

  const TableSchema& schema_;
  irs::MemoryDirectory directory_;
  irs::IndexReaderOptions reader_options_;
  irs::IndexWriter::ptr writer_;
  std::vector<MemorySegment> segments_;
  uint64_t docs_ = 0;
  std::condition_variable_any wake_;
  std::jthread worker_;
};

}  // namespace web3::indexer
#endif  // WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_MEMORY_SEGMENT_MANAGER_H_
