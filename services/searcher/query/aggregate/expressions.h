#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_EXPRESSIONS_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_EXPRESSIONS_H_

#include <string>
#include <string_view>

#include "common/http.h"

namespace web3::searcher {

struct ScanColumns;
struct TableSchema;

// Resolve schema fields to shared input slots while compiling SELECT and GROUP
// BY.
class AggregateExpressionCompiler {
 public:
  AggregateExpressionCompiler(const TableSchema& schema, ScanColumns& columns)
      : schema_(schema), columns_(columns) {}

  std::string BindColumn(std::string_view name);
  std::string Compile(Json item);

 private:
  const TableSchema& schema_;
  ScanColumns& columns_;
};

}  // namespace web3::searcher

#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_EXPRESSIONS_H_
