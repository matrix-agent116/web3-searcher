#ifndef WEB3_SEARCHER_SERVICES_INDEXER_TABLE_MANAGER_H_
#define WEB3_SEARCHER_SERVICES_INDEXER_TABLE_MANAGER_H_

#include <duckdb/main/database.hpp>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include "indexer/table.h"

namespace web3 {

class TableManager {
 public:
  explicit TableManager(std::filesystem::path directory);

  // Publish or open the catalog before serving queries. Later calls report
  // zero imported rows and reuse the initialized tables.
  std::string EnsureTables();
  std::string Upsert(std::string_view table, simdjson::dom::array documents);
  std::string Search(std::string_view table, std::string_view field,
                     std::string_view value) const;

 private:
  std::filesystem::path directory_;
  duckdb::DuckDB database_{nullptr};
  std::once_flag initialized_;
  std::map<std::string, Table, std::less<>> tables_;
};

}  // namespace web3

#endif  // WEB3_SEARCHER_SERVICES_INDEXER_TABLE_MANAGER_H_
