#include "searcher/table/candidate.h"

#include <utility>
#include <vector>

#include "searcher/table/utils.h"

namespace web3::searcher {

Candidate::Candidate(duckdb::DatabaseInstance& database,
                     const TableSchema& schema)
    : schema_(schema), writer_(MakeIndexWriter(directory_, database)) {}

std::shared_ptr<const Snapshot> Candidate::Replace(
    const Snapshot& confirmed, std::shared_ptr<const Snapshot> published,
    uint64_t height, simdjson::dom::array fields,
    simdjson::dom::array documents) {
  const auto match = schema_.MatchFields(fields);
  std::optional<MemorySegment> block;
  if (documents.size() == 0) {
    if (!blocks_.contains(height)) return published;
  } else {
    // Candidates can be rebased on a different predecessor. Keep complete rows.
    const std::vector<uint8_t> full_rows(documents.size(), 0);
    block = MemorySegment::Build(0, *writer_, schema_,
                                 {documents, match, full_rows});
    if (blocks_.empty() || height > blocks_.rbegin()->first) {
      // The published view already contains every eligible predecessor.
      auto view = std::make_shared<Snapshot>(*published);
      view->ApplySegments(schema_, view->NextOrdinal(), {&block->reader, 1});
      blocks_.emplace_hint(blocks_.end(), height, std::move(*block));
      return view;
    }
  }
  auto next = blocks_;
  if (block) {
    next.insert_or_assign(height, std::move(*block));
  } else {
    next.erase(height);
  }
  auto view = Compose(confirmed, next, height);
  blocks_.swap(next);
  return view;
}

std::shared_ptr<const Snapshot> Candidate::Overlay(
    const Snapshot& confirmed) const {
  return Compose(confirmed, blocks_, std::nullopt);
}

std::shared_ptr<const Snapshot> Candidate::Compose(
    const Snapshot& confirmed, const Blocks& blocks,
    std::optional<uint64_t> submitted) const {
  std::shared_ptr<Snapshot> view;
  for (const auto& [height, block] : blocks) {
    if (height <= confirmed.BlockNumber()) continue;
    auto next = std::make_shared<Snapshot>(view ? *view : confirmed);
    const auto ordinal = next->NextOrdinal();
    try {
      next->ApplySegments(schema_, ordinal, {&block.reader, 1});
    } catch (const AmbiguousMatch&) {
      // A corrected predecessor can make a previously accepted key ambiguous.
      // Omit that whole candidate until a replacement resolves it.
      if (submitted == height) throw;
      if (!view) view = std::make_shared<Snapshot>(confirmed);
      continue;
    }
    view = std::move(next);
  }
  return view;
}

void Candidate::DiscardThrough(uint64_t height) {
  blocks_.erase(blocks_.begin(), blocks_.upper_bound(height));
}

}  // namespace web3::searcher
