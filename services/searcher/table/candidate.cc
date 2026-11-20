#include "searcher/table/candidate.h"

#include <utility>
#include <vector>

#include "searcher/table/utils.h"

namespace web3::searcher {

Candidate::Candidate(duckdb::DatabaseInstance& database,
                     const TableSchema& schema)
    : schema_(schema), writer_(MakeIndexWriter(directory_, database)) {}

std::shared_ptr<const Snapshot> Candidate::Replace(
    const Snapshot& confirmed, uint64_t height, simdjson::dom::array fields,
    simdjson::dom::array documents) {
  const auto match = schema_.MatchFields(fields);
  auto next = blocks_;
  if (documents.size() == 0) {
    next.erase(height);
  } else {
    // Candidates can be rebased on a different predecessor. Keep complete rows.
    const std::vector<uint8_t> full_rows(documents.size(), 0);
    next.insert_or_assign(height,
                          MemorySegment::Build(0, *writer_, schema_,
                                               {documents, match, full_rows}));
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
