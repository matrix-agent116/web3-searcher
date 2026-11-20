#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_SNAPSHOT_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_SNAPSHOT_H_

#include <absl/numeric/int128.h>
#include <simdjson.h>

#include <cstddef>
#include <cstdint>
#include <iresearch/index/directory_reader.hpp>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/search_limit.h"
#include "searcher/config/schema.h"

namespace irs {
class MergeWriter;
}

namespace web3::searcher {

struct MemorySegment;

class AmbiguousMatch : public std::invalid_argument {
 public:
  AmbiguousMatch()
      : std::invalid_argument(
            "match_field combination matches multiple documents") {}
};

struct SearchSegment {
  std::size_t ordinal;
  irs::SegmentReader reader;
};

class Snapshot {
 public:
  absl::int128 BlockNumber() const { return block_number_; }
  std::string Search(const TableSchema& schema, std::string_view expression,
                     std::span<const std::string_view> fields = {},
                     SearchLimit limit = {}) const;
  void CommitBlock(uint64_t block_number);
  void AdvanceBlock(absl::int128 block_number);
  std::span<const SearchSegment> Segments() const { return segments_; }
  // Ordinals stay stable while physical merges replace or remove readers.
  std::size_t NextOrdinal() const { return next_ordinal_; }
  std::shared_ptr<const Snapshot> WithCompactedPrefix(
      const Snapshot& inputs, const irs::MergeWriter& merger,
      const irs::SegmentReader& merged) const;
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
  std::size_t next_ordinal_ = 0;
  std::vector<SearchSegment> segments_;
};

}  // namespace web3::searcher

#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_SNAPSHOT_H_
