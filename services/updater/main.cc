#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include "common/http.h"

int main(int argc, char** argv) {
  try {
    if (argc == 4 && std::string_view(argv[1]) == "serve") {
      auto upsert = [&](web3::Json request, web3::Completion done) {
        web3::PostAsync(argv[3], "/update", simdjson::minify(request),
                        std::move(done));
      };
      web3::Serve(argv[2], {{"/upsert", upsert}, {"/update", upsert}});
      return 0;
    }
    if (argc == 7 && std::string_view(argv[1]) == "update") {
      simdjson::dom::parser parser;
      const auto block_number =
          parser.parse(std::string(argv[3])).get_uint64().value();
      const auto fields = simdjson::minify(
          parser.parse(std::string(argv[4])).get_array().value());
      auto documents = parser.load(argv[5]).get_array().value();
      return web3::PrintResponse(web3::Post(
          argv[6], "/update",
          R"({"table":)" + web3::Quote(argv[2]) + R"(,"block_number":)" +
              std::to_string(block_number) + R"(,"match_field":)" + fields +
              R"(,"documents":)" + simdjson::minify(documents) + "}"));
    }
    std::cerr << "usage: updater serve URL INDEXER_URL\n"
                 "       updater update TABLE BLOCK_NUMBER MATCH_FIELDS_JSON "
                 "DOCUMENTS.json URL\n";
    return 2;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
