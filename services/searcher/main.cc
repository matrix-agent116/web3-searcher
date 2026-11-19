#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "common/http.h"
#include "common/system.h"
#include "common/thread_pool.h"

namespace {

std::string Query(std::string_view table, std::string_view field,
                  std::string_view value) {
  return R"({"table":)" + web3::Quote(table) + R"(,"field":)" +
         web3::Quote(field) + R"(,"value":)" + web3::Quote(value) + "}";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 4 && std::string_view(argv[1]) == "serve") {
      web3::ThreadPool pool(web3::CpuCoreCount());
      web3::Serve(argv[2],
                  {{"/search",
                    [&](web3::Json request, web3::Completion done) {
                      auto table = request["table"];
                      return web3::PostAsync(
                          argv[3], "/search",
                          Query(table.error() == simdjson::NO_SUCH_FIELD
                                    ? "transactions"
                                    : table.get_string().value(),
                                request["field"].get_string().value(),
                                request["value"].get_string().value()),
                          std::move(done));
                    }}},
                  pool);
      return 0;
    }
    if ((argc == 5 || argc == 6) && std::string_view(argv[1]) == "query") {
      return web3::PrintResponse(
          web3::Post(argv[argc - 1], "/search",
                     Query(argc == 6 ? argv[2] : "transactions", argv[argc - 3],
                           argv[argc - 2])));
    }
    std::cerr << "usage: searcher serve URL INDEXER_URL\n"
                 "       searcher query [TABLE] FIELD VALUE URL\n";
    return 2;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
