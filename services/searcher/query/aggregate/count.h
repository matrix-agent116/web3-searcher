#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_COUNT_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_COUNT_H_

#include <cstdint>

namespace irs {
class Filter;
}

namespace web3::searcher {

class AggregateSession;
class Snapshot;

uint64_t CountMatches(const Snapshot& snapshot, const irs::Filter& filter,
                      const AggregateSession& session);

}  // namespace web3::searcher

#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_COUNT_H_
