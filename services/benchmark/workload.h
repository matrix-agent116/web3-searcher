#ifndef WEB3_SEARCHER_SERVICES_BENCHMARK_WORKLOAD_H_
#define WEB3_SEARCHER_SERVICES_BENCHMARK_WORKLOAD_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/http.h"

namespace web3::benchmark {

enum QueryKind : std::size_t { kHash, kReceivedStats, kTopSenders };
struct QueryRoute {
  const char* name;
  const char* path;
};
inline constexpr std::array<QueryRoute, 3> kQueryRoutes{{
    {"hash", "/search"},
    {"received_stats", "/v1/account/received-stats"},
    {"top_senders", "/v1/account/top-senders"},
}};

std::string BlockBody(uint64_t height, uint32_t count);
std::string SearchBody(std::string_view hash);

struct Query {
  QueryKind kind;
  std::string key;
  bool expect_hit;
  std::string Body() const;
  void CheckHits(simdjson::dom::array hits) const;
};

Query SampleQuery(QueryKind kind, uint64_t sequence, uint64_t first_height,
                  uint32_t visible_blocks, uint32_t tx_per_block);

}  // namespace web3::benchmark

#endif  // WEB3_SEARCHER_SERVICES_BENCHMARK_WORKLOAD_H_
