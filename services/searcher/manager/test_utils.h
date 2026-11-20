#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_MANAGER_TEST_UTILS_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_MANAGER_TEST_UTILS_H_

#include <filesystem>
#include <fstream>
#include <string_view>

#include "searcher/table/test_utils.h"

namespace web3::searcher::test {

class CatalogFixture : public TempDirectoryFixture {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(TempDirectoryFixture::SetUp());
    WriteFile("schema.json", R"({"tables":[
      {"name":"transactions","documents":"transactions.json","fields":[
        {"name":"hash","type":"VARCHAR","stored":true,"indexed":true},
        {"name":"value","type":"VARCHAR","stored":true,"indexed":false}]},
      {"name":"blocks","documents":"blocks.json","fields":[
        {"name":"hash","type":"VARCHAR","stored":true,"indexed":true}]}]})");
    WriteFile("transactions.json", R"([{"hash":"a","value":"seed"}])");
    WriteFile("blocks.json", "[]");
  }

  void WriteFile(const std::filesystem::path& name, std::string_view text) {
    std::ofstream stream;
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream.open(path_ / name);
    stream << text;
    stream.close();
  }
};

}  // namespace web3::searcher::test

#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_MANAGER_TEST_UTILS_H_
