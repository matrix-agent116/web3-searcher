#include "searcher/router/internal_routes.h"

#include <absl/strings/str_cat.h>

#include <stdexcept>
#include <string_view>
#include <vector>

#include "common/search_limit.h"
#include "searcher/manager/manager.h"

namespace web3::searcher {

HttpRoutes InternalRoutes(Manager& manager) {
  auto submit = [&](auto method) {
    return [&manager, method](Json request, Completion done) {
      const auto height = request["block_number"].get_uint64().value();
      const auto count =
          (manager.*method)(request["table"].get_string().value(), height,
                            request["match_field"].get_array().value(),
                            request["documents"].get_array().value());
      done({200, absl::StrCat(R"({"ok":true,"upserted":)", count,
                              R"(,"block_number":)", height, "}")});
    };
  };
  return {.post = {
              {"/solidity", submit(&Manager::ConfirmBlock)},
              {"/candidate", submit(&Manager::SubmitCandidate)},
              {"/block_number",
               [&](Json request, Completion done) {
                 const auto number = manager.GetBlockNumber(
                     request["table"].get_string().value());
                 done({200, absl::StrCat(R"({"ok":true,"block_number":)",
                                         number, "}")});
               }},
              {"/search",
               [&](Json request, Completion done) {
                 std::vector<std::string_view> fields;
                 auto selected = request["fields"];
                 if (selected.error() != simdjson::NO_SUCH_FIELD) {
                   for (auto field : selected.get_array().value())
                     fields.push_back(field.get_string().value());
                   if (fields.empty())
                     throw std::invalid_argument("fields must be nonempty");
                 }
                 auto table = request["table"];
                 done({200,
                       manager.Search(table.error() == simdjson::NO_SUCH_FIELD
                                          ? "transactions"
                                          : table.get_string().value(),
                                      request["query"].get_string().value(),
                                      fields, SearchLimit::FromJson(request))});
               }},
          }};
}

}  // namespace web3::searcher
