#include "searcher/router/routes.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include "searcher/manager/manager.h"
#include "searcher/manager/test_utils.h"
#include "searcher/query/aggregate/plan.h"
#include "searcher/router/internal_routes.h"
#include "searcher/router/test_utils.h"

namespace web3::searcher {
namespace {

using RoutesTest = test::CatalogFixture;
using test::Invoke;

TEST_F(RoutesTest, LoadsRelativeRoutesAndExecutesGetAndPostAgainstTheCatalog) {
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

TEST_F(RoutesTest, CountAllAndFilteredRowsAcrossNativeCountWindows) {
  std::string docs = "[";
  for (size_t i = 0; i < 65537; ++i) {
    if (i) docs += ',';
    docs += i % 2 ? R"({"hash":"odd"})" : R"({"hash":"even"})";
  }
  WriteFile("transactions.json", docs + "]");
  Manager manager(path_);
  manager.EnsureTables();
  simdjson::dom::parser parser;
  const std::string definition =
      R"({"select":{"n":{"op":"count","field":"*"},"copy":{"op":"count","field":"*"}}})";
  AggregatePlan plan(parser.parse(definition).value(), SearchLimit(1),
                     manager.Schema("transactions"));
  EXPECT_EQ(manager.Aggregate("transactions", plan, "*:*"),
            R"({"ok":true,"hits":[{"n":65537,"copy":65537}]})");
  EXPECT_EQ(manager.Aggregate("transactions", plan, "hash:even"),
            R"({"ok":true,"hits":[{"n":32769,"copy":32769}]})");
  EXPECT_EQ(manager.Aggregate("transactions", plan, "*:* AND NOT hash:even"),
            R"({"ok":true,"hits":[{"n":32768,"copy":32768}]})");
  EXPECT_EQ(manager.Aggregate("transactions", plan, "hash:absent"),
            R"({"ok":true,"hits":[{"n":0,"copy":0}]})");
}

TEST_F(RoutesTest, AggregatesPatchedGroupsAcrossBatchesAndSegments) {
  std::string docs = "[";
  for (int i = 0; i < 5000; ++i) {
    if (i) docs += ',';
    docs += R"({"hash":")" + std::to_string(i) + R"(","value":)" +
            (i % 2 ? R"("odd"})" : R"("even"})");
  }
  WriteFile("transactions.json", docs + "]");
  Manager manager(path_);
  manager.EnsureTables();
  auto routes = InternalRoutes(manager);
  WriteFile("router.conf", "counts.json\n");
  WriteFile("counts.json", R"({"path":"/counts","table":"transactions",
    "query":{"lucene":"*:*","select":{"group":"value","n":{"op":"count","field":"*"}},
             "group_by":["value"],"order_by":[{"field":"n","direction":"desc"},
                                              {"field":"group","direction":"asc"}],"limit":2}})");
  LoadRoutes(path_ / "router.conf", manager, routes);
  const auto& count = routes.get.at("/counts");
  EXPECT_EQ(
      Invoke(count).body,
      R"({"ok":true,"hits":[{"group":"even","n":2500},{"group":"odd","n":2500}]})");
  // An existing stored-only field is patched, and a new row is appended.
  ASSERT_EQ(Invoke(routes.post.at("/solidity"),
                   R"({"table":"transactions","block_number":1,
              "match_field":["hash"],"documents":[{"hash":"0","value":"odd"},
                                                   {"hash":"new","value":"odd"}]})")
                .status,
            200);
  EXPECT_EQ(
      Invoke(count).body,
      R"({"ok":true,"hits":[{"group":"odd","n":2502},{"group":"even","n":2499}]})");
}

TEST_F(RoutesTest,
       ProjectsWideIntegersUnderMultipleAliasesWithoutPrecisionLoss) {
  WriteFile("schema.json",
            R"({"tables":[{"name":"payments","documents":"rows.json","fields":[
    {"name":"id","type":"VARCHAR","stored":true,"indexed":true},
    {"name":"amount","type":"UHUGEINT","stored":true,"indexed":false},
    {"name":"tags","type":"VARCHAR[]","stored":true,"indexed":false}]}]})");
  WriteFile(
      "rows.json",
      R"([{"id":"tx","amount":"1267650600228229401496703205377","tags":["a",null]}])");
  WriteFile("router.conf", "projection.json\n");
  WriteFile("projection.json", R"({"path":"/p","table":"payments","query":{
    "lucene":"id:tx","select":{"wei":"amount","copy":"amount","labels":"tags"}}})");
  Manager manager(path_);
  manager.EnsureTables();
  HttpRoutes routes;
  LoadRoutes(path_ / "router.conf", manager, routes);
  const auto reply = Invoke(routes.get.at("/p"));
  EXPECT_EQ(reply.status, 200);
  EXPECT_EQ(
      reply.body,
      R"({"ok":true,"hits":[{"wei":1267650600228229401496703205377,"copy":1267650600228229401496703205377,"labels":["a",null]}]})");
}

TEST_F(RoutesTest, SearchReadsLatestStoredValueForRepeatedCandidateKey) {
  WriteFile("transactions.json",
            R"([{"hash":"c","value":"seed"},{"hash":"s","value":"live"}])");
  Manager manager(path_);
  manager.EnsureTables();
  auto routes = InternalRoutes(manager);
  ASSERT_EQ(
      Invoke(routes.post.at("/candidate"),
             R"({"table":"transactions","block_number":0,"match_field":["hash"],
    "documents":[{"hash":"c","value":"first"},{"hash":"c","value":"last"},{"hash":"d","value":"other"}]})")
          .status,
      200);
  EXPECT_EQ(manager.Search("transactions", "hash:c", {}, {}),
            R"({"ok":true,"hits":[{"hash":"c","value":"last"}]})");
}

TEST_F(RoutesTest, BatchScanStopsBeforeColumnOnlyUpdatesAtEndOfSegment) {
  WriteFile("schema.json", R"({"tables":[{"name":"payments","fields":[
    {"name":"id","type":"VARCHAR","stored":true,"indexed":true},
    {"name":"sender","type":"VARCHAR","stored":true,"indexed":true},
    {"name":"amount","type":"DOUBLE","stored":true,"indexed":false}]}]})");
  Manager manager(path_);
  manager.EnsureTables();
  auto routes = InternalRoutes(manager);
  ASSERT_EQ(Invoke(routes.post.at("/solidity"),
                   R"({"table":"payments","block_number":0,"match_field":["id"],
    "documents":[{"id":"a","sender":"x","amount":1},{"id":"b","sender":"x","amount":2},
                 {"id":"c","sender":"x","amount":3},{"id":"c","sender":"x","amount":7}]})")
                .status,
            200);
  simdjson::dom::parser parser;
  const std::string definition =
      R"({"group_by":["sender"],"select":{"who":"sender","total":{"op":"sum","field":"amount"}}})";
  AggregatePlan plan(parser.parse(definition).value(), {},
                     manager.Schema("payments"));
  EXPECT_EQ(manager.Aggregate("payments", plan, "*:*"),
            R"({"ok":true,"hits":[{"who":"x","total":10}]})");
}

TEST_F(RoutesTest, FloatingPointAggregatesIgnoreNulls) {
  WriteFile("schema.json",
            R"({"tables":[{"name":"payments","documents":"rows.json","fields":[
    {"name":"id","type":"VARCHAR","stored":true,"indexed":true},
    {"name":"fee","type":"FLOAT","stored":true,"indexed":false},
    {"name":"rate","type":"DOUBLE","stored":true,"indexed":false}]}]})");
  WriteFile("rows.json", R"([{"id":"a","fee":-0.25,"rate":0.125},
                              {"id":"b","fee":1.5,"rate":0.375},{"id":"c"}])");
  Manager manager(path_);
  manager.EnsureTables();
  simdjson::dom::parser parser;
  for (const std::string field : {"fee", "rate"}) {
    SCOPED_TRACE(field);
    const std::string select = R"({"select":{
      "n":{"op":"count","field":")" +
                               field + R"("},
      "sum":{"op":"sum","field":")" +
                               field + R"("},
      "lo":{"op":"min","field":")" +
                               field + R"("},
      "hi":{"op":"max","field":")" +
                               field + R"("},
      "avg":{"op":"avg","field":")" +
                               field + R"("}}})";
    AggregatePlan plan(parser.parse(select).value(), {},
                       manager.Schema("payments"));
    const auto reply = manager.Aggregate("payments", plan, "*:*");
    auto hit = parser.parse(reply)["hits"].at(0);
    EXPECT_EQ(hit["n"].get_int64().value(), 2);
    EXPECT_DOUBLE_EQ(hit["sum"].get_double().value(),
                     field == "fee" ? 1.25 : 0.5);
    EXPECT_DOUBLE_EQ(hit["lo"].get_double().value(),
                     field == "fee" ? -0.25 : 0.125);
    EXPECT_DOUBLE_EQ(hit["hi"].get_double().value(),
                     field == "fee" ? 1.5 : 0.375);
    EXPECT_DOUBLE_EQ(hit["avg"].get_double().value(),
                     field == "fee" ? 0.625 : 0.25);
  }
}

TEST_F(RoutesTest, BignumAverageUsesExactSignedIntegerDivision) {
  WriteFile("schema.json",
            R"({"tables":[{"name":"payments","documents":"rows.json","fields":[
    {"name":"id","type":"VARCHAR","stored":true,"indexed":true},
    {"name":"amount","type":"BIGNUM","stored":true,"indexed":false}]}]})");
  // Three copies make the sum wider than 256 bits, with nonzero low bits.
  const std::string amount =
      "115792089237316195423570985008687907853269984665640564039457584007913129"
      "639935";
  WriteFile(
      "rows.json",
      R"([{"id":"big","amount":")" + amount + R"("},{"id":"big","amount":")" +
          amount + R"("},{"id":"big","amount":")" + amount +
          R"("},{"id":"neg","amount":"-8"},{"id":"neg","amount":"3"},{"id":"neg"},
         {"id":"pos","amount":"8"},{"id":"pos","amount":"-3"},
         {"id":"zero","amount":"-1"},{"id":"zero","amount":"0"},{"id":"empty"}])");
  Manager manager(path_);
  manager.EnsureTables();
  simdjson::dom::parser parser;
  const std::string definition = R"({"select":{
    "mean":{"op":"avg","field":"amount"},"least":{"op":"min","field":"amount"},
    "greatest":{"op":"max","field":"amount"},"total":{"op":"sum","field":"amount"}}})";
  AggregatePlan plan(parser.parse(definition).value(), {},
                     manager.Schema("payments"));
  auto hit = [&](const std::string& id) {
    auto reply = manager.Aggregate("payments", plan, "id:" + id);
    return parser.parse(reply)["hits"].at(0).value();
  };
  const auto big = hit("big");
  EXPECT_EQ(big["mean"].get_string().value(), amount);
  EXPECT_EQ(big["least"].get_string().value(), amount);
  EXPECT_EQ(big["greatest"].get_string().value(), amount);
  EXPECT_EQ(big["total"].get_string().value(),
            "347376267711948586270712955026063723559809953996921692118372752023"
            "739388919805");
  EXPECT_EQ(hit("neg")["mean"].get_string().value(), "-2");
  EXPECT_EQ(hit("pos")["mean"].get_string().value(), "2");
  EXPECT_EQ(hit("zero")["mean"].get_string().value(), "0");
  EXPECT_TRUE(hit("empty")["mean"].is_null());
  EXPECT_TRUE(hit("absent")["mean"].is_null());
}

TEST_F(RoutesTest, RejectsInvalidAggregateAtRouteLoad) {
  Manager manager(path_);
  manager.EnsureTables();
  WriteFile("router.conf", "bad.json\n");
  for (
      const auto* query :
      {R"({"lucene":"*:*","select":{"bad":{"op":"sum","field":"value"}}})",
       R"({"lucene":"*:*","select":{"bad":{"op":"avg","field":"value"}}})",
       R"({"lucene":"*:*","select":{"bad":{"op":"sum","field":"*"}}})",
       R"({"lucene":"*:*","select":{"bad":{"op":"sum","field":"value","format":"hex"}}})",
       R"({"lucene":"*:*","select":{"bad":{"op":"median","field":"hash"}}})",
       R"({"lucene":"*:*","select":{"v":"value","n":{"op":"count","field":"*"}}})",
       R"({"lucene":"*:*","select":{"n":{"op":"count","field":"*"}},"group_by":["absent"]})",
       R"({"lucene":"*:*","select":{"n":{"op":"count","field":"*"}},"group_by":["hash","hash"]})",
       R"({"lucene":"*:*","select":{"n":{"op":"count","field":"*"}},"order_by":[{"field":"hash","direction":"asc"}]})",
       R"({"lucene":"*:*","select":{"n":{"op":"count","field":"*"}},"order_by":[{"field":"n","direction":"invalid"}]})",
       R"({"lucene":"value:${x}","select":{"n":{"op":"count","field":"*"}}})"}) {
    SCOPED_TRACE(query);
    WriteFile("bad.json",
              std::string(R"({"path":"/bad","table":"transactions","query":)") +
                  query + "}");
    HttpRoutes routes;
    EXPECT_THROW(LoadRoutes(path_ / "router.conf", manager, routes),
                 std::runtime_error);
    EXPECT_TRUE(routes.post.empty());
  }
}

TEST_F(RoutesTest, InvalidManifestsReportTheFailingEntry) {
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

TEST_F(RoutesTest, ConfiguredRouteHidesStorageErrorDetails) {
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

TEST_F(RoutesTest, RepeatedProjectionOfAWideColumnReturnsPayloadTooLarge) {
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
