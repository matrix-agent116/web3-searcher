#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_EXECUTOR_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_EXECUTOR_H_

#include <memory>
#include <string>
#include <string_view>

namespace web3::searcher {

class AggregatePlan;
class Snapshot;
struct TableSchema;

std::string ExecuteAggregate(const AggregatePlan& plan,
                             std::shared_ptr<const Snapshot> snapshot,
                             const TableSchema& schema,
                             std::string_view expression);

}  // namespace web3::searcher

#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_EXECUTOR_H_
