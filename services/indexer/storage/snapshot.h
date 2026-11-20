#ifndef WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_SNAPSHOT_H_
#define WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_SNAPSHOT_H_

#include <absl/numeric/int128.h>
#include <simdjson.h>

#include <cstddef>
#include <cstdint>
#include <iresearch/index/directory_reader.hpp>
#include <span>
#include <vector>

#include "indexer/config/schema.h"

namespace web3::indexer {

struct MemorySegment;

struct SearchSegment {
  std::size_t ordinal;
  irs::SegmentReader reader;
};

class Snapshot {
 public:
  absl::int128 BlockNumber() const { return block_number_; }
  void AdvanceBlock(absl::int128 block_number);
  std::span<const SearchSegment> Segments() const { return segments_; }
  std::vector<uint8_t> PlanColumnUpdates(
      const TableSchema& schema, simdjson::dom::array documents,
      std::span<const irs::field_id> match_fields) const;
  // Apply to a private snapshot before publishing it to readers.
  void ApplySegments(const TableSchema& schema, std::size_t offset,
                     std::span<const irs::SegmentReader> added,
                     bool first_is_compacted = false);
  void ReplaceMemorySegments(std::span<const MemorySegment> inputs,
                             const irs::SegmentReader& merged);

 private:
  // Include -1 (no committed block) and every uint64_t input height.
  absl::int128 block_number_ = -1;
  std::vector<SearchSegment> segments_;
};

}  // namespace web3::indexer

#endif  // WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_SNAPSHOT_H_
