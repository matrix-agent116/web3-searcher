#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_PLAN_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_PLAN_H_

#include <span>
#include <string>
#include <vector>

#include "common/http.h"
#include "common/search_limit.h"
#include "searcher/query/row_scan.h"

namespace web3::searcher {

// Compiled once per route. SQL refers to generated positions for all inputs
// and outputs, so route and request strings are never interpreted as SQL.
class AggregatePlan {
 public:
  static bool Required(Json query);
  AggregatePlan(Json query, SearchLimit limit, const TableSchema& schema);

  // COUNT(*) is the only valid projection without input columns or group keys.
  bool IsCountOnly() const { return columns_.types.empty(); }
  const ScanColumns& Columns() const { return columns_; }
  const std::string& Sql() const { return sql_; }
  std::span<const std::string> JsonKeyPrefixes() const {
    return json_key_prefixes_;
  }

 private:
  ScanColumns columns_;
  std::vector<std::string> json_key_prefixes_;
  std::string sql_;
};

}  // namespace web3::searcher

#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_PLAN_H_
