#include "searcher/router/internal_routes.h"

#include <gtest/gtest.h>

#include <stdexcept>

#include "searcher/manager/manager.h"
#include "searcher/manager/test_utils.h"
#include "searcher/router/test_utils.h"

namespace web3::searcher {
namespace {

using InternalRoutesTest = test::CatalogFixture;
using test::Invoke;

TEST_F(InternalRoutesTest, UpdatesQueriesAndHeightShareOneView) {
  Manager manager(path_);
  manager.EnsureTables();
  const auto routes = InternalRoutes(manager);
  EXPECT_TRUE(routes.get.empty());
  ASSERT_EQ(routes.post.size(), 4);
  const auto& submit = routes.post.at("/candidate");
  const auto& confirm = routes.post.at("/solidity");
  const auto& search = routes.post.at("/search");
  const auto& height = routes.post.at("/block_number");
  const auto pending =
      Invoke(submit, R"({"table":"transactions","block_number":4,
      "match_field":["hash"],"documents":[{"hash":"a","value":"pending"}]})");
  EXPECT_EQ(pending.status, 200);
  EXPECT_EQ(pending.body, R"({"ok":true,"upserted":1,"block_number":4})");
  const auto pending_height = Invoke(height, R"({"table":"transactions"})");
  EXPECT_EQ(pending_height.status, 200);
  EXPECT_EQ(pending_height.body, R"({"ok":true,"block_number":-1})");
  const auto visible = Invoke(search, R"({"query":"hash:a"})");
  EXPECT_EQ(visible.status, 200);
  EXPECT_EQ(visible.body,
            R"({"ok":true,"hits":[{"hash":"a","value":"pending"}]})");

  const auto confirmed =
      Invoke(confirm, R"({"table":"transactions","block_number":4,
      "match_field":["hash"],"documents":[{"hash":"a","value":"final"},
                                          {"hash":"b","value":"final"}]})");
  EXPECT_EQ(confirmed.status, 200);
  EXPECT_EQ(confirmed.body, R"({"ok":true,"upserted":2,"block_number":4})");
  EXPECT_EQ(Invoke(height, R"({"table":"transactions"})").body,
            R"({"ok":true,"block_number":4})");
  EXPECT_EQ(
      Invoke(
          search,
          R"({"table":"transactions","query":"*:*","fields":["value"],"limit":1})")
          .body,
      R"({"ok":true,"hits":[{"value":"final"}]})");
  EXPECT_EQ(Invoke(search, R"({"table":"blocks","query":"*:*"})").body,
            R"({"ok":true,"hits":[]})");
}

TEST_F(InternalRoutesTest, EmptyFinalizedBlockAdvancesOnlyItsTable) {
  Manager manager(path_);
  manager.EnsureTables();
  const auto routes = InternalRoutes(manager);
  const auto& confirm = routes.post.at("/solidity");
  const auto& height = routes.post.at("/block_number");
  constexpr auto kGenesis =
      R"({"table":"blocks","block_number":0,"match_field":["hash"],"documents":[]})";
  const auto genesis = Invoke(confirm, kGenesis);
  EXPECT_EQ(genesis.status, 200);
  EXPECT_EQ(genesis.body, R"({"ok":true,"upserted":0,"block_number":0})");
  EXPECT_EQ(Invoke(height, R"({"table":"blocks"})").body,
            R"({"ok":true,"block_number":0})");
  EXPECT_EQ(Invoke(height, R"({"table":"transactions"})").body,
            R"({"ok":true,"block_number":-1})");
  EXPECT_THROW(Invoke(confirm, kGenesis), std::invalid_argument);

  const auto stale = Invoke(routes.post.at("/candidate"),
                            R"({"table":"blocks","block_number":0,
                    "match_field":["hash"],"documents":[{"hash":"late"}]})");
  EXPECT_EQ(stale.status, 200);
  EXPECT_EQ(stale.body, R"({"ok":true,"upserted":0,"block_number":0})");
  EXPECT_EQ(
      Invoke(routes.post.at("/search"), R"({"table":"blocks","query":"*:*"})")
          .body,
      R"({"ok":true,"hits":[]})");
}

TEST_F(InternalRoutesTest, InvalidBlockRequestsPreserveHeightAndDocuments) {
  Manager manager(path_);
  manager.EnsureTables();
  const auto routes = InternalRoutes(manager);
  for (const auto* path : {"/candidate", "/solidity"}) {
    SCOPED_TRACE(path);
    const auto& submit = routes.post.at(path);
    for (
        const auto* body :
        {R"({"block_number":1,"match_field":["hash"],"documents":[]})",
         R"({"table":"transactions","match_field":["hash"],"documents":[]})",
         R"({"table":"transactions","block_number":-1,"match_field":["hash"],"documents":[]})",
         R"({"table":"transactions","block_number":1,"match_field":"hash","documents":[]})",
         R"({"table":"transactions","block_number":1,"match_field":["hash"],"documents":{}})"}) {
      SCOPED_TRACE(body);
      EXPECT_THROW(Invoke(submit, body), simdjson::simdjson_error);
    }
    EXPECT_THROW(Invoke(submit, R"({"table":"transactions","block_number":1,
          "match_field":["value"],"documents":[{"hash":"a","value":"bad"}]})"),
                 std::invalid_argument);
    EXPECT_EQ(
        Invoke(routes.post.at("/block_number"), R"({"table":"transactions"})")
            .body,
        R"({"ok":true,"block_number":-1})");
    EXPECT_EQ(Invoke(routes.post.at("/search"), R"({"query":"*:*"})").body,
              R"({"ok":true,"hits":[{"hash":"a","value":"seed"}]})");
  }
}

TEST_F(InternalRoutesTest, ValidatesSearchFieldsAndLimit) {
  Manager manager(path_);
  manager.EnsureTables();
  const auto routes = InternalRoutes(manager);
  const auto& search = routes.post.at("/search");
  for (const auto* body :
       {R"({})", R"({"query":false})", R"({"query":"*:*","fields":"hash"})",
        R"({"query":"*:*","fields":["hash",1]})"}) {
    SCOPED_TRACE(body);
    EXPECT_THROW(Invoke(search, body), simdjson::simdjson_error);
  }
  for (const auto* body :
       {R"({"query":"*:*","fields":[]})",
        R"({"query":"*:*","fields":["unknown"]})",
        R"({"query":"*:*","fields":["hash","hash"]})",
        R"({"query":"*:*","limit":0})", R"({"query":"*:*","limit":65536})",
        R"({"query":"*:*","limit":"1"})"}) {
    SCOPED_TRACE(body);
    EXPECT_THROW(Invoke(search, body), std::invalid_argument);
  }
  EXPECT_EQ(
      Invoke(search, R"({"query":"*:*","fields":["hash"],"limit":65535})").body,
      R"({"ok":true,"hits":[{"hash":"a"}]})");
}

TEST_F(InternalRoutesTest, RejectsUnknownTablesAndMissingHeightTable) {
  Manager manager(path_);
  manager.EnsureTables();
  const auto routes = InternalRoutes(manager);
  constexpr auto kBlock =
      R"({"table":"unknown","block_number":1,"match_field":["hash"],"documents":[]})";
  EXPECT_THROW(Invoke(routes.post.at("/candidate"), kBlock),
               std::invalid_argument);
  EXPECT_THROW(Invoke(routes.post.at("/solidity"), kBlock),
               std::invalid_argument);
  EXPECT_THROW(
      Invoke(routes.post.at("/search"), R"({"table":"unknown","query":"*:*"})"),
      std::invalid_argument);
  const auto& height = routes.post.at("/block_number");
  EXPECT_THROW(Invoke(height, R"({"table":"unknown"})"), std::invalid_argument);
  EXPECT_THROW(Invoke(height), simdjson::simdjson_error);
  EXPECT_THROW(Invoke(height, R"({"table":42})"), simdjson::simdjson_error);
}

}  // namespace
}  // namespace web3::searcher
