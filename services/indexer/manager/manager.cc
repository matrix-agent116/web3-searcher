#include "indexer/manager/manager.h"

#include <cerrno>
#include <cstdlib>
#include <duckdb/main/connection.hpp>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "indexer/storage/utils.h"

namespace web3::indexer {
namespace fs = std::filesystem;
namespace {

std::size_t CreateTable(const fs::path& directory,
                        duckdb::DatabaseInstance& database,
                        const TableSchema& schema) {
  simdjson::dom::parser parser;
  auto rows = schema.documents
                  ? parser.load((directory / *schema.documents).string())
                  : parser.parse("[]", 2);
  auto documents = rows.get_array().value();
  // Publish a complete table in one rename; retries never open a partial index.
  std::string temp = (directory / ".table.XXXXXX").string();
  if (!mkdtemp(temp.data())) {
    throw std::system_error(errno, std::generic_category(),
                            "create staging directory");
  }
  try {
    Storage::Write(temp, database, schema, documents);
    fs::rename(temp, directory / schema.name);
  } catch (...) {
    fs::remove_all(temp);
    throw;
  }
  return documents.size();
}

}  // namespace

Manager::Manager(fs::path directory) : directory_(std::move(directory)) {}

std::string Manager::EnsureTables() {
  std::size_t indexed = 0;
  std::call_once(initialized_, [&] {
    auto lock = LockDirectory(directory_);
    duckdb::Connection connection(database_);
    auto schemas = ReadSchema(directory_ / "schema.json", *connection.context);
    decltype(tables_) opened;
    for (auto& schema : schemas) {
      auto name = schema.name;
      auto path = directory_ / name;
      if (!fs::exists(path) || (fs::is_directory(path) && fs::is_empty(path))) {
        indexed += CreateTable(directory_, *database_.instance, schema);
      }
      opened.emplace(name, std::make_shared<Storage>(path, database_.instance,
                                                     std::move(schema)));
    }
    tables_.swap(opened);
  });
  return R"({"ok":true,"tables":)" + std::to_string(tables_.size()) +
         R"(,"indexed":)" + std::to_string(indexed) + "}";
}

std::vector<std::shared_ptr<Storage>> Manager::GetStorages() const {
  std::vector<std::shared_ptr<Storage>> storages;
  storages.reserve(tables_.size());
  for (const auto& [name, storage] : tables_) storages.push_back(storage);
  return storages;
}

Storage& Manager::GetStorage(std::string_view name) const {
  auto table = tables_.find(name);
  if (table == tables_.end())
    throw std::invalid_argument("unknown table: " + std::string(name));
  return *table->second;
}

absl::int128 Manager::GetBlockNumber(std::string_view name) const {
  return GetStorage(name).GetBlockNumber();
}

std::string Manager::Upsert(std::string_view name, uint64_t block_number,
                            simdjson::dom::array match_fields,
                            simdjson::dom::array documents) {
  const auto committed =
      GetStorage(name).Upsert(block_number, match_fields, documents);
  return R"({"ok":true,"upserted":)" + std::to_string(documents.size()) +
         R"(,"block_number":)" + std::to_string(committed) + "}";
}

std::string Manager::Search(std::string_view name, std::string_view field,
                            std::string_view value) const {
  return GetStorage(name).Search(field, value);
}

}  // namespace web3::indexer
