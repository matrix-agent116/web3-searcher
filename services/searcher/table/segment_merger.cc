#include "searcher/table/segment_merger.h"

#include <absl/cleanup/cleanup.h>
#include <sys/file.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <iresearch/index/file_names.hpp>
#include <iresearch/index/merge_writer.hpp>
#include <iresearch/index/segment_list.hpp>
#include <iresearch/utils/async.hpp>
#include <iresearch/utils/directory_utils.hpp>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "searcher/table/solidity.h"
#include "searcher/table/update_log.h"
#include "searcher/table/utils.h"
#include "searcher/table/view_rebuilder.h"

namespace web3::searcher {
namespace fs = std::filesystem;
namespace {

struct PublishedViews {
  std::shared_ptr<const Snapshot> disk;
  std::shared_ptr<const Snapshot> confirmed;
  std::shared_ptr<const Snapshot> table;
  bool operator==(const PublishedViews&) const = default;
};

DirectoryLock TryLockCompaction(const fs::path& path) {
  DirectoryLock file(std::fopen((path / ".merge.lock").c_str(), "a"),
                     std::fclose);
  if (!file)
    throw std::system_error(errno, std::generic_category(), "open merge lock");
  while (flock(fileno(file.get()), LOCK_EX | LOCK_NB) != 0) {
    if (errno == EINTR) continue;
    if (errno == EWOULDBLOCK) return {nullptr, std::fclose};
    throw std::system_error(errno, std::generic_category(), "lock compaction");
  }
  return file;
}

}  // namespace

SegmentMerger::SegmentMerger(std::vector<std::shared_ptr<Solidity>> stores)
    : worker_(Run, std::move(stores)) {}

void SegmentMerger::Run(std::stop_token stop,
                        std::vector<std::shared_ptr<Solidity>> stores) {
  std::mutex mutex;
  std::unique_lock lock(mutex);
  std::condition_variable_any wake;
  while (!stop.stop_requested()) {
    for (const auto& store : stores) {
      if (stop.stop_requested()) return;
      try {
        if (store) CompactIfNeeded(*store, stop);
      } catch (const std::exception& error) {
        std::cerr << "compacting table " << store->schema_.name << ": "
                  << error.what() << '\n';
      }
    }
    wake.wait_for(lock, stop, std::chrono::seconds(1), [] { return false; });
  }
}

void SegmentMerger::CompactIfNeeded(Solidity& store, std::stop_token stop) {
  std::shared_ptr<const Snapshot> inputs;
  std::string prefix;
  uint64_t base;
  uint64_t generation;
  size_t input_count;
  DirectoryLock merge_lock{nullptr, std::fclose};
  {
    std::lock_guard lock(store.writer_mutex_);
    if (store.disk_segment_count_ < store.schema_.merge_segments) return;
    merge_lock = TryLockCompaction(store.path_);
    if (!merge_lock) return;
    auto file_lock = LockDirectory(store.path_);
    store.ReloadIfChanged();
    if (store.disk_segment_count_ < store.schema_.merge_segments) return;
    inputs = store.disk_snapshot_;
    prefix = store.manifest_.segments;
    base = store.last_segment_id_ | kCompactedBase;
    input_count = store.disk_segment_count_;
    generation = store.reload_generation_;
  }

  irs::TrackingDirectory pending(store.directory_);
  absl::Cleanup rollback = [&] { pending.RemoveTracked(); };
  irs::SegmentMeta meta;
  meta.name = irs::FileName(base);
  irs::MergeWriter merger(pending, {.db = store.database_.get()});
  std::vector<const irs::SubReader*> readers;
  for (const auto& input : inputs->Segments()) readers.push_back(&input.reader);
  merger.Reset(readers.begin(), readers.end());
  if (!irs::GetBlocking(
          merger.Flush(meta, [&] { return !stop.stop_requested(); }))) {
    if (stop.stop_requested()) return;
    throw std::runtime_error("IResearch could not merge the disk segments");
  }
  for (const auto& name : irs::SegmentFiles(meta.name, meta.kinds))
    SyncPath(store.path_ / name);
  SyncPath(store.path_);

  std::string base_entry;
  irs::AppendSegmentList(base_entry, base, meta.kinds, meta.docs_count);
  auto disk = irs::OpenSegmentList(store.directory_, base_entry,
                                   {.db = store.database_.get()});
  const auto compact = [&](const std::shared_ptr<const Snapshot>& view) {
    return view->WithCompactedPrefix(*inputs, merger,
                                     disk.GetReaders().front());
  };
  // Call only under the writer mutex, including when a Table detaches.
  const auto views = [&]() -> PublishedViews {
    return {store.disk_snapshot_, store.snapshot_,
            store.table_views_ ? store.table_views_->Read() : nullptr};
  };
  for (;;) {
    PublishedViews before;
    {
      std::lock_guard lock(store.writer_mutex_);
      if (stop.stop_requested() || generation != store.reload_generation_)
        return;
      before = views();
    }
    auto next_disk = compact(before.disk);
    auto next_confirmed =
        before.confirmed == before.disk ? next_disk : compact(before.confirmed);
    auto next_table = before.table && before.table != before.confirmed
                          ? compact(before.table)
                          : next_confirmed;
    std::unique_lock lock(store.writer_mutex_);
    const auto acquired = std::chrono::steady_clock::now();
    if (stop.stop_requested() || generation != store.reload_generation_) return;
    if (before != views()) continue;
    {
      auto file_lock = LockDirectory(store.path_);
      const auto current = ReadManifest(store.path_);
      // Another process may have committed to the same checkpoint's WAL.
      if (current != store.manifest_ || !current.segments.starts_with(prefix) ||
          UpdateLog(store.path_, store.last_segment_id_).Size() !=
              store.log_size_)
        return;
      SolidityManifest next{
          current.block_number,
          base_entry + current.segments.substr(prefix.size())};
      ReplaceManifest(store.directory_, store.path_, next);
      std::move(rollback).Cancel();
      store.manifest_ = std::move(next);
      store.disk_segment_count_ -= input_count - 1;
      store.disk_snapshot_ = std::move(next_disk);
      store.Publish(std::move(next_confirmed), {});
      if (store.table_views_)
        store.table_views_->PublishCompaction(std::move(next_table));
      SyncPath(store.path_);
    }
    lock.unlock();
    const auto held = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - acquired);
    std::fprintf(stderr, "compacted %s inputs=%zu publish_lock_us=%lld\n",
                 store.schema_.name.c_str(), input_count,
                 static_cast<long long>(held.count()));
    break;
  }
  // Existing query snapshots retain their mmap handles on Linux and macOS.
  RemoveObsoleteSegments(store.path_, base);
}

// Also finishes reclamation if the process died after publishing the base.
void SegmentMerger::RemoveObsoleteSegments(const fs::path& path,
                                           uint64_t base) {
  const auto checkpoint = base & ~kCompactedBase;
  for (const auto& entry : fs::directory_iterator(path)) {
    auto name = entry.path().filename().string();
    uint64_t id;
    if (!irs::ParseSegmentId(name, id) || id == base ||
        (id & ~kCompactedBase) > checkpoint)
      continue;
    const auto known = irs::SegmentFiles(irs::FileName(id), 0xff);
    if (std::ranges::find(known, name) != known.end()) fs::remove(entry.path());
  }
}

}  // namespace web3::searcher
