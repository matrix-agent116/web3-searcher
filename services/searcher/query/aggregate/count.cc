#include "searcher/query/aggregate/count.h"

#include <iresearch/search/queries/docs_mask_query.hpp>
#include <stdexcept>

#include "searcher/query/aggregate/session.h"
#include "searcher/table/snapshot.h"

namespace web3::searcher {

uint64_t CountMatches(const Snapshot& snapshot, const irs::Filter& filter,
                      const AggregateSession& session) {
  uint64_t count = 0;
  for (const auto& entry : snapshot.Segments()) {
    session.CheckDeadline();
    const auto& segment = entry.reader;
    auto query = filter.PrepareSegment(segment, {});
    if (!query || irs::QueryBuilder::IsEmpty(*query)) continue;
    if (query->Kind() == irs::QueryKind::All) {
      count += segment.live_docs_count();
      continue;
    }
    query = irs::WithDocsMask(std::move(query), segment, {});
    if (!query || irs::QueryBuilder::IsEmpty(*query)) continue;
    constexpr uint32_t kWindow = 65536;
    const uint64_t end = irs::doc_limits::min() + segment.docs_count();
    auto plan = query->PlanCount(
        {.span = kWindow, .partial = segment.docs_count() > kWindow});
    if (!plan) throw std::runtime_error("missing IResearch count plan");
    for (uint64_t begin = irs::doc_limits::min(); begin < end;
         begin += kWindow) {
      session.CheckDeadline();
      const auto stop = begin + kWindow >= end
                            ? irs::doc_limits::eof()
                            : static_cast<irs::doc_id_t>(begin + kWindow);
      count += plan->Run(static_cast<irs::doc_id_t>(begin), stop);
    }
    count += plan->Finish();
  }
  return count;
}

}  // namespace web3::searcher
