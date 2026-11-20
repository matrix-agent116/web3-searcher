#ifndef WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_UTILS_H_
#define WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_UTILS_H_

#include <absl/functional/function_ref.h>
#include <absl/numeric/int128.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iresearch/index/index_writer.hpp>
#include <memory>
#include <string>
#include <string_view>

namespace web3::indexer {

// A compacted base uses the high bit of its last input's ID. Appended segments
// and the WAL keep their original checkpoint IDs, including during a merge.
inline constexpr uint64_t kCompactedBase = uint64_t{1} << 63;

uint64_t SegmentId(std::string_view name);

irs::IndexWriter::ptr MakeIndexWriter(irs::Directory& target,
                                      duckdb::DatabaseInstance& database);

// Return the manifest entries for the synced batch and advance last_id.
std::string WriteDiskSegments(
    irs::Directory& target, duckdb::DatabaseInstance& database,
    uint64_t& last_id,
    absl::FunctionRef<void(irs::IndexWriter::Transaction&)> fill_batch);

using DirectoryLock = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
// Serialize changes to the catalog or an individual table across processes.
DirectoryLock LockDirectory(const std::filesystem::path& directory);

void SyncFd(int fd, const std::filesystem::path& path);
void SyncPath(const std::filesystem::path& path);

struct StorageManifest {
  absl::int128 block_number = -1;
  std::string segments;
  bool operator==(const StorageManifest&) const = default;
};

StorageManifest ReadManifest(const std::filesystem::path& directory);
void WriteManifest(irs::Directory& target, std::string_view name,
                   const StorageManifest& manifest);
// Sync and rename the pending manifest. The caller syncs the parent before
// retiring the WAL or obsolete segments.
void ReplaceManifest(irs::Directory& target,
                     const std::filesystem::path& directory,
                     const StorageManifest& manifest);

}  // namespace web3::indexer

#endif  // WEB3_SEARCHER_SERVICES_INDEXER_STORAGE_UTILS_H_
