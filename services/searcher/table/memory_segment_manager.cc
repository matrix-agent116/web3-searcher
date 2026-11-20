#include "searcher/table/memory_segment_manager.h"

#include <chrono>
#include <iostream>
#include <iresearch/index/segment_list.hpp>
#include <iresearch/utils/async.hpp>
#include <stdexcept>
#include <utility>

#include "searcher/config/schema.h"
#include "searcher/table/utils.h"

namespace web3::searcher {

MemorySegmentManager::MemorySegmentManager(duckdb::DatabaseInstance& database,
                                           const TableSchema& schema)
    : schema_(schema),
      reader_options_{.db = &database},
      writer_(MakeIndexWriter(directory_, database)) {}

MemorySegment MemorySegmentManager::Build(size_t ordinal,
                                          const MemorySegment::Batch& update) {
  return MemorySegment::Build(ordinal, *writer_, schema_, update);
}

void MemorySegmentManager::Append(MemorySegment segment) {
  segments_.push_back(std::move(segment));
  docs_ += segments_.back().reader.docs_count();
  wake_.notify_one();
}

void MemorySegmentManager::UndoAppend() {
  docs_ -= segments_.back().reader.docs_count();
  segments_.pop_back();
}

void MemorySegmentManager::Reset(std::vector<MemorySegment> segments) {
  segments_ = std::move(segments);
  docs_ = 0;
  for (const auto& segment : segments_) docs_ += segment.reader.docs_count();
  wake_.notify_one();
}

void MemorySegmentManager::StartMerger(std::mutex& write_mutex,
                                       PublishMerge publish) {
  worker_ = std::jthread(
      [this, &write_mutex, publish = std::move(publish)](std::stop_token stop) {
        Run(stop, write_mutex, publish);
      });
}

void MemorySegmentManager::StopMerger() {
  worker_.request_stop();
  if (worker_.joinable()) worker_.join();
}

void MemorySegmentManager::Run(std::stop_token stop, std::mutex& write_mutex,
                               const PublishMerge& publish) {
  using namespace std::chrono_literals;
  while (!stop.stop_requested()) {
    try {
      std::vector<MemorySegment> inputs;
      size_t offset;
      {
        std::unique_lock lock(write_mutex);
        std::span<const MemorySegment> group;
        if (!wake_.wait(lock, stop, [&] {
              group = MergeCandidates();
              return !group.empty();
            }))
          return;
        offset = group.data() - segments_.data();
        inputs.assign(group.begin(), group.end());
      }
      auto merged = writer_->MergePieces(Readers(inputs),
                                         writer_->NextSegmentId(), nullptr);
      if (!merged.reader)
        throw std::runtime_error("could not merge memory segments");
      std::lock_guard lock(write_mutex);
      // A concurrent checkpoint or WAL reload may have replaced our inputs.
      if (stop.stop_requested() || segments_.size() < offset + inputs.size() ||
          segments_[offset].reader != inputs.front().reader)
        continue;
      irs::SegmentReader reader(std::move(merged.reader));
      publish(inputs, reader);
      segments_[offset] = {inputs.front().ordinal, std::move(reader),
                           inputs.front().level + 1};
      segments_.erase(segments_.begin() + offset + 1,
                      segments_.begin() + offset + inputs.size());
    } catch (const std::exception& error) {
      std::cerr << "merging memory segments for " << schema_.name << ": "
                << error.what() << '\n';
      std::unique_lock lock(write_mutex);
      wake_.wait_for(lock, stop, 1s, [] { return false; });
    }
  }
}

irs::DirectoryReader MemorySegmentManager::Readers(
    std::span<const MemorySegment> pieces) const {
  std::vector<irs::SegmentReader> readers;
  readers.reserve(pieces.size());
  for (const auto& piece : pieces) readers.push_back(piece.reader);
  return {directory_, reader_options_, std::move(readers), /*stats=*/false};
}

std::span<const MemorySegment> MemorySegmentManager::MergeCandidates() const {
  std::span<const MemorySegment> pieces = segments_;
  if (pieces.size() < schema_.merge_segments) return {};
  size_t count = 0;
  uint64_t docs = 0;
  for (size_t i = 0; i < pieces.size(); ++i) {
    const auto rows = pieces[i].reader.docs_count();
    if (i == 0 || pieces[i].level != pieces[i - 1].level ||
        docs + rows > irs::doc_limits::kMaxDocs) {
      count = 0;
      docs = 0;
    }
    docs += rows;
    if (++count == schema_.merge_segments)
      return pieces.subspan(i + 1 - count, count);
  }
  return {};
}

std::string MemorySegmentManager::Flush(irs::Directory& target,
                                        uint64_t& last_id) const {
  std::span<const MemorySegment> pieces = segments_;
  auto writer = MakeIndexWriter(target, *reader_options_.db);
  std::string manifest;
  while (!pieces.empty()) {
    size_t count = 0;
    uint64_t docs = 0;
    for (const auto& piece : pieces) {
      if (docs + piece.reader.docs_count() > irs::doc_limits::kMaxDocs) break;
      docs += piece.reader.docs_count();
      ++count;
    }
    const auto id = ++last_id;
    auto result = irs::GetBlocking(writer->WriteAsync(
        Readers(pieces.first(count)), id, nullptr, [] { return true; },
        nullptr));
    if (!result.reader)
      throw std::runtime_error("could not flush memory segments");
    irs::AppendSegmentList(manifest, id, result.meta.kinds,
                           result.meta.docs_count);
    pieces = pieces.subspan(count);
  }
  return manifest;
}

}  // namespace web3::searcher
