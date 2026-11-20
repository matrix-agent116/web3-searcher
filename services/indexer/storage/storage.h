#ifndef WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_STORAGE_H_
#define WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_STORAGE_H_

#include <absl/numeric/int128.h>
#include <simdjson.h>

#include <cstdint>
#include <filesystem>
#include <iresearch/store/mmap_directory.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "indexer/config/schema.h"
#include "indexer/storage/memory_segment_manager.h"
#include "indexer/storage/snapshot.h"
#include "indexer/storage/utils.h"

namespace web3::indexer {

class Storage {
 public:
  Storage(const std::filesystem::path& path,
          duckdb::shared_ptr<duckdb::DatabaseInstance> database,
          TableSchema schema);
  ~Storage();
  Storage(const Storage&) = delete;
  Storage& operator=(const Storage&) = delete;

  static void Write(const std::filesystem::path& path,
                    duckdb::DatabaseInstance& database,
                    const TableSchema& schema, simdjson::dom::array documents);
  absl::int128 GetBlockNumber() const;
  uint64_t Upsert(uint64_t block_number, simdjson::dom::array match_fields,
                  simdjson::dom::array documents);
  std::string Search(std::string_view field, std::string_view value) const;

 private:
  friend class SegmentMerger;

  void Publish(std::shared_ptr<const Snapshot> snapshot);
  // Returns the physical ID of the first disk segment (zero for an empty
  // table).
  uint64_t Open(StorageManifest manifest, bool replace_manifest = false);

  std::filesystem::path path_;
  duckdb::shared_ptr<duckdb::DatabaseInstance> database_;
  TableSchema schema_;
  irs::MMapDirectory directory_;
  MemorySegmentManager memory_segment_manager_;
  std::uintmax_t log_size_ = 0;
  std::mutex writer_mutex_;
  std::size_t disk_segment_count_ = 0;
  std::shared_ptr<const Snapshot> disk_snapshot_;
  uint64_t last_segment_id_ = 0;
  StorageManifest manifest_;
  mutable std::mutex snapshot_mutex_;
  std::shared_ptr<const Snapshot> snapshot_;
};

}  // namespace web3::indexer

#endif  // WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_STORAGE_H_
