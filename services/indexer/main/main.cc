#include <absl/strings/str_cat.h>

#include <iostream>
#include <iresearch/utils/cpu_features.hpp>
#include <stdexcept>
#include <string_view>

#include "common/http.h"
#include "common/system.h"
#include "common/thread_pool.h"
#include "indexer/manager/manager.h"

namespace web3::indexer {
namespace {

int Run(int argc, char** argv) {
  std::string_view command = argc > 1 ? argv[1] : "";
  if (!((command == "serve" && argc == 4) ||
        (command == "build" && argc == 3))) {
    std::cerr << "usage: indexer serve INDEX_DIR URL\n"
                 "       indexer build INDEX_DIR\n";
    return 2;
  }
  try {
    irs::CheckCpuFeatures(argv[0]);
    Manager manager(argv[2]);
    auto result = manager.EnsureTables();
    if (command == "build") {
      return web3::PrintResponse({200, result});
    }
    manager.StartMerger();
    web3::ThreadPool pool(web3::CpuCoreCount());
    auto submit = [&](auto method) {
      return [&, method](web3::Json request, web3::Completion done) {
        const auto height = request["block_number"].get_uint64().value();
        const auto count =
            (manager.*method)(request["table"].get_string().value(), height,
                              request["match_field"].get_array().value(),
                              request["documents"].get_array().value());
        done({200, absl::StrCat(R"({"ok":true,"upserted":)", count,
                                R"(,"block_number":)", height, "}")});
      };
    };
    web3::Serve(argv[3],
                {
                    {"/upsert", submit(&Manager::ConfirmBlock)},
                    {"/update", submit(&Manager::ConfirmBlock)},
                    {"/candidate", submit(&Manager::SubmitCandidate)},
                    {"/block_number",
                     [&](web3::Json request, web3::Completion done) {
                       const auto number = manager.GetBlockNumber(
                           request["table"].get_string().value());
                       done({200, absl::StrCat(R"({"ok":true,"block_number":)",
                                               number, "}")});
                     }},
                    {"/search",
                     [&](web3::Json request, web3::Completion done) {
                       auto table = request["table"];
                       done({200, manager.Search(
                                      table.error() == simdjson::NO_SUCH_FIELD
                                          ? "transactions"
                                          : table.get_string().value(),
                                      request["field"].get_string().value(),
                                      request["value"].get_string().value())});
                     }},
                },
                pool);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}

}  // namespace
}  // namespace web3::indexer

int main(int argc, char** argv) { return web3::indexer::Run(argc, argv); }
