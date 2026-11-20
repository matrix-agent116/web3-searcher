#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_QUERY_TEMPLATE_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_QUERY_TEMPLATE_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/http.h"
#include "common/search_limit.h"

namespace web3::searcher {

class QueryTemplate {
 public:
  explicit QueryTemplate(Json route);
  std::string Bind(Json input, bool get) const;
  std::vector<std::string_view> Fields() const;
  SearchLimit Limit() const { return limit_; }
  std::string Response(Json hits) const;

 private:
  struct Param {
    std::string name;
    std::string type;
    std::string get_key;
    std::string post_pointer;
    std::string Bind(Json input, bool get) const;
  };
  void ReadParameters(simdjson::dom::object params);
  void Compile(std::string_view expression, bool infer_params);
  void ReadSelection(simdjson::dom::object selection);

  std::vector<Param> params_;
  std::vector<std::variant<std::string, std::size_t>> parts_;
  struct OutputField {
    std::string json_key;
    std::string column;
  };
  std::vector<OutputField> select_;
  SearchLimit limit_;
  std::vector<std::string> columns_;
};

}  // namespace web3::searcher

#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_QUERY_TEMPLATE_H_
