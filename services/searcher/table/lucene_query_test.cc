#include "searcher/table/lucene_query.h"

#include <gtest/gtest.h>

#include <stdexcept>

#include "searcher/table/test_utils.h"

namespace web3::searcher {
namespace {

using LuceneQueryTest = test::SolidityFixture;

TEST_F(LuceneQueryTest, FieldGroupMatchesEscapedKeywordAcrossSegments) {
  Upsert(0, R"([{"id":"a:1","tag":"Burn"},{"id":"b","tag":"Mint"}])");
  Upsert(1, R"([{"id":"c","tag":"Mint"}])");

  const auto search = [&](std::string_view expression) {
    return solidity_->ReadSnapshot()->Search(solidity_->Schema(), expression);
  };
  EXPECT_EQ(search(R"(id:a\:1)"), search("tag:Burn"));
  EXPECT_NE(search("tag:Burn"), search("tag:burn"));
  EXPECT_EQ(search(R"(id:(a\:1 OR c) AND NOT tag:Burn)"), search("id:c"));
  EXPECT_NE(search("id:c"), search("id:missing"));
}

TEST_F(LuceneQueryTest, RejectsUnknownAndStoredOnlyFieldsBeforeExecution) {
  const auto& schema = solidity_->Schema();
  EXPECT_THROW(ParseLuceneQuery(schema, "id:a OR unknown:x"),
               std::invalid_argument);
  EXPECT_THROW(ParseLuceneQuery(schema, "value:x"), std::invalid_argument);
  EXPECT_THROW(ParseLuceneQuery(schema, "id:("), std::invalid_argument);
}

}  // namespace
}  // namespace web3::searcher
