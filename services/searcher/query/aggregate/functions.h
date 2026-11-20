#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_FUNCTIONS_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_FUNCTIONS_H_

namespace duckdb {
class ClientContext;
}

namespace web3::searcher {

inline constexpr char kBignumMean[] = "web3_bignum_mean";

void RegisterAggregateFunctions(duckdb::ClientContext& context);

}  // namespace web3::searcher

#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_FUNCTIONS_H_
