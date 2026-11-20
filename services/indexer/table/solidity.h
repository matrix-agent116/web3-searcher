#ifndef WEB3_SEARCHER_SERVICES_INDEXER_TABLE_SOLIDITY_H_
#define WEB3_SEARCHER_SERVICES_INDEXER_TABLE_SOLIDITY_H_

#include <absl/functional/any_invocable.h>
#include <absl/functional/function_ref.h>
#include <absl/numeric/int128.h>
#include <simdjson.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iresearch/store/mmap_directory.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "indexer/config/schema.h"
#include "indexer/table/memory_segment_manager.h"
#include "indexer/table/snapshot.h"
#include "indexer/table/utils.h"

namespace web3::indexer {

class Solidity {
 public:
  // The listener may fail during preparation. Its returned action runs only
  // after the new base is committed, while the writer still has exclusive
  // access.
  using OnCommit = absl::AnyInvocable<void() && noexcept>;
  using SnapshotListener =
      std::function<OnCommit(std::shared_ptr<const Snapshot>)>;
  void SetSnapshotListener(SnapshotListener listener);

  Solidity(const std::filesystem::path& path,
           duckdb::shared_ptr<duckdb::DatabaseInstance> database,
           TableSchema schema);
  ~Solidity();
  Solidity(const Solidity&) = delete;
  Solidity& operator=(const Solidity&) = delete;

  static void Write(const std::filesystem::path& path,
                    duckdb::DatabaseInstance& database,
                    const TableSchema& schema, simdjson::dom::array documents);
  const TableSchema& Schema() const { return schema_; }
  std::shared_ptr<const Snapshot> ReadSnapshot() const;
  // Refresh from disk, then keep WAL replay and the visitor's use of the
  // shared field analyzers serialized for the lifetime of this call.
  void WithCurrentSnapshot(
      absl::FunctionRef<void(const std::shared_ptr<const Snapshot>&)> visit);
  absl::int128 GetBlockNumber() const;
  uint64_t Upsert(uint64_t block_number, simdjson::dom::array match_fields,
                  simdjson::dom::array documents);
  std::string Search(std::string_view field, std::string_view value) const;

 private:
  friend class SegmentMerger;
  // Caller holds the writer mutex and directory lock.
  void ReloadIfChanged();

  OnCommit PreparePublication(
      const std::shared_ptr<const Snapshot>& snapshot) const;
  void Publish(std::shared_ptr<const Snapshot> snapshot, OnCommit on_commit);
  // Returns the physical ID of the first disk segment (zero for an empty
  // table).
  uint64_t Open(SolidityManifest manifest, bool replace_manifest = false);

  std::filesystem::path path_;
  duckdb::shared_ptr<duckdb::DatabaseInstance> database_;
  TableSchema schema_;
  irs::MMapDirectory directory_;
  MemorySegmentManager memory_segment_manager_;
  std::uintmax_t log_size_ = 0;
  std::mutex writer_mutex_;
  SnapshotListener listener_;
  std::size_t disk_segment_count_ = 0;
  std::shared_ptr<const Snapshot> disk_snapshot_;
  uint64_t last_segment_id_ = 0;
  SolidityManifest manifest_;
  mutable std::mutex snapshot_mutex_;
  std::shared_ptr<const Snapshot> snapshot_;
};

}  // namespace web3::indexer

#endif  // WEB3_SEARCHER_SERVICES_INDEXER_TABLE_SOLIDITY_H_
