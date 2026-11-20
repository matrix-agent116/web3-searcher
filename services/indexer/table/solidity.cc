////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2026 SereneDB GmbH, Berlin, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is SereneDB GmbH, Berlin, Germany
////////////////////////////////////////////////////////////////////////////////

#include "indexer/table/solidity.h"

#include <absl/cleanup/cleanup.h>

#include <algorithm>
#include <iresearch/index/segment_list.hpp>
#include <iresearch/index/segment_reader_impl.hpp>
#include <iresearch/store/mmap_directory.hpp>
#include <iresearch/utils/directory_utils.hpp>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "indexer/config/schema.h"
#include "indexer/table/row_codec.h"
#include "indexer/table/segment_merger.h"
#include "indexer/table/update_log.h"
#include "indexer/table/utils.h"

namespace web3::indexer {
namespace fs = std::filesystem;

Solidity::Solidity(const fs::path& path,
                   duckdb::shared_ptr<duckdb::DatabaseInstance> database,
                   TableSchema schema)
    : path_(path),
      database_(std::move(database)),
      schema_(std::move(schema)),
      directory_(path),
      memory_segment_manager_(*database_, schema_) {
  auto lock = LockDirectory(path_);
  const auto first = Open(ReadManifest(path_));
  if (first & kCompactedBase) {
    SyncPath(path_);
    SegmentMerger::RemoveObsoleteSegments(path_, first);
  }
  memory_segment_manager_.StartMerger(
      writer_mutex_, [this](auto inputs, const auto& merged) {
        auto view = std::make_shared<Snapshot>(*snapshot_);
        view->ReplaceMemorySegments(inputs, merged);
        auto on_commit = PreparePublication(view);
        Publish(std::move(view), std::move(on_commit));
      });
}

Solidity::~Solidity() { memory_segment_manager_.StopMerger(); }

void Solidity::Write(const fs::path& path, duckdb::DatabaseInstance& database,
                     const TableSchema& schema,
                     simdjson::dom::array documents) {
  irs::MMapDirectory dir(path);
  uint64_t last_id = 0;
  auto manifest = WriteDiskSegments(dir, database, last_id, [&](auto& batch) {
    for (auto document : documents) {
      InsertRow(batch, schema, document.get_object().value());
    }
  });
  WriteManifest(dir, "segments", {.segments = std::move(manifest)});
}

void Solidity::SetSnapshotListener(SnapshotListener listener) {
  std::lock_guard lock(writer_mutex_);
  if (listener) {
    auto on_commit = listener(snapshot_);
    std::move(on_commit)();
  }
  listener_ = std::move(listener);
}

Solidity::OnCommit Solidity::PreparePublication(
    const std::shared_ptr<const Snapshot>& snapshot) const {
  return listener_ ? listener_(snapshot) : OnCommit{};
}

void Solidity::Publish(std::shared_ptr<const Snapshot> snapshot,
                       OnCommit on_commit) {
  {
    std::lock_guard lock(snapshot_mutex_);
    snapshot_.swap(snapshot);
  }
  if (on_commit) std::move(on_commit)();
}

uint64_t Solidity::Open(SolidityManifest manifest, bool replace_manifest) {
  irs::IndexReaderOptions options;
  options.db = database_.get();
  auto disk = irs::OpenSegmentList(directory_, manifest.segments, options);
  uint64_t last_id = 0;
  for (const auto& segment : disk) {
    last_id =
        std::max(last_id, SegmentId(segment.Meta().name) & ~kCompactedBase);
  }
  UpdateLog log(path_, last_id);
  auto snapshot = std::make_shared<Snapshot>();
  snapshot->AdvanceBlock(manifest.block_number);
  const auto first_id = disk.size() ? SegmentId(disk[0].Meta().name) : 0;
  snapshot->ApplySegments(schema_, 0, disk.GetReaders(),
                          (first_id & kCompactedBase) != 0);
  auto disk_snapshot = std::make_shared<const Snapshot>(*snapshot);
  std::vector<MemorySegment> pieces;
  uint64_t buffered = 0;
  log.Replay([&](uint64_t block_number, simdjson::dom::array fields,
                 simdjson::dom::array docs) {
    snapshot->CommitBlock(block_number);
    const auto match = schema_.MatchFields(fields);
    if (docs.size() == 0) return;
    auto modes = snapshot->PlanColumnUpdates(schema_, docs, match);
    const auto ordinal = disk.size() + buffered;
    auto segment = memory_segment_manager_.Build(ordinal, {docs, match, modes});
    snapshot->ApplySegments(schema_, ordinal, {&segment.reader, 1});
    buffered += segment.reader.docs_count();
    pieces.push_back(std::move(segment));
  });
  const auto log_size = log.Size();
  auto on_commit = PreparePublication(snapshot);
  // Validate the merged base, subsequent flushes and WAL before committing.
  if (replace_manifest) {
    ReplaceManifest(directory_, path_, manifest);
  } else {
    SyncPath(path_);
  }
  log_size_ = log_size;
  memory_segment_manager_.Reset(std::move(pieces));
  last_segment_id_ = last_id;
  disk_snapshot_ = std::move(disk_snapshot);
  disk_segment_count_ = disk.size();
  manifest_ = std::move(manifest);
  Publish(std::move(snapshot), std::move(on_commit));
  return first_id;
}

std::shared_ptr<const Snapshot> Solidity::ReadSnapshot() const {
  std::lock_guard lock(snapshot_mutex_);
  return snapshot_;
}

absl::int128 Solidity::GetBlockNumber() const {
  return ReadSnapshot()->BlockNumber();
}

void Solidity::WithCurrentSnapshot(
    absl::FunctionRef<void(const std::shared_ptr<const Snapshot>&)> visit) {
  std::lock_guard lock(writer_mutex_);
  {
    auto file_lock = LockDirectory(path_);
    ReloadIfChanged();
  }
  visit(snapshot_);
}

void Solidity::ReloadIfChanged() {
  auto current = ReadManifest(path_);
  if (current != manifest_ ||
      UpdateLog(path_, last_segment_id_).Size() != log_size_) {
    Open(std::move(current));
  }
}

uint64_t Solidity::Upsert(uint64_t block_number, simdjson::dom::array fields,
                          simdjson::dom::array documents) {
  const auto match = schema_.MatchFields(fields);
  std::lock_guard lock(writer_mutex_);
  auto file_lock = LockDirectory(path_);
  ReloadIfChanged();

  UpdateLog log(path_, last_segment_id_);
  auto snapshot = std::make_shared<Snapshot>(*snapshot_);
  snapshot->CommitBlock(block_number);
  OnCommit on_commit;
  if (documents.size() == 0) {
    on_commit = PreparePublication(snapshot);
    log_size_ = log.Append(block_number, fields, documents, log_size_);
    Publish(std::move(snapshot), std::move(on_commit));
    return block_number;
  }

  auto modes = snapshot_->PlanColumnUpdates(schema_, documents, match);
  const auto ordinal = disk_segment_count_ + memory_segment_manager_.Docs();
  auto added =
      memory_segment_manager_.Build(ordinal, {documents, match, modes});
  memory_segment_manager_.Append(added);
  absl::Cleanup undo_piece = [&] { memory_segment_manager_.UndoAppend(); };
  const bool flush = memory_segment_manager_.Docs() >= schema_.flush_docs;
  if (flush) {
    auto last_id = last_segment_id_;
    irs::TrackingDirectory pending(directory_);
    absl::Cleanup rollback = [&] { pending.RemoveTracked(); };
    auto segments = memory_segment_manager_.Flush(pending, last_id);
    auto disk =
        irs::OpenSegmentList(directory_, segments, {.db = database_.get()});
    snapshot = std::make_shared<Snapshot>(*disk_snapshot_);
    snapshot->AdvanceBlock(block_number);
    snapshot->ApplySegments(schema_, disk_segment_count_, disk.GetReaders());
    SolidityManifest manifest{block_number, manifest_.segments + segments};
    on_commit = PreparePublication(snapshot);
    ReplaceManifest(pending, path_, manifest);
    std::move(rollback).Cancel();
    SyncPath(path_);
    std::move(undo_piece).Cancel();
    disk_segment_count_ += disk.size();
    disk_snapshot_ = snapshot;
    manifest_ = std::move(manifest);
    last_segment_id_ = last_id;
    memory_segment_manager_.Reset();
    log_size_ = 0;
  } else {
    snapshot->ApplySegments(schema_, ordinal, {&added.reader, 1});
    on_commit = PreparePublication(snapshot);
    log_size_ = log.Append(block_number, fields, documents, log_size_);
    std::move(undo_piece).Cancel();
  }
  Publish(std::move(snapshot), std::move(on_commit));
  if (flush) log.Retire();
  return block_number;
}

std::string Solidity::Search(std::string_view field,
                             std::string_view value) const {
  return ReadSnapshot()->Search(schema_, field, value);
}

}  // namespace web3::indexer
