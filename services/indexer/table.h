#ifndef WEB3_SEARCHER_SERVICES_INDEXER_TABLE_H_
#define WEB3_SEARCHER_SERVICES_INDEXER_TABLE_H_

#include <simdjson.h>

#include <cstdint>
#include <filesystem>
#include <iresearch/index/directory_reader.hpp>
#include <iresearch/store/memory_directory.hpp>
#include <iresearch/store/mmap_directory.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "indexer/schema.h"

namespace web3 {

class Table {
 public:
  Table(const std::filesystem::path& path, duckdb::DatabaseInstance& database,
        TableSchema schema);
  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  static void Write(const std::filesystem::path& path,
                    duckdb::DatabaseInstance& database,
                    const TableSchema& schema, simdjson::dom::array documents);
  void Upsert(simdjson::dom::array documents);
  std::string Search(std::string_view field, std::string_view value) const;

 private:
  using RowId = std::pair<std::size_t, irs::doc_id_t>;
  using KeyMap = std::map<std::string, RowId, std::less<>>;
  using HiddenRows = std::set<RowId>;
  struct SearchSegment {
    std::size_t ordinal;
    irs::SegmentReader reader;
    std::size_t live_docs;
    std::shared_ptr<const std::vector<bool>> hidden;
  };
  using Snapshot = std::vector<SearchSegment>;
  static const SearchSegment& FindSegment(const Snapshot& segments,
                                          std::size_t ordinal);
  std::vector<uint8_t> PlanColumnUpdates(simdjson::dom::array documents,
                                         const Snapshot& segments,
                                         const KeyMap& keys) const;
  void ApplySegments(Snapshot& segments, std::size_t offset,
                     const irs::DirectoryReader& added, const KeyMap& existing,
                     KeyMap& changes) const;
  void ApplyColumnPatches(SearchSegment& target,
                          const std::map<irs::doc_id_t, RowId>& rows,
                          const irs::DirectoryReader& source,
                          std::size_t first_ordinal,
                          const HiddenRows& hidden) const;
  void Publish(std::shared_ptr<const Snapshot> snapshot);
  std::string SearchByPrimaryKey(std::string_view value) const;
  std::string ReadRow(const irs::SubReader& segment, irs::doc_id_t id) const;
  void Open(std::string manifest);
  irs::SegmentReader IndexInMemory(simdjson::dom::array documents,
                                   std::span<const uint8_t> column_updates);

  std::filesystem::path path_;
  duckdb::DatabaseInstance& database_;
  TableSchema schema_;
  irs::MMapDirectory directory_;
  irs::MemoryDirectory memory_;
  irs::IndexWriter::ptr memory_writer_;
  // One decision per WAL row, in replay order, until the next checkpoint.
  std::vector<uint8_t> buffered_column_updates_;
  std::uintmax_t log_size_ = 0;
  std::mutex writer_mutex_;
  std::size_t segment_count_ = 0;
  uint64_t last_segment_id_ = 0;
  std::string manifest_;
  // Writers hold keys_mutex_ through snapshot publication so point lookups
  // get a row ID and segment reader from the same commit.
  mutable std::shared_mutex keys_mutex_;
  KeyMap keys_;
  mutable std::mutex snapshot_mutex_;
  std::shared_ptr<const Snapshot> snapshot_;
};

}  // namespace web3

#endif  // WEB3_SEARCHER_SERVICES_INDEXER_TABLE_H_
