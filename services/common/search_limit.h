#ifndef WEB3_SEARCHER_SERVICES_COMMON_SEARCH_LIMIT_H_
#define WEB3_SEARCHER_SERVICES_COMMON_SEARCH_LIMIT_H_

#include <simdjson.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace web3 {

class SearchLimit {
 public:
  static constexpr std::size_t kMax = 65535;

  SearchLimit() = default;
  explicit SearchLimit(uint64_t value) {
    if (value == 0 || value > kMax)
      throw std::invalid_argument("limit must be between 1 and " +
                                  std::to_string(kMax));
    value_ = static_cast<std::size_t>(value);
  }

  static SearchLimit FromJson(simdjson::dom::element object) {
    auto field = object["limit"];
    if (field.error() == simdjson::NO_SUCH_FIELD) return {};
    uint64_t value;
    if (field.get_uint64().get(value))
      throw std::invalid_argument("limit must be a positive integer");
    return SearchLimit(value);
  }

  std::size_t value() const { return value_; }

 private:
  std::size_t value_ = 100;
};

}  // namespace web3

#endif  // WEB3_SEARCHER_SERVICES_COMMON_SEARCH_LIMIT_H_
