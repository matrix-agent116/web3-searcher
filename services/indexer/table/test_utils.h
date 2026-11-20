#ifndef WEB3_SEARCHER_SERVICES_INDEXER_TABLE_TEST_UTILS_H_
#define WEB3_SEARCHER_SERVICES_INDEXER_TABLE_TEST_UTILS_H_

#include <gtest/gtest.h>
#include <simdjson.h>

#include <cstdint>
#include <cstdlib>
#include <duckdb/main/database.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "indexer/config/schema.h"
#include "indexer/table/solidity.h"

namespace web3::indexer::test {

inline simdjson::dom::array ParseArray(simdjson::dom::parser& parser,
                                       std::string_view json) {
  return parser.parse(json.data(), json.size()).get_array().value();
}

inline TableSchema MakeSchema() {
  TableSchema schema;
  schema.name = "transactions";
  schema.fields.push_back(
      {"id", duckdb::LogicalType::VARCHAR, true, true, 1, {}});
  schema.fields.push_back(
      {"tag", duckdb::LogicalType::VARCHAR, true, true, 2, {}});
  schema.fields.push_back(
      {"value", duckdb::LogicalType::VARCHAR, true, false, 3, {}});
  schema.leaves = {{"id", 1}, {"tag", 2}};
  schema.flush_docs = 2;
  schema.merge_segments = 2;
  return schema;
}

class TempDirectoryFixture : public testing::Test {
 protected:
  void SetUp() override {
    std::string path =
        (std::filesystem::temp_directory_path() / "web3-table-XXXXXX").string();
    ASSERT_NE(::mkdtemp(path.data()), nullptr);
    path_ = path;
  }

  void TearDown() override {
    if (!path_.empty()) std::filesystem::remove_all(path_);
  }

  std::filesystem::path path_;
};

class SolidityFixture : public TempDirectoryFixture {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(TempDirectoryFixture::SetUp());
    simdjson::dom::parser parser;
    Solidity::Write(path_, *db_.instance, MakeSchema(),
                    ParseArray(parser, "[]"));
    Reopen();
  }

  void TearDown() override {
    solidity_.reset();
    TempDirectoryFixture::TearDown();
  }

  void Reopen() {
    solidity_.reset();
    solidity_ = std::make_shared<Solidity>(path_, db_.instance, MakeSchema());
  }

  uint64_t Upsert(uint64_t height, std::string_view documents,
                  std::string_view fields = R"(["id"])") {
    simdjson::dom::parser fields_parser;
    simdjson::dom::parser docs_parser;
    return solidity_->Upsert(height, ParseArray(fields_parser, fields),
                             ParseArray(docs_parser, documents));
  }

  std::size_t HitCount(std::string_view id) {
    simdjson::dom::parser parser;
    return parser.parse(solidity_->Search("id", id))["hits"]
        .get_array()
        .value()
        .size();
  }

  duckdb::DuckDB db_{nullptr};
  std::shared_ptr<Solidity> solidity_;
};

}  // namespace web3::indexer::test
#endif  // WEB3_SEARCHER_SERVICES_INDEXER_TABLE_TEST_UTILS_H_
