#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_ROUTER_TEST_UTILS_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_ROUTER_TEST_UTILS_H_

#include <gtest/gtest.h>

#include <optional>
#include <string_view>
#include <utility>

#include "common/http.h"

namespace web3::searcher::test {

inline Reply Invoke(const Handler& handler, std::string_view body = "{}") {
  simdjson::dom::parser parser;
  std::optional<Reply> result;
  handler(parser.parse(body.data(), body.size()).value(), [&](Reply reply) {
    EXPECT_FALSE(result.has_value());
    result = std::move(reply);
  });
  return std::move(result).value();
}

}  // namespace web3::searcher::test

#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_ROUTER_TEST_UTILS_H_
