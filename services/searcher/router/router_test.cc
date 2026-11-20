#include "searcher/router/router.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "searcher/manager/manager.h"
#include "searcher/manager/test_utils.h"
#include "searcher/router/internal_routes.h"

namespace web3::searcher {
namespace {

using RouterTest = test::CatalogFixture;

Reply Invoke(const Handler& handler, std::string_view body = "{}") {
  simdjson::dom::parser parser;
  std::optional<Reply> result;
  handler(parser.parse(body.data(), body.size()).value(), [&](Reply reply) {
    EXPECT_FALSE(result.has_value());
    result = std::move(reply);
  });
  return std::move(result).value();
}

TEST_F(RouterTest, LoadsRelativeRoutesAndExecutesGetAndPostAgainstTheCatalog) {
  Manager manager(path_);
  manager.EnsureTables();
  auto routes = InternalRoutes(manager);
  std::filesystem::create_directory(path_ / "urls");
  WriteFile("router.conf",
            "# routes\r\n\n urls/lookup.json \r\nurls/height.json\n");
  WriteFile("urls/lookup.json", R"({
    "path":"/tx","table":"transactions","operation":"search",
    "params":{"hash":{"get":"q","post":"/filter/id"}},
    "query":{"lucene":"hash:${hash}","select":{"public_value":"value"}}})");
  WriteFile("urls/height.json", R"({"path":"/height","methods":["GET"],
              "table":"transactions","operation":"block_number"})");
  LoadRoutes(path_ / "router.conf", manager, routes);
  const auto get = Invoke(routes.get.at("/tx"), R"({"q":"a"})");
  const auto post = Invoke(routes.post.at("/tx"), R"({"filter":{"id":"a"}})");
  EXPECT_EQ(get.status, 200);
  EXPECT_EQ(post.status, 200);
  EXPECT_EQ(get.body, R"({"ok":true,"hits":[{"public_value":"seed"}]})");
  EXPECT_EQ(post.body, get.body);
  EXPECT_THROW(Invoke(routes.get.at("/tx")), std::invalid_argument);
  EXPECT_FALSE(routes.post.contains("/height"));
  EXPECT_EQ(Invoke(routes.get.at("/height")).body,
            R"({"ok":true,"block_number":-1})");
  EXPECT_TRUE(routes.post.contains("/solidity"));
}

TEST_F(RouterTest, BuiltInUpdatesQueriesAndHeightShareOneView) {
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
  EXPECT_EQ(Invoke(height, R"({"table":"transactions"})").body,
            R"({"ok":true,"block_number":-1})");
  EXPECT_EQ(Invoke(search, R"({"query":"hash:a"})").body,
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
  EXPECT_THROW(Invoke(search, R"({"query":"*:*","fields":[]})"),
               std::invalid_argument);
  EXPECT_THROW(Invoke(search, R"({"query":"*:*","limit":0})"),
               std::invalid_argument);
}

TEST_F(RouterTest, InvalidManifestsReportTheFailingEntry) {
  Manager manager(path_);
  auto routes = InternalRoutes(manager);
  const auto config = path_ / "router.conf";
  EXPECT_THROW(LoadRoutes(config, manager, routes), std::runtime_error);
  WriteFile("router.conf", "  # no entries\n\r\n");
  EXPECT_THROW(LoadRoutes(config, manager, routes), std::invalid_argument);

  WriteFile("router.conf", "# routes\n\nroute.json\n");
  for (
      const auto* definition :
      {R"({"path":"/search","table":"t","operation":"block_number"})",
       R"({"path":"/health","table":"t","operation":"block_number"})",
       R"({"path":"bad","table":"t","operation":"block_number"})",
       R"({"path":"/q","table":"","operation":"block_number"})",
       R"({"path":"/q","table":"t","operation":"unknown"})",
       R"({"path":"/q","table":"t","operation":"block_number","params":{}})",
       R"({"path":"/q","table":"t","operation":"block_number","query":{}})",
       R"({"path":"/q","table":"t","operation":"block_number","methods":[]})",
       R"({"path":"/q","table":"t","operation":"block_number","methods":["PUT"]})",
       R"({"path":"/q","table":"t","operation":"block_number","methods":["GET","GET"]})",
       "{"}) {
    SCOPED_TRACE(definition);
    WriteFile("route.json", definition);
    auto fresh = InternalRoutes(manager);
    try {
      LoadRoutes(config, manager, fresh);
      FAIL() << "invalid route accepted";
    } catch (const std::runtime_error& error) {
      EXPECT_NE(std::string_view(error.what()).find(config.string() + ":3"),
                std::string_view::npos);
      EXPECT_NE(std::string_view(error.what()).find("route.json"),
                std::string_view::npos);
    }
  }
}

TEST_F(RouterTest, ConfiguredRouteHidesStorageErrorDetails) {
  Manager manager(path_);
  manager.EnsureTables();
  HttpRoutes routes;
  WriteFile("router.conf", "lookup.json\n");
  WriteFile("lookup.json", R"({"path":"/tx","table":"missing_private_table",
                       "query":{"lucene":"*:*","select":{"id":"hash"}}})");
  LoadRoutes(path_ / "router.conf", manager, routes);
  const auto reply = Invoke(routes.get.at("/tx"));
  EXPECT_EQ(reply.status, 500);
  EXPECT_EQ(reply.body, R"({"ok":false,"error":"query execution failed"})");
}

TEST_F(RouterTest, RepeatedProjectionOfAWideColumnReturnsPayloadTooLarge) {
  const std::string value(kMaxHttpBodyBytes / 2, 'x');
  WriteFile("transactions.json",
            R"([{"hash":"a","value":")" + value + R"("}])");
  Manager manager(path_);
  manager.EnsureTables();
  HttpRoutes routes;
  WriteFile("router.conf", "wide.json\n");
  WriteFile("wide.json",
            R"({"path":"/wide","table":"transactions","methods":["POST"],
                   "query":{"lucene":"*:*","select":{"first":"value","second":"value"}}})");
  LoadRoutes(path_ / "router.conf", manager, routes);
  EXPECT_FALSE(routes.get.contains("/wide"));
  const auto reply = Invoke(routes.post.at("/wide"));
  EXPECT_EQ(reply.status, 413);
  EXPECT_EQ(reply.body, R"({"ok":false,"error":"response too large"})");
}

}  // namespace
}  // namespace web3::searcher
