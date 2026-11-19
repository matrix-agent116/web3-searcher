#include <iostream>
#include <iresearch/utils/cpu_features.hpp>
#include <stdexcept>
#include <string_view>

#include "common/http.h"
#include "common/system.h"
#include "common/thread_pool.h"
#include "indexer/table_manager.h"

int main(int argc, char** argv) {
  std::string_view command = argc > 1 ? argv[1] : "";
  if (!((command == "serve" && argc == 4) ||
        (command == "build" && argc == 3))) {
    std::cerr << "usage: indexer serve INDEX_DIR URL\n"
                 "       indexer build INDEX_DIR\n";
    return 2;
  }
  try {
    irs::CheckCpuFeatures(argv[0]);
    web3::TableManager tables(argv[2]);
    auto result = tables.EnsureTables();
    if (command == "build") {
      return web3::PrintResponse({200, result});
    }
    web3::ThreadPool pool(web3::CpuCoreCount());
    web3::Serve(
        argv[3],
        {
            {"/update",
             [&](web3::Json request, web3::Completion done) {
               done({200,
                     tables.Upsert(request["table"].get_string().value(),
                                   request["documents"].get_array().value())});
             }},
            {"/search",
             [&](web3::Json request, web3::Completion done) {
               auto table = request["table"];
               done(
                   {200, tables.Search(table.error() == simdjson::NO_SUCH_FIELD
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
}
