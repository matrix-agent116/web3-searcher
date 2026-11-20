#include "searcher/router/routes.h"

#include <absl/strings/str_cat.h>

#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "searcher/manager/manager.h"
#include "searcher/query/aggregate/session.h"
#include "searcher/query/query_template.h"

namespace web3::searcher {
namespace {

struct Route {
  std::string table;
  std::optional<QueryTemplate> query_template;

  Reply Execute(const Manager& manager, Json input, bool get) const {
    // Bind request parameters before handling errors from the configured query.
    const auto query = query_template ? query_template->Bind(input, get) : "";
    try {
      if (!query_template)
        return {200, absl::StrCat(R"({"ok":true,"block_number":)",
                                  manager.GetBlockNumber(table), "}")};
      if (const auto& plan = query_template->Aggregate())
        return {200, manager.Aggregate(table, *plan, query)};
      return {200, manager.SearchProjected(table, query_template->Projection(),
                                           query, query_template->Limit())};
    } catch (const AggregateUnavailable&) {
      return {503,
              R"({"ok":false,"error":"aggregate resource limit exceeded"})"};
    } catch (const std::length_error&) {
      return {413, R"({"ok":false,"error":"response too large"})"};
    } catch (const std::exception&) {
      return {500, R"({"ok":false,"error":"query execution failed"})"};
    }
  }
};

void AddRoute(Json spec, const Manager& manager, HttpRoutes& routes) {
  const std::string path(spec["path"].get_string().value());
  if (!path.starts_with('/') || path == "/health" ||
      path.find_first_not_of("/abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRST"
                             "UVWXYZ0123456789._-") != path.npos)
    throw std::invalid_argument("invalid or reserved route path: " + path);
  if (routes.get.contains(path) || routes.post.contains(path))
    throw std::invalid_argument("duplicate URL: " + path);
  auto route = std::make_shared<Route>();
  route->table = spec["table"].get_string().value();
  if (route->table.empty()) throw std::invalid_argument("empty route table");
  auto operation = spec["operation"];
  if (operation.error() == simdjson::NO_SUCH_FIELD ||
      operation.get_string().value() == "search") {
    route->query_template.emplace(spec, [&]() -> const TableSchema& {
      return manager.Schema(route->table);
    });
  } else if (operation.get_string().value() != "block_number" ||
             spec["query"].error() != simdjson::NO_SUCH_FIELD ||
             spec["params"].error() != simdjson::NO_SUCH_FIELD) {
    throw std::invalid_argument("invalid route operation: " + path);
  }
  auto add_method = [&](std::string_view method) {
    const bool get = method == "GET";
    if (!get && method != "POST")
      throw std::invalid_argument("unsupported route method");
    auto& handlers = get ? routes.get : routes.post;
    auto handler = [route, get, &manager](Json input, Completion done) {
      done(route->Execute(manager, input, get));
    };
    if (!handlers.emplace(path, std::move(handler)).second)
      throw std::invalid_argument("duplicate route: " + path);
  };
  auto methods = spec["methods"];
  if (methods.error() == simdjson::NO_SUCH_FIELD) {
    add_method("GET");
    add_method("POST");
  } else {
    auto allowed = methods.get_array().value();
    if (allowed.size() == 0) throw std::invalid_argument("empty route methods");
    for (auto item : allowed) add_method(item.get_string().value());
  }
}

}  // namespace

void LoadRoutes(const std::filesystem::path& config, const Manager& manager,
                HttpRoutes& routes) {
  std::ifstream manifest(config);
  if (!manifest)
    throw std::runtime_error("cannot open route list: " + config.string());
  bool loaded = false;
  simdjson::dom::parser parser;
  std::string line;
  size_t number = 0;
  while (std::getline(manifest, line)) {
    ++number;
    const auto start = line.find_first_not_of(" \t\r");
    if (start == line.npos || line[start] == '#') continue;
    const auto end = line.find_last_not_of(" \t\r");
    const auto path =
        config.parent_path() / line.substr(start, end - start + 1);
    try {
      AddRoute(parser.load(path.string()).value(), manager, routes);
      loaded = true;
    } catch (const std::exception& error) {
      throw std::runtime_error(config.string() + ":" + std::to_string(number) +
                               " (" + path.string() + "): " + error.what());
    }
  }
  if (manifest.bad())
    throw std::runtime_error("cannot read route list: " + config.string());
  if (!loaded) throw std::invalid_argument("no routes in " + config.string());
}

}  // namespace web3::searcher
