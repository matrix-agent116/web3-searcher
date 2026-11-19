#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include "common/http.h"

int main(int argc, char** argv) {
  try {
    if (argc == 4 && std::string_view(argv[1]) == "serve") {
      web3::Serve(argv[2],
                  {{"/update", [&](web3::Json request, web3::Completion done) {
                      web3::PostAsync(argv[3], "/update",
                                      simdjson::minify(request),
                                      std::move(done));
                    }}});
      return 0;
    }
    if (argc == 5 && std::string_view(argv[1]) == "update") {
      simdjson::dom::parser parser;
      auto documents = parser.load(argv[3]).get_array().value();
      return web3::PrintResponse(web3::Post(
          argv[4], "/update",
          R"({"table":)" + web3::Quote(argv[2]) + R"(,"documents":)" +
              simdjson::minify(documents) + "}"));
    }
    std::cerr << "usage: updater serve URL INDEXER_URL\n"
                 "       updater update TABLE DOCUMENTS.json URL\n";
    return 2;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
