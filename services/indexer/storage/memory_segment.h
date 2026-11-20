#ifndef WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_MEMORY_SEGMENT_H_
#define WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_MEMORY_SEGMENT_H_

#include <simdjson.h>

#include <cstddef>
#include <cstdint>
#include <iresearch/index/index_writer.hpp>
#include <span>

namespace web3::indexer {

struct TableSchema;

// An immutable, unmasked batch of operations in checkpoint order.
struct MemorySegment {
  struct Batch {
    simdjson::dom::array documents;
    std::span<const irs::field_id> match_fields;
    std::span<const uint8_t> column_updates;
  };

  static MemorySegment Build(size_t ordinal, irs::IndexWriter& writer,
                             const TableSchema& schema, const Batch& update);

  size_t ordinal;
  irs::SegmentReader reader;
  unsigned level = 0;
};

}  // namespace web3::indexer

#endif  // WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_MEMORY_SEGMENT_H_
