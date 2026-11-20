#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_QUERY_TEMPLATE_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_QUERY_TEMPLATE_H_

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/http.h"
#include "common/search_limit.h"
#include "searcher/query/aggregate/plan.h"
#include "searcher/query/row_scan.h"

namespace web3::searcher {

class QueryTemplate {
 public:
  explicit QueryTemplate(
      Json route, std::function<const TableSchema&()> resolve_schema = {});
  std::string Bind(Json input, bool get) const;
  const RowProjection& Projection() const;
  const std::optional<AggregatePlan>& Aggregate() const { return aggregate_; }
  SearchLimit Limit() const { return limit_; }

 private:
  struct Param {
    std::string name;
    std::string type;
    std::string get_key;
    std::string post_pointer;
    std::string Bind(Json input, bool get) const;
  };
  std::string ValidationExpression() const;
  void ReadParameters(simdjson::dom::object params);
  void Compile(std::string_view expression, bool infer_params);
  void ReadSelection(simdjson::dom::object selection,
                     const std::function<const TableSchema&()>& resolve_schema);

  std::vector<Param> params_;
  std::vector<std::variant<std::string, std::size_t>> parts_;
  std::optional<RowProjection> projection_;
  SearchLimit limit_;
  std::optional<AggregatePlan> aggregate_;
};

}  // namespace web3::searcher

#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_QUERY_TEMPLATE_H_
