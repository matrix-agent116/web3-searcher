#include "indexer/manager/manager.h"

#include <duckdb/main/connection.hpp>
#include <stdexcept>
#include <utility>
#include <vector>

#include "indexer/config/schema.h"
#include "indexer/table/table.h"

namespace web3::indexer {
namespace fs = std::filesystem;
Manager::Manager(fs::path directory) : directory_(std::move(directory)) {}

Manager::~Manager() = default;

std::string Manager::EnsureTables() {
  std::size_t indexed = 0;
  std::call_once(initialized_, [&] {
    duckdb::Connection connection(database_);
    auto schemas = ReadSchema(directory_ / "schema.json", *connection.context);
    decltype(tables_) opened;
    for (auto& schema : schemas) {
      auto name = schema.name;
      opened.emplace(name, Table::Open(directory_, database_.instance,
                                       std::move(schema), indexed));
    }
    tables_.swap(opened);
  });
  return R"({"ok":true,"tables":)" + std::to_string(tables_.size()) +
         R"(,"indexed":)" + std::to_string(indexed) + "}";
}

void Manager::StartMerger() {
  std::vector<std::shared_ptr<Table>> tables;
  tables.reserve(tables_.size());
  for (const auto& [name, table] : tables_) tables.push_back(table);
  merger_ = Table::StartMerger(tables);
}

Table& Manager::GetTable(std::string_view name) const {
  auto table = tables_.find(name);
  if (table == tables_.end())
    throw std::invalid_argument("unknown table: " + std::string(name));
  return *table->second;
}

absl::int128 Manager::GetBlockNumber(std::string_view name) const {
  return GetTable(name).GetBlockNumber();
}

std::size_t Manager::ConfirmBlock(std::string_view name, uint64_t height,
                                  simdjson::dom::array fields,
                                  simdjson::dom::array documents) {
  return GetTable(name).ConfirmBlock(height, fields, documents);
}

std::size_t Manager::SubmitCandidate(std::string_view name, uint64_t height,
                                     simdjson::dom::array fields,
                                     simdjson::dom::array documents) {
  return GetTable(name).SubmitCandidate(height, fields, documents);
}

std::string Manager::Search(std::string_view name, std::string_view field,
                            std::string_view value) const {
  return GetTable(name).Search(field, value);
}

}  // namespace web3::indexer
