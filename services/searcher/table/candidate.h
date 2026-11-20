#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_CANDIDATE_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_CANDIDATE_H_

#include <cstdint>
#include <iresearch/store/memory_directory.hpp>
#include <map>
#include <memory>
#include <optional>

#include "searcher/table/memory_segment.h"
#include "searcher/table/snapshot.h"

namespace web3::searcher {

// Candidate blocks are volatile. Table serializes changes with solidified
// blocks. The rebuild worker constructs an immutable view for each accepted
// change.
class Candidate {
 public:
  Candidate(duckdb::DatabaseInstance& database, const TableSchema& schema);
  std::shared_ptr<const Snapshot> Replace(const Snapshot& confirmed,
                                          uint64_t height,
                                          simdjson::dom::array fields,
                                          simdjson::dom::array documents);
  std::shared_ptr<const Snapshot> Overlay(const Snapshot& confirmed) const;
  void DiscardThrough(uint64_t height);

 private:
  using Blocks = std::map<uint64_t, MemorySegment>;
  std::shared_ptr<const Snapshot> Compose(
      const Snapshot& confirmed, const Blocks& blocks,
      std::optional<uint64_t> submitted) const;
  const TableSchema& schema_;
  irs::MemoryDirectory directory_;
  irs::IndexWriter::ptr writer_;
  Blocks blocks_;
};

}  // namespace web3::searcher
#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_CANDIDATE_H_
